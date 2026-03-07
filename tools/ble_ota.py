#!/usr/bin/env python3
"""
BLE OTA firmware uploader for ESP32 boards running the OTA app.

Requires: bleak (pip install bleak)

Usage:
    python3 ble_ota.py firmware.bin                    # Scan and upload
    python3 ble_ota.py firmware.ota                    # Upload signed package
    python3 ble_ota.py firmware.bin --name ESP32-OTA   # Connect by name
    python3 ble_ota.py firmware.bin --addr AA:BB:CC:DD:EE:FF  # Connect by address
"""

import sys
import asyncio
import struct
import argparse
import binascii
import time

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("ERROR: 'bleak' not installed. Run: pip install bleak")
    sys.exit(1)

# UUIDs must match hal_ble_ota.h
SERVICE_UUID = "fb1e4001-54ae-4a28-9f74-dfccb248601d"
CTRL_UUID    = "fb1e4002-54ae-4a28-9f74-dfccb248601d"
DATA_UUID    = "fb1e4003-54ae-4a28-9f74-dfccb248601d"

# OTA header constants
MAGIC = 0x4154304F
HEADER_SIZE = 64
SIGNATURE_SIZE = 64
HEADER_VER_SIGNED = 2
FLAG_SIGNED = 0x01

# Protocol commands
CMD_BEGIN = 0x01
CMD_ABORT = 0x02
CMD_INFO  = 0x03
CMD_SIG   = 0x04

RESP_READY    = 0x10
RESP_PROGRESS = 0x11
RESP_OK       = 0x12
RESP_FAIL     = 0x13
RESP_INFO     = 0x14


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def parse_ota_file(path: str):
    """Parse .ota file, return dict with firmware, crc, signature info."""
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

    return {
        'firmware': all_data[total_header:],
        'crc32': fw_crc,
        'signed': is_signed,
        'sig_r': all_data[HEADER_SIZE:HEADER_SIZE+32] if is_signed else None,
        'sig_s': all_data[HEADER_SIZE+32:HEADER_SIZE+64] if is_signed else None,
        'version': version,
        'board': board,
    }


async def scan_for_device(name_filter: str = None, timeout: float = 10.0):
    """Scan for ESP32-OTA BLE devices."""
    print(f"Scanning for BLE devices ({timeout}s)...")
    results = await BleakScanner.discover(timeout=timeout, return_adv=True)

    ota_devices = []
    for addr, (dev, adv) in results.items():
        if dev.name and 'OTA' in dev.name.upper():
            ota_devices.append((dev, adv))
        elif name_filter and dev.name and name_filter.lower() in dev.name.lower():
            ota_devices.append((dev, adv))

    return ota_devices


