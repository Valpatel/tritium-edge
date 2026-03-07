#!/usr/bin/env python3
"""
Serial OTA firmware uploader for ESP32 boards running the OTA app.

Usage:
    # Upload raw firmware.bin
    python3 serial_ota.py /dev/ttyACM0 firmware.bin

    # Upload signed .ota package (auto-detected, sends signature for verification)
    python3 serial_ota.py /dev/ttyACM0 firmware.ota

    # Write to SD card
    python3 serial_ota.py /dev/ttyACM0 firmware.ota --sd

Protocol:
    PC -> Device: OTA_BEGIN <size> <crc32>\n
    Device -> PC: OTA_READY\n
    PC -> Device: OTA_SIG <r_hex> <s_hex>\n   (if signed .ota)
    Device -> PC: OTA_SIG_OK\n
    PC -> Device: <raw binary data in chunks>
    Device -> PC: OTA_NEXT <offset>\n  (every 4KB)
    Device -> PC: OTA_OK\n  (success, device reboots)
    Device -> PC: OTA_FAIL <reason>\n  (failure)
"""

import sys
import time
import struct
import serial
import argparse


MAGIC = 0x4154304F
HEADER_SIZE = 64
SIGNATURE_SIZE = 64
HEADER_VER_SIGNED = 2
FLAG_SIGNED = 0x01


def crc32(data: bytes) -> int:
    """Compute CRC32 matching the ESP32 implementation."""
    import binascii
    return binascii.crc32(data) & 0xFFFFFFFF


def parse_ota_file(path: str):
    """Parse .ota file, return (firmware_data, crc32, signature_r, signature_s, version, board)."""
    with open(path, 'rb') as f:
        all_data = f.read()

    if len(all_data) < HEADER_SIZE:
        return None

    magic, hdr_ver, flags, fw_size, fw_crc = struct.unpack('<IHHII', all_data[:16])
    if magic != MAGIC:
        return None

    version = all_data[16:40].split(b'\x00')[0].decode()
    board = all_data[40:56].split(b'\x00')[0].decode()

    is_signed = (hdr_ver == HEADER_VER_SIGNED) and (flags & FLAG_SIGNED)
    total_header = HEADER_SIZE + (SIGNATURE_SIZE if is_signed else 0)

    fw_data = all_data[total_header:]
    sig_r = sig_s = None
    if is_signed:
        sig_r = all_data[HEADER_SIZE:HEADER_SIZE+32]
        sig_s = all_data[HEADER_SIZE+32:HEADER_SIZE+64]

    return {
        'firmware': fw_data,
        'crc32': fw_crc,
        'signed': is_signed,
        'sig_r': sig_r,
        'sig_s': sig_s,
        'version': version,
        'board': board,
    }


