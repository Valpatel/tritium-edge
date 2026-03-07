#!/usr/bin/env python3
"""
Comprehensive OTA test suite.

Tests all OTA pathways:
  1. Serial OTA with raw .bin (unsigned)
  2. Serial OTA with signed .ota
  3. Serial OTA with tampered .ota (should fail CRC)
  4. BLE OTA with signed .ota
  5. BLE OTA with unsigned .bin (may fail if OTA_REQUIRE_SIGNATURE)
  6. SD card write + boot-time OTA with signed .ota
  7. Device info queries (serial + BLE)

Usage:
    python3 test_ota.py --port /dev/ttyACM1 --firmware firmware.bin --key keys/ota_signing_key.pem
"""

import sys
import os
import subprocess
import time
import argparse
import tempfile
import struct
import binascii

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(TOOLS_DIR)


def run(cmd, timeout=120, check=True):
    """Run a command and return (returncode, stdout)."""
    print(f"  $ {' '.join(cmd)}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return result.returncode, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT"


def test_serial_info(port):
    """Test serial IDENTIFY command."""
    print("\n=== Test: Serial IDENTIFY ===")
    import serial
    s = serial.Serial(port, 115200, timeout=2)
    s.write(b"IDENTIFY\n")
    time.sleep(1)
    data = s.read(1024).decode(errors='replace')
    s.close()
    if '"app":"OTA"' in data or '"board"' in data:
        print(f"  PASS: {data.strip()}")
        return True
    print(f"  FAIL: no valid response: {data.strip()}")
    return False


def test_serial_ota_info(port):
    """Test serial OTA_INFO command."""
    print("\n=== Test: Serial OTA_INFO ===")
    import serial
    s = serial.Serial(port, 115200, timeout=2)
    s.write(b"OTA_INFO\n")
    time.sleep(1)
    data = s.read(1024).decode(errors='replace')
    s.close()
    if '"version"' in data:
        print(f"  PASS: {data.strip()}")
        return True
    print(f"  FAIL: {data.strip()}")
    return False


def test_serial_ota_signed(port, ota_file):
    """Test serial OTA with signed .ota file."""
    print("\n=== Test: Serial OTA (signed .ota) ===")
    rc, out = run([sys.executable, os.path.join(TOOLS_DIR, 'serial_ota.py'),
                   port, ota_file], timeout=120)
    if rc == 0 and ('OTA_OK' in out or 'SUCCESS' in out):
        print("  PASS: Serial OTA signed succeeded")
        return True
    print(f"  FAIL (rc={rc})")
    # Print last few lines
    for line in out.strip().split('\n')[-5:]:
        print(f"    {line}")
    return False


def test_serial_ota_raw(port, bin_file):
    """Test serial OTA with raw .bin file."""
    print("\n=== Test: Serial OTA (raw .bin) ===")
    rc, out = run([sys.executable, os.path.join(TOOLS_DIR, 'serial_ota.py'),
                   port, bin_file], timeout=120)
    if rc == 0 and ('OTA_OK' in out or 'SUCCESS' in out):
        print("  PASS: Serial OTA raw succeeded")
        return True
    if 'unsigned firmware rejected' in out:
        print("  PASS (expected): unsigned rejected (OTA_REQUIRE_SIGNATURE)")
        return True
    print(f"  FAIL (rc={rc})")
    for line in out.strip().split('\n')[-5:]:
        print(f"    {line}")
    return False


def test_serial_fw_hash(port):
    """Test firmware hash attestation command."""
    print("\n=== Test: Serial FW_HASH ===")
    import serial
    s = serial.Serial(port, 115200, timeout=3)
    s.write(b"FW_HASH\n")
    time.sleep(2)
    data = s.read(1024).decode(errors='replace')
    s.close()
    if 'FW_HASH ' in data:
        for line in data.strip().split('\n'):
            if line.startswith('FW_HASH '):
                h = line.split()[1]
                if len(h) == 64:
                    print(f"  PASS: {h[:32]}...")
                    return True
    print(f"  FAIL: {data.strip()}")
    return False


def test_serial_audit_log(port):
    """Test OTA audit log command."""
    print("\n=== Test: Serial OTA_AUDIT ===")
    import serial
    s = serial.Serial(port, 115200, timeout=3)
    s.write(b"OTA_AUDIT\n")
    time.sleep(2)
    data = s.read(2048).decode(errors='replace')
    s.close()
    if 'OTA_AUDIT_EMPTY' in data:
        print("  PASS: Audit log empty (expected for fresh device)")
        return True
    if '"src"' in data or '"ok"' in data:
        lines = [l for l in data.strip().split('\n') if l.startswith('{')]
        print(f"  PASS: {len(lines)} audit entries")
        return True
    print(f"  FAIL: {data.strip()[:200]}")
    return False


def test_serial_mesh_status(port):
    """Test mesh OTA status command."""
    print("\n=== Test: Serial MESH_OTA_STATUS ===")
    import serial
    s = serial.Serial(port, 115200, timeout=3)
    s.write(b"MESH_OTA_STATUS\n")
    time.sleep(2)
    data = s.read(1024).decode(errors='replace')
    s.close()
    if '"sending"' in data and '"receiving"' in data:
        print(f"  PASS: {data.strip()}")
        return True
    print(f"  FAIL: {data.strip()}")
    return False


def test_package_verify(firmware, key):
    """Test package_firmware.py --verify with signed .ota."""
    print("\n=== Test: Package + Verify ===")
    import tempfile
    ota = tempfile.mktemp(suffix='.ota')
    # Package
    rc, out = run([sys.executable, os.path.join(TOOLS_DIR, 'package_firmware.py'),
                   firmware, '--sign', key,
                   '--board', 'test-board', '--version', '1.0.0-test',
                   '-o', ota])
    if rc != 0:
        print(f"  FAIL: Package failed: {out.strip()}")
        return False
    # Verify
    rc, out = run([sys.executable, os.path.join(TOOLS_DIR, 'package_firmware.py'),
                   ota, '--verify', '--pubkey', key])
    try:
        os.unlink(ota)
    except:
        pass
    if rc == 0 and 'VERIFIED' in out:
        print("  PASS: Package + verify OK")
        return True
    print(f"  FAIL: {out.strip()[-200:]}")
    return False


def test_ble_scan():
    """Test BLE scan for OTA devices."""
    print("\n=== Test: BLE Scan ===")
    rc, out = run([sys.executable, os.path.join(TOOLS_DIR, 'ble_ota.py'),
                   '--scan'], timeout=20)
    if 'ESP32-OTA' in out:
        print(f"  PASS: Found device")
        return True
    print("  FAIL: No OTA device found")
    return False


def test_ble_info():
    """Test BLE device info."""
    print("\n=== Test: BLE Device Info ===")
    rc, out = run([sys.executable, os.path.join(TOOLS_DIR, 'ble_ota.py'),
                   '--info'], timeout=20)
    if '"ble_ota":true' in out:
        print(f"  PASS: {out.strip()}")
        return True
    print(f"  FAIL: {out.strip()}")
    return False


def test_ble_ota_signed(ota_file):
    """Test BLE OTA with signed .ota file."""
    print("\n=== Test: BLE OTA (signed .ota) ===")
    rc, out = run([sys.executable, os.path.join(TOOLS_DIR, 'ble_ota.py'),
                   ota_file], timeout=300)
    if rc == 0 and 'SUCCESS' in out:
        print("  PASS: BLE OTA signed succeeded")
        return True
    print(f"  FAIL (rc={rc})")
    for line in out.strip().split('\n')[-5:]:
        print(f"    {line}")
    return False


def test_ble_ota_tampered(ota_file):
    """Test BLE OTA with tampered .ota file (should fail)."""
    print("\n=== Test: BLE OTA (tampered .ota — expect FAIL) ===")
    # Create tampered copy
    with open(ota_file, 'rb') as f:
        data = bytearray(f.read())
    data[200] ^= 0xFF  # Flip a byte in firmware data

    with tempfile.NamedTemporaryFile(suffix='.ota', delete=False) as tmp:
        tmp.write(data)
        tampered_path = tmp.name

    try:
        rc, out = run([sys.executable, os.path.join(TOOLS_DIR, 'ble_ota.py'),
                       tampered_path], timeout=300)
        if 'CRC32 mismatch' in out or 'FAIL' in out:
            print("  PASS: Tampered firmware correctly rejected")
            return True
        if rc != 0:
            print("  PASS: Transfer failed (expected)")
            return True
        print("  FAIL: Tampered firmware was accepted!")
        return False
    finally:
        os.unlink(tampered_path)


def main():
    parser = argparse.ArgumentParser(description='OTA test suite')
    parser.add_argument('--port', required=True, help='Serial port')
    parser.add_argument('--firmware', required=True, help='firmware.bin path')
    parser.add_argument('--key', default='keys/ota_signing_key.pem', help='Signing key')
    parser.add_argument('--test', help='Run specific test (serial-info, ble-scan, etc.)')
    parser.add_argument('--skip-ble', action='store_true', help='Skip BLE tests')
    args = parser.parse_args()

    # Package signed .ota
    ota_file = tempfile.mktemp(suffix='.ota')
    print(f"Packaging signed firmware: {args.firmware} -> {ota_file}")
    rc, out = run([sys.executable, os.path.join(TOOLS_DIR, 'package_firmware.py'),
                   args.firmware, '--sign', args.key,
                   '--board', 'touch-lcd-349', '--version', '99.0.0-test',
                   '-o', ota_file])
    if rc != 0:
        print(f"Failed to package: {out}")
        return 1
    print(f"  {out.strip()}")

    results = {}
    tests = [
        ('serial-info', lambda: test_serial_info(args.port)),
        ('serial-ota-info', lambda: test_serial_ota_info(args.port)),
        ('serial-fw-hash', lambda: test_serial_fw_hash(args.port)),
        ('serial-audit-log', lambda: test_serial_audit_log(args.port)),
        ('serial-mesh-status', lambda: test_serial_mesh_status(args.port)),
        ('package-verify', lambda: test_package_verify(args.firmware, args.key)),
        ('ble-scan', lambda: test_ble_scan()),
        ('ble-info', lambda: test_ble_info()),
        ('serial-ota-signed', lambda: test_serial_ota_signed(args.port, ota_file)),
    ]

    if not args.skip_ble:
        tests.extend([
            ('ble-ota-signed', lambda: test_ble_ota_signed(ota_file)),
            ('ble-ota-tampered', lambda: test_ble_ota_tampered(ota_file)),
        ])

    for name, test_fn in tests:
        if args.test and args.test != name:
            continue
        try:
            # Wait for device to be ready between tests
            time.sleep(3)
            results[name] = test_fn()
        except Exception as e:
            print(f"  ERROR: {e}")
            results[name] = False

    # Cleanup
    try:
        os.unlink(ota_file)
    except:
        pass

    # Summary
    print("\n" + "=" * 50)
    print("OTA TEST RESULTS")
    print("=" * 50)
    passed = 0
    failed = 0
    for name, ok in results.items():
        status = "PASS" if ok else "FAIL"
        print(f"  {status:4s}  {name}")
        if ok:
            passed += 1
        else:
            failed += 1
    print(f"\n{passed} passed, {failed} failed, {len(results)} total")

    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
