#!/usr/bin/env python3
"""
First-time ESP32 device provisioning tool.

Handles the complete setup workflow for a new ESP32:
  1. Flash the OTA firmware via esptool
  2. Wait for device to boot and respond to IDENTIFY
  3. Push provisioning data (certs, device identity) over serial
  4. Verify device is provisioned and connecting to server
  5. Register device with fleet server

Usage:
    # Provision a new device with certs from fleet server
    python provision_device.py --port /dev/ttyACM0 --device-id esp32-abc123 \\
        --server http://fleet-server:8080

    # Provision with local certs directory
    python provision_device.py --port /dev/ttyACM0 --certs ./certs \\
        --device-name "kitchen-display" --server-url https://api.example.com

    # Flash + provision in one step
    python provision_device.py --port /dev/ttyACM0 --flash touch-lcd-349 \\
        --server http://fleet-server:8080

    # Just flash (no provisioning)
    python provision_device.py --port /dev/ttyACM0 --flash touch-lcd-349 --flash-only
"""

import argparse
import json
import os
import subprocess
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(TOOLS_DIR)


def flash_firmware(port: str, board: str, app: str = "ota"):
    """Flash OTA firmware to the device using PlatformIO."""
    env = f"{board}-{app}" if app else board
    print(f"Flashing firmware: env={env}, port={port}")

    cmd = [
        "pio", "run", "-e", env,
        "-t", "upload",
        "--upload-port", port,
    ]
    result = subprocess.run(cmd, cwd=PROJECT_DIR, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Flash failed:\n{result.stderr[-500:]}")
        return False
    print("Flash OK")
    return True


def wait_for_device(port: str, baud: int = 115200, timeout: float = 30.0):
    """Wait for device to boot and respond to IDENTIFY."""
    import serial

    deadline = time.time() + timeout
    print(f"Waiting for device on {port}...")

    while time.time() < deadline:
        try:
            s = serial.Serial(port, baud, timeout=2)
            time.sleep(1)
            s.reset_input_buffer()
            s.write(b"IDENTIFY\n")
            time.sleep(1)
            data = s.read(1024).decode(errors='replace')
            s.close()

            if '"app"' in data or '"board"' in data:
                # Extract the JSON line
                for line in data.split('\n'):
                    line = line.strip()
                    if line.startswith('{') and '"board"' in line:
                        print(f"Device responded: {line}")
                        return json.loads(line)
                return {"raw": data.strip()}
        except Exception as e:
            time.sleep(2)

    print("Timeout waiting for device")
    return None


def send_provision_data(port: str, device_json: dict, certs_dir: str = None,
                        baud: int = 115200):
    """Send provisioning data to device over serial USB.

    Protocol (matches hal_provision.h USB provisioning):
        PC -> Device: PROVISION_BEGIN\n
        Device -> PC: {"status":"ready","msg":"Send provisioning JSON"}\n
        PC -> Device: {"cmd":"provision","device_id":"...","ca_pem":"...","client_crt":"...","client_key":"..."}\n
        Device -> PC: {"status":"ok","msg":"Provisioned"}\n
    """
    import serial

    s = serial.Serial(port, baud, timeout=5)
    time.sleep(0.5)
    s.reset_input_buffer()

    print("Starting provisioning...")

    # Send PROVISION_BEGIN to trigger the HAL's startUSBProvision()
    s.write(b"PROVISION_BEGIN\n")
    s.flush()

    # Wait for ready
    deadline = time.time() + 5
    ready = False
    while time.time() < deadline:
        if s.in_waiting:
            line = s.readline().decode('utf-8', errors='replace').strip()
            if 'PROVISION_READY' in line or '"ready"' in line:
                ready = True
                print(f"  Device ready: {line}")
                break
            elif line:
                print(f"  device: {line}")
    if not ready:
        print("Device not ready for provisioning")
        s.close()
        return False

    # Build the provisioning JSON blob (matches HAL's _processUSBCommand)
    prov = {
        "cmd": "provision",
        "device_id": device_json.get("device_id", ""),
        "device_name": device_json.get("device_name", ""),
        "server_url": device_json.get("server_url", ""),
        "mqtt_broker": device_json.get("mqtt_broker", ""),
        "mqtt_port": device_json.get("mqtt_port", 8883),
    }

    # Load certs
    if certs_dir:
        cert_files = {
            "ca_pem": ["ca.pem", "ca.crt"],
            "client_crt": ["client.crt", "client.pem"],
            "client_key": ["client.key"],
        }
        for json_key, filenames in cert_files.items():
            for fn in filenames:
                p = os.path.join(certs_dir, fn)
                if os.path.exists(p):
                    with open(p, 'r') as f:
                        prov[json_key] = f.read().strip()
                    print(f"  Cert {json_key}: loaded from {fn}")
                    break

    # Send as single JSON line (the HAL reads one line and processes it)
    prov_json = json.dumps(prov, separators=(',', ':'))
    print(f"  Sending provisioning JSON ({len(prov_json)} bytes)...")
    s.write((prov_json + "\n").encode())
    s.flush()

    # Wait for response
    deadline = time.time() + 10
    ok = False
    while time.time() < deadline:
        if s.in_waiting:
            line = s.readline().decode('utf-8', errors='replace').strip()
            if '"ok"' in line or 'Provisioned' in line:
                ok = True
                print(f"  Result: {line}")
                break
            elif '"error"' in line or 'FAIL' in line:
                print(f"  Error: {line}")
                break
            elif line:
                print(f"  device: {line}")

    s.close()

    if ok:
        print("Provisioning complete!")
    else:
        print("Provisioning failed")
    return ok


def register_with_fleet(server_url: str, device_info: dict):
    """Register the provisioned device with the fleet server."""
    try:
        import urllib.request
        data = json.dumps({
            "version": device_info.get("version", "freshly-provisioned"),
            "board": device_info.get("board", device_info.get("display", "unknown")),
            "mac": device_info.get("mac", ""),
        }).encode()

        device_id = device_info.get("device_id", "unknown")
        url = f"{server_url}/api/devices/{device_id}/status"
        req = urllib.request.Request(url, data=data,
                                      headers={"Content-Type": "application/json"},
                                      method="POST")
        resp = urllib.request.urlopen(req, timeout=10)
        result = json.loads(resp.read())
        print(f"Registered with fleet server: {result}")
        return True
    except Exception as e:
        print(f"Fleet registration failed (non-fatal): {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="First-time ESP32 device provisioning",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--port", required=True, help="Serial port")
    parser.add_argument("--flash", metavar="BOARD",
                        help="Flash OTA firmware for this board first")
    parser.add_argument("--flash-only", action="store_true",
                        help="Only flash, skip provisioning")
    parser.add_argument("--device-id",
                        help="Device ID (fetched from fleet server if --server given)")
    parser.add_argument("--device-name", help="Human-friendly device name")
    parser.add_argument("--certs", help="Local certs directory")
    parser.add_argument("--server", help="Fleet server URL (e.g., http://localhost:8080)")
    parser.add_argument("--server-url",
                        help="Device's target server URL (stored on device)")
    parser.add_argument("--wifi-ssid", help="Factory WiFi SSID")
    parser.add_argument("--wifi-pass", help="Factory WiFi password")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    # Step 1: Flash firmware
    if args.flash:
        if not flash_firmware(args.port, args.flash):
            sys.exit(1)
        print()

    # Step 2: Wait for device
    print("--- Detecting device ---")
    time.sleep(3)
    device_info = wait_for_device(args.port, args.baud)
    if not device_info:
        print("ERROR: Device not responding")
        sys.exit(1)
    print()

    if args.flash_only:
        print("Flash-only mode, skipping provisioning")
        sys.exit(0)

    # Step 3: Get/generate provisioning data
    certs_dir = args.certs
    device_json = {}

    if args.server and not args.certs:
        # Fetch provisioning package from fleet server
        print(f"--- Fetching provisioning data from {args.server} ---")
        try:
            import urllib.request

            if args.device_id:
                # Use existing device
                device_json = {"device_id": args.device_id}
                url = f"{args.server}/api/provision/{args.device_id}"
                resp = urllib.request.urlopen(url, timeout=10)
                prov_info = json.loads(resp.read())
                # Download certs to temp dir
                import tempfile
                certs_dir = tempfile.mkdtemp(prefix="esp32-prov-")
                print(f"  Temp certs dir: {certs_dir}")
            else:
                # Generate new device
                data = json.dumps({
                    "device_name": args.device_name or "",
                    "server_url": args.server_url or args.server,
                    "wifi_ssid": args.wifi_ssid or "",
                    "wifi_pass": args.wifi_pass or "",
                }).encode()
                req = urllib.request.Request(
                    f"{args.server}/api/provision/generate",
                    data=data,
                    headers={"Content-Type": "application/json"},
                    method="POST")
                resp = urllib.request.urlopen(req, timeout=10)
                result = json.loads(resp.read())
                device_json = result['identity']
                args.device_id = result['device_id']
                certs_dir = result['provision_dir']
                print(f"  Generated device: {result['device_id']}")
                print(f"  Certs dir: {certs_dir}")
        except Exception as e:
            print(f"Failed to fetch provisioning data: {e}")
            sys.exit(1)
    elif args.certs:
        # Use local certs
        identity_path = os.path.join(args.certs, "device.json")
        if os.path.exists(identity_path):
            with open(identity_path) as f:
                device_json = json.load(f)
        else:
            import uuid
            device_json = {
                "device_id": args.device_id or f"esp32-{uuid.uuid4().hex[:12]}",
                "device_name": args.device_name or "",
                "server_url": args.server_url or args.server or "",
                "provisioned": True,
            }
    else:
        print("ERROR: Either --certs or --server is required for provisioning")
        sys.exit(1)

    print()

    # Step 4: Send provisioning data
    print("--- Provisioning device ---")
    ok = send_provision_data(args.port, device_json, certs_dir, args.baud)
    if not ok:
        print("\nProvisioning FAILED")
        sys.exit(1)
    print()

    # Step 5: Register with fleet server
    if args.server:
        print("--- Registering with fleet server ---")
        device_info['device_id'] = device_json.get('device_id', args.device_id)
        register_with_fleet(args.server, device_info)
        print()

    # Step 6: Verify
    print("--- Verifying ---")
    time.sleep(2)
    verify_info = wait_for_device(args.port, args.baud, timeout=10)
    if verify_info:
        print("Device verified and responsive")
    else:
        print("WARNING: Could not verify device after provisioning")

    print("\n=== Provisioning Complete ===")
    print(f"  Device ID: {device_json.get('device_id', '?')}")
    print(f"  Board: {device_info.get('board', device_info.get('display', '?'))}")
    if args.server:
        print(f"  Fleet server: {args.server}")


if __name__ == "__main__":
    main()
