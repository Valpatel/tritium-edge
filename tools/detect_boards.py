#!/usr/bin/env python3
"""
Detect connected Waveshare ESP32-S3 boards via USB.

Lists all ESP32 serial devices and queries each for board identification.
Boards running our firmware respond to "IDENTIFY" with JSON board info.

Usage:
    python3 tools/detect_boards.py          # List all boards
    python3 tools/detect_boards.py --json   # Machine-readable output
"""

import argparse
import json
import os
import sys
import time
import glob

try:
    import serial
except ImportError:
    print("pyserial not installed. Install with: pip install pyserial")
    sys.exit(1)


# Known MAC-to-board mappings (add yours here)
KNOWN_MACS = {
    "1C:DB:D4:9C:CD:68": "ESP32-S3-Touch-AMOLED-2.41-B",
    "20:6E:F1:9A:24:E8": "ESP32-S3-Touch-LCD-3.49",
    "20:6E:F1:9A:12:00": "ESP32-S3-Touch-LCD-3.5B-C",
}


def find_esp32_devices():
    """Find all ESP32 USB serial devices via udev."""
    devices = []
    for dev_path in sorted(glob.glob("/dev/ttyACM*")):
        try:
            # Read udev properties
            import subprocess
            result = subprocess.run(
                ["udevadm", "info", "--query=property", "--name=" + dev_path],
                capture_output=True, text=True, timeout=5
            )
            props = {}
            for line in result.stdout.splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    props[k] = v

            if props.get("ID_VENDOR") == "Espressif":
                mac = props.get("ID_USB_SERIAL_SHORT", "unknown")
                devices.append({
                    "port": dev_path,
                    "mac": mac,
                    "known_as": KNOWN_MACS.get(mac),
                })
        except Exception:
            pass
    return devices


def query_firmware(port, timeout=3.0):
    """Send IDENTIFY command and parse JSON response."""
    try:
        ser = serial.Serial(port, 115200, timeout=1.0)
        time.sleep(0.1)  # Let device settle
        ser.reset_input_buffer()
        ser.write(b"IDENTIFY\n")
        ser.flush()

        deadline = time.time() + timeout
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
            if line.startswith("{"):
                try:
                    info = json.loads(line)
                    ser.close()
                    return info
                except json.JSONDecodeError:
                    pass
        ser.close()
    except (serial.SerialException, OSError) as e:
        return {"error": str(e)}
    return None


def main():
    parser = argparse.ArgumentParser(description="Detect connected ESP32-S3 boards")
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args()

    devices = find_esp32_devices()

    if not devices:
        if args.json:
            print("[]")
        else:
            print("No ESP32 devices found.")
        return

    results = []
    for dev in devices:
        info = query_firmware(dev["port"])
        entry = {**dev, "firmware": info}
        results.append(entry)

    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for r in results:
            port = r["port"]
            mac = r["mac"]
            known = r.get("known_as", "")
            fw = r.get("firmware")

            print(f"\n{port}  (MAC: {mac})")
            if known:
                print(f"  Known as: {known}")
            if fw and "error" not in fw:
                print(f"  Board:    {fw.get('board', '?')}")
                print(f"  Display:  {fw.get('display', '?')} via {fw.get('interface', '?')}")
                print(f"  App:      {fw.get('app', '?')}")
            elif fw and "error" in fw:
                print(f"  Error:    {fw['error']}")
            else:
                print("  Firmware: No response (not running our firmware, or in bootloader)")


if __name__ == "__main__":
    main()