async def ble_ota(address: str, firmware_path: str, name: str = None):
    # Parse firmware
    ota_info = parse_ota_file(firmware_path)
    if ota_info and ota_info['firmware']:
        fw_data = ota_info['firmware']
        fw_crc = ota_info['crc32']
        is_signed = ota_info['signed']
        print(f"OTA Package: {firmware_path}")
        print(f"  Version: {ota_info['version']}, Board: {ota_info['board']}")
        print(f"  Signed: {'ECDSA P-256' if is_signed else 'no'}")
    else:
        with open(firmware_path, 'rb') as f:
            fw_data = f.read()
        fw_crc = crc32(fw_data)
        is_signed = False
        ota_info = None
        print(f"Raw firmware: {firmware_path}")

    fw_size = len(fw_data)
    print(f"Size: {fw_size} bytes ({fw_size / 1024:.1f} KB)")
    print(f"CRC32: 0x{fw_crc:08X}")
    print()

    # Find device if no address given
    if not address:
        devices = await scan_for_device(name)
        if not devices:
            print("No OTA devices found. Make sure the device is running the OTA app.")
            return False
        if len(devices) == 1:
            dev, adv = devices[0]
            address = dev.address
            print(f"Found: {dev.name} ({address}) RSSI={adv.rssi}")
        else:
            print("Multiple devices found:")
            for i, (dev, adv) in enumerate(devices):
                print(f"  [{i}] {dev.name} ({dev.address}) RSSI={adv.rssi}")
            choice = input("Select device [0]: ").strip()
            idx = int(choice) if choice else 0
            address = devices[idx][0].address

    print(f"\nConnecting to {address}...")

    # Notification handler
    last_resp = [None]
    resp_event = asyncio.Event()

    def notification_handler(sender, data):
        if len(data) < 1:
            return
        resp_type = data[0]
        last_resp[0] = data
        if resp_type == RESP_PROGRESS:
            if len(data) >= 2:
                pct = data[1]
                print(f"\r  Progress: {pct}%", end='', flush=True)
        elif resp_type == RESP_READY:
            resp_event.set()
        elif resp_type == RESP_OK:
            print(f"\n  OTA complete!")
            resp_event.set()
        elif resp_type == RESP_FAIL:
            reason = data[1:].decode('utf-8', errors='replace')
            print(f"\n  FAIL: {reason}")
            resp_event.set()
        elif resp_type == RESP_INFO:
            info = data[1:].decode('utf-8', errors='replace')
            print(f"  Device info: {info}")
            resp_event.set()

    async with BleakClient(address, timeout=15.0) as client:
        # Trigger MTU negotiation (BlueZ requires explicit acquire)
        if hasattr(client._backend, '_acquire_mtu'):
            await client._backend._acquire_mtu()
        print(f"Connected! MTU={client.mtu_size}")

        # Subscribe to notifications
        await client.start_notify(CTRL_UUID, notification_handler)

        # Request device info
        await client.write_gatt_char(CTRL_UUID, bytes([CMD_INFO]))
        try:
            await asyncio.wait_for(resp_event.wait(), timeout=3.0)
        except asyncio.TimeoutError:
            print("  (no info response)")
        resp_event.clear()

        # Send BEGIN command
        begin_data = bytes([CMD_BEGIN]) + struct.pack('<II', fw_size, fw_crc)
        print(f"Sending BEGIN ({fw_size} bytes, crc=0x{fw_crc:08X})...")
        await client.write_gatt_char(CTRL_UUID, begin_data)

        await asyncio.wait_for(resp_event.wait(), timeout=5.0)
        resp_event.clear()

        if last_resp[0] and last_resp[0][0] != RESP_READY:
            print("Device not ready!")
            return False

        print("Device: READY")

        # Send signature if signed
        if is_signed and ota_info:
            sig_data = bytes([CMD_SIG]) + ota_info['sig_r'] + ota_info['sig_s']
            await client.write_gatt_char(CTRL_UUID, sig_data)
            await asyncio.wait_for(resp_event.wait(), timeout=3.0)
            resp_event.clear()
            print("Signature sent")

        # Send firmware data
        # Use MTU - 3 as chunk size (ATT header is 3 bytes)
        chunk_size = client.mtu_size - 3
        if chunk_size < 20:
            chunk_size = 20
        if chunk_size > 512:
            chunk_size = 512

        # Flow control: use write-without-response with pacing delay.
        # The ESP32 NimBLE stack can buffer ~4-8 packets before needing time to flush.
        # A small delay every few chunks prevents buffer overflow disconnects.
        batch_size = 4  # chunks before yielding
        batch_delay = 0.02  # seconds between batches

        print(f"Sending firmware ({chunk_size}-byte chunks, pace={batch_size}x{batch_delay}s)...")
        sent = 0
        chunk_num = 0
        start_time = time.time()

        while sent < fw_size:
            end = min(sent + chunk_size, fw_size)
            chunk = fw_data[sent:end]
            chunk_num += 1

            await client.write_gatt_char(DATA_UUID, chunk, response=False)
            sent += len(chunk)

            # Pace: yield after every batch to let BLE stack flush
            if chunk_num % batch_size == 0:
                await asyncio.sleep(batch_delay)

        elapsed = time.time() - start_time
        speed = sent / elapsed / 1024 if elapsed > 0 else 0
        print(f"\n  Sent {sent} bytes in {elapsed:.1f}s ({speed:.1f} KB/s)")

        # Wait for OK/FAIL. The ESP32 may still be processing buffered BLE data,
        # then needs to verify CRC32 + ECDSA before responding.
        print("Waiting for device verification (CRC32 + ECDSA)...")
        deadline = time.time() + 120.0  # generous timeout for processing + verification
        while time.time() < deadline:
            if last_resp[0] and last_resp[0][0] == RESP_OK:
                print("\nSUCCESS! Device will reboot.")
                return True
            if last_resp[0] and last_resp[0][0] == RESP_FAIL:
                reason = last_resp[0][1:].decode('utf-8', errors='replace')
                print(f"\nFAIL: {reason}")
                return False
            resp_event.clear()
            try:
                await asyncio.wait_for(resp_event.wait(), timeout=2.0)
            except asyncio.TimeoutError:
                pass

        print("\nTIMEOUT waiting for device response")
        return False


async def scan_only(name_filter: str):
    """Just scan and list OTA devices."""
    devices = await scan_for_device(name_filter, timeout=10.0)
    if not devices:
        print("No OTA devices found.")
        return
    for dev, adv in devices:
        print(f"  {dev.name:20s} {dev.address}  RSSI={adv.rssi}")


async def info_only(address: str, name: str):
    """Connect and get device info."""
    if not address:
        devices = await scan_for_device(name)
        if not devices:
            print("No OTA devices found.")
            return
        address = devices[0][0].address

    resp_event = asyncio.Event()
    info_data = [None]

    def on_notify(sender, data):
        if data[0] == RESP_INFO:
            info_data[0] = data[1:].decode('utf-8', errors='replace')
            resp_event.set()

    async with BleakClient(address, timeout=10.0) as client:
        if hasattr(client._backend, '_acquire_mtu'):
            await client._backend._acquire_mtu()
        await client.start_notify(CTRL_UUID, on_notify)
        await client.write_gatt_char(CTRL_UUID, bytes([CMD_INFO]))
        try:
            await asyncio.wait_for(resp_event.wait(), timeout=3.0)
            print(f"Device: {address}")
            print(f"MTU: {client.mtu_size}")
            print(f"Info: {info_data[0]}")
        except asyncio.TimeoutError:
            print("No info response")


def main():
    parser = argparse.ArgumentParser(description='BLE OTA firmware uploader')
    parser.add_argument('firmware', nargs='?', help='Path to firmware.bin or firmware.ota')
    parser.add_argument('--addr', help='BLE device address (AA:BB:CC:DD:EE:FF)')
    parser.add_argument('--name', default='ESP32-OTA', help='BLE device name to scan for')
    parser.add_argument('--scan', action='store_true', help='Just scan for OTA devices')
    parser.add_argument('--info', action='store_true', help='Connect and get device info')
    args = parser.parse_args()

    if args.scan:
        asyncio.run(scan_only(args.name))
        return

    if args.info:
        asyncio.run(info_only(args.addr, args.name))
        return

    if not args.firmware:
        parser.error("firmware path is required (unless using --scan or --info)")

    success = asyncio.run(ble_ota(args.addr, args.firmware, args.name))
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