def serial_ota(port: str, firmware_path: str, baud: int = 115200):
    # Try parsing as .ota package first
    ota_info = parse_ota_file(firmware_path)

    if ota_info and ota_info['firmware']:
        fw_data = ota_info['firmware']
        fw_crc = ota_info['crc32']
        is_signed = ota_info['signed']
        print(f"OTA Package: {firmware_path}")
        print(f"  Version: {ota_info['version']}, Board: {ota_info['board']}")
        print(f"  Signed: {'ECDSA P-256' if is_signed else 'no'}")
    else:
        # Raw .bin file
        with open(firmware_path, 'rb') as f:
            fw_data = f.read()
        fw_crc = crc32(fw_data)
        is_signed = False
        ota_info = None
        print(f"Raw firmware: {firmware_path}")

    fw_size = len(fw_data)
    print(f"Size: {fw_size} bytes ({fw_size / 1024:.1f} KB)")
    print(f"CRC32: 0x{fw_crc:08X}")
    print(f"Port: {port}")
    print()

    # Open serial
    ser = serial.Serial(port, baud, timeout=5)
    time.sleep(0.5)
    ser.reset_input_buffer()

    # Query device info
    ser.write(b'OTA_INFO\n')
    time.sleep(0.5)
    while ser.in_waiting:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if line.startswith('{'):
            print(f"Device: {line}")
        elif 'OTA' in line or 'ota' in line:
            print(f"  {line}")

    # Send signature BEFORE OTA_BEGIN (device must be in command mode)
    if is_signed and ota_info:
        r_hex = ota_info['sig_r'].hex()
        s_hex = ota_info['sig_s'].hex()
        sig_cmd = f'OTA_SIG {r_hex} {s_hex}\n'.encode()
        print(f"\nSending signature ({len(sig_cmd)} bytes)...")
        ser.write(sig_cmd)

        sig_deadline = time.time() + 3
        sig_ok = False
        while time.time() < sig_deadline:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if 'OTA_SIG_OK' in line:
                    sig_ok = True
                    print("Device: OTA_SIG_OK (signature will be verified after transfer)")
                    break
                elif 'OTA_SIG_FAIL' in line:
                    print(f"Device rejected signature: {line}")
                    ser.close()
                    return False
                elif line:
                    print(f"  {line}")
        if not sig_ok:
            print("WARNING: Device did not acknowledge signature, continuing anyway")

    # Send OTA_BEGIN (device enters RECEIVING mode after this)
    cmd = f'OTA_BEGIN {fw_size} {fw_crc}\n'.encode()
    print(f"Sending: {cmd.decode().strip()}")
    ser.write(cmd)

    # Wait for OTA_READY
    deadline = time.time() + 5
    ready = False
    while time.time() < deadline:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if 'OTA_READY' in line:
                ready = True
                print("Device: OTA_READY")
                break
            elif 'OTA_FAIL' in line:
                print(f"Device rejected: {line}")
                ser.close()
                return False
            elif line:
                print(f"  {line}")

    if not ready:
        print("ERROR: Device did not respond with OTA_READY")
        ser.close()
        return False

    # Send firmware data in flow-controlled chunks
    flow_chunk = 4096  # Flow control boundary (match flash page)
    usb_piece = 64     # USB CDC max packet size
    sent = 0
    start_time = time.time()

    while sent < fw_size:
        # Send one flow-control chunk in small USB pieces
        chunk_end = min(sent + flow_chunk, fw_size)
        while sent < chunk_end:
            piece_end = min(sent + usb_piece, chunk_end)
            piece = fw_data[sent:piece_end]
            ser.write(piece)
            sent += len(piece)
            time.sleep(0.001)  # Let USB CDC process

        # Print progress
        pct = sent * 100 // fw_size
        elapsed = time.time() - start_time
        speed = sent / elapsed / 1024 if elapsed > 0 else 0
        print(f"\r  Progress: {pct}% ({sent}/{fw_size}) {speed:.1f} KB/s", end='', flush=True)

        # Wait for flow control ACK from device
        ack_deadline = time.time() + 10
        got_ack = False
        while time.time() < ack_deadline:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if 'OTA_NEXT' in line:
                    got_ack = True
                    break
                elif 'OTA_FAIL' in line:
                    print(f"\nDevice error: {line}")
                    ser.close()
                    return False
                elif 'OTA_OK' in line:
                    print(f"\n  Sent {sent} bytes in {elapsed:.1f}s")
                    print("SUCCESS! Device will reboot.")
                    ser.close()
                    return True
            time.sleep(0.001)

        if not got_ack:
            print(f"\nTimeout waiting for device ACK at offset {sent}")
            ser.close()
            return False

    print(f"\n  Sent {sent} bytes in {time.time() - start_time:.1f}s")

    # Wait for OTA_OK or OTA_FAIL
    print("Waiting for verification...")
    deadline = time.time() + 30  # Longer timeout for signature verification
    while time.time() < deadline:
        try:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if 'OTA_OK' in line:
                    print("SUCCESS! Device will reboot.")
                    ser.close()
                    return True
                elif 'OTA_FAIL' in line:
                    print(f"FAILED: {line}")
                    ser.close()
                    return False
                elif line:
                    print(f"  {line}")
        except (serial.SerialException, OSError):
            # Device rebooted and USB reconnected — success indicator
            print("Device disconnected (likely rebooting after successful OTA)")
            try:
                ser.close()
            except Exception:
                pass
            print("Waiting for device to reboot...")
            time.sleep(5)
            try:
                ser2 = serial.Serial(port, baud, timeout=3)
                time.sleep(1)
                ser2.reset_input_buffer()
                ser2.write(b'IDENTIFY\n')
                time.sleep(1)
                while ser2.in_waiting:
                    line = ser2.readline().decode('utf-8', errors='replace').strip()
                    if line:
                        print(f"  Rebooted: {line}")
                ser2.close()
            except Exception as e:
                print(f"  Could not reconnect: {e}")
            return True
        time.sleep(0.1)

    print("TIMEOUT waiting for device response")
    print("Note: device may have written firmware but verification may have failed.")
    print("Check device serial output for details.")
    ser.close()
    return False


def serial_sd_write(port: str, firmware_path: str, baud: int = 115200, sd_filename: str = None):
    """Write firmware to device's SD card for boot-time OTA."""
    with open(firmware_path, 'rb') as f:
        fw_data = f.read()

    fw_size = len(fw_data)

    # Auto-detect SD filename from extension
    if sd_filename is None:
        if firmware_path.endswith('.ota'):
            sd_filename = '/firmware.ota'
        else:
            sd_filename = '/firmware.bin'

    print(f"Writing to SD card: {firmware_path} -> {sd_filename} ({fw_size} bytes)")
    print(f"Port: {port}")

    ser = serial.Serial(port, baud, timeout=5)
    time.sleep(0.5)
    ser.reset_input_buffer()

    # Send SD write command with filename
    cmd = f'OTA_SD_WRITE {fw_size} {sd_filename}\n'.encode()
    print(f"Sending: {cmd.decode().strip()}")
    ser.write(cmd)

    # Wait for READY
    deadline = time.time() + 5
    ready = False
    while time.time() < deadline:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if 'OTA_READY' in line:
                ready = True
                break
            elif 'OTA_FAIL' in line:
                print(f"Device error: {line}")
                ser.close()
                return False

    if not ready:
        print("ERROR: Device not ready")
        ser.close()
        return False

    # Send data in flow-controlled chunks
    flow_chunk = 4096
    usb_piece = 64
    sent = 0
    start_time = time.time()

    while sent < fw_size:
        chunk_end = min(sent + flow_chunk, fw_size)
        while sent < chunk_end:
            piece_end = min(sent + usb_piece, chunk_end)
            ser.write(fw_data[sent:piece_end])
            sent += piece_end - sent
            time.sleep(0.001)

        # Wait for ACK
        ack_deadline = time.time() + 15
        got_ack = False
        while time.time() < ack_deadline:
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if 'OTA_NEXT' in line:
                    got_ack = True
                    break
                elif 'OTA_SD_WRITE_OK' in line:
                    elapsed = time.time() - start_time
                    print(f"\n  SD write complete: {sent} bytes in {elapsed:.1f}s")
                    ser.close()
                    return True
                elif 'OTA_FAIL' in line:
                    print(f"\nDevice error: {line}")
                    ser.close()
                    return False
            time.sleep(0.001)

        if not got_ack:
            print(f"\nTimeout at {sent}")
            ser.close()
            return False

        pct = sent * 100 // fw_size
        elapsed = time.time() - start_time
        speed = sent / elapsed / 1024 if elapsed > 0 else 0
        print(f"\r  Progress: {pct}% ({sent}/{fw_size}) {speed:.1f} KB/s", end='', flush=True)

    # Wait for final OK
    deadline = time.time() + 10
    while time.time() < deadline:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if 'OTA_SD_WRITE_OK' in line:
                elapsed = time.time() - start_time
                print(f"\n  SD write complete: {sent} bytes in {elapsed:.1f}s")
                ser.close()
                return True
            elif 'OTA_FAIL' in line:
                print(f"\nDevice error: {line}")
                ser.close()
                return False
        time.sleep(0.1)

    ser.close()
    return False


def main():
    parser = argparse.ArgumentParser(description='Serial OTA firmware uploader')
    parser.add_argument('port', help='Serial port (e.g., /dev/ttyACM0)')
    parser.add_argument('firmware', help='Path to firmware.bin or firmware.ota')
    parser.add_argument('-b', '--baud', type=int, default=115200, help='Baud rate')
    parser.add_argument('--sd', action='store_true',
                        help='Write firmware to SD card instead of flashing directly')
    args = parser.parse_args()

    if args.sd:
        success = serial_sd_write(args.port, args.firmware, args.baud)
    else:
        success = serial_ota(args.port, args.firmware, args.baud)
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
