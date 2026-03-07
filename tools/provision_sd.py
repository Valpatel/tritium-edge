#!/usr/bin/env python3
"""
Generate SD card provisioning contents for ESP32 devices.

Creates a /provision/ directory structure on one or more SD cards with:
  - device.json       — Device identity and server config
  - ca.pem            — CA certificate
  - client.crt        — Client certificate
  - client.key        — Client private key
  - factory_wifi.json  — Temporary WiFi for initial setup (optional)

Usage:
  # Single device to a mounted SD card
  python provision_sd.py --sd /media/sdcard --certs ./certs --server https://api.example.com

  # Batch: 10 devices with auto-generated IDs
  python provision_sd.py --sd /media/sd0 /media/sd1 /media/sd2 ... \\
      --certs ./certs --server https://api.example.com --batch 10

  # With factory WiFi (temporary, cleared after first connection)
  python provision_sd.py --sd /media/sdcard --certs ./certs \\
      --server https://api.example.com \\
      --wifi-ssid "FactoryNet" --wifi-pass "setup123"

  # With MQTT broker
  python provision_sd.py --sd /media/sdcard --certs ./certs \\
      --server https://api.example.com \\
      --mqtt broker.example.com --mqtt-port 8883
"""

import argparse
import json
import os
import shutil
import sys
import uuid
from pathlib import Path


def generate_device_id():
    """Generate a unique device identifier."""
    return f"esp32-{uuid.uuid4().hex[:12]}"


def create_provision_dir(sd_path, device_id, device_name, certs_dir,
                         server_url, mqtt_broker, mqtt_port,
                         wifi_ssid, wifi_pass):
    """Create /provision/ directory on an SD card mount point."""
    prov_dir = Path(sd_path) / "provision"
    prov_dir.mkdir(parents=True, exist_ok=True)

    # Device identity
    identity = {
        "device_id": device_id,
        "device_name": device_name or device_id,
        "server_url": server_url,
        "mqtt_broker": mqtt_broker or "",
        "mqtt_port": mqtt_port,
        "provisioned": True,
    }
    (prov_dir / "device.json").write_text(json.dumps(identity, indent=2) + "\n")

    # Copy certificates
    certs = Path(certs_dir)
    cert_files = {
        "ca.pem": ["ca.pem", "ca.crt", "ca-cert.pem"],
        "client.crt": ["client.crt", "client.pem", "client-cert.pem"],
        "client.key": ["client.key", "client-key.pem"],
    }

    for target_name, source_names in cert_files.items():
        copied = False
        for src_name in source_names:
            src = certs / src_name
            if src.exists():
                shutil.copy2(src, prov_dir / target_name)
                copied = True
                break
        if not copied:
            print(f"  WARNING: No certificate found for {target_name} "
                  f"(looked for: {', '.join(source_names)})")

    # Factory WiFi (optional)
    if wifi_ssid:
        wifi_config = {
            "ssid": wifi_ssid,
            "password": wifi_pass or "",
        }
        (prov_dir / "factory_wifi.json").write_text(
            json.dumps(wifi_config, indent=2) + "\n"
        )

    return prov_dir


def main():
    parser = argparse.ArgumentParser(
        description="Generate SD card provisioning contents for ESP32 devices",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--sd", nargs="+", required=True,
                        help="Mount point(s) of SD card(s)")
    parser.add_argument("--certs", required=True,
                        help="Directory containing CA and client certificates")
    parser.add_argument("--server", required=True,
                        help="Server endpoint URL")
    parser.add_argument("--mqtt", default="",
                        help="MQTT broker address")
    parser.add_argument("--mqtt-port", type=int, default=8883,
                        help="MQTT port (default: 8883)")
    parser.add_argument("--device-id", default=None,
                        help="Device ID (auto-generated if not specified)")
    parser.add_argument("--device-name", default=None,
                        help="Human-friendly device name")
    parser.add_argument("--batch", type=int, default=0,
                        help="Generate N unique device configs (round-robin across SD cards)")
    parser.add_argument("--wifi-ssid", default=None,
                        help="Factory WiFi SSID (temporary, cleared after first connection)")
    parser.add_argument("--wifi-pass", default=None,
                        help="Factory WiFi password")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print what would be created without writing")

    args = parser.parse_args()

    # Validate certs directory
    if not os.path.isdir(args.certs):
        print(f"Error: Certs directory not found: {args.certs}", file=sys.stderr)
        sys.exit(1)

    # Validate SD mount points
    for sd in args.sd:
        if not os.path.isdir(sd):
            print(f"Error: SD mount point not found: {sd}", file=sys.stderr)
            sys.exit(1)

    if args.batch > 0:
        # Batch mode: generate N devices, round-robin across SD cards
        devices = []
        for i in range(args.batch):
            did = generate_device_id()
            sd_idx = i % len(args.sd)
            devices.append((args.sd[sd_idx], did))

        print(f"Provisioning {args.batch} devices across {len(args.sd)} SD card(s):")
        for sd_path, device_id in devices:
            print(f"  {device_id} -> {sd_path}")
            if not args.dry_run:
                prov_dir = create_provision_dir(
                    sd_path, device_id, None, args.certs,
                    args.server, args.mqtt, args.mqtt_port,
                    args.wifi_ssid, args.wifi_pass,
                )
                print(f"    Created: {prov_dir}")

        # Write manifest
        manifest = {
            "generated_devices": [
                {"device_id": did, "sd_card": sd}
                for sd, did in devices
            ],
            "server_url": args.server,
            "mqtt_broker": args.mqtt,
        }
        manifest_path = Path("provision_manifest.json")
        if not args.dry_run:
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
            print(f"\nManifest written to: {manifest_path}")
    else:
        # Single device mode
        device_id = args.device_id or generate_device_id()
        sd_path = args.sd[0]

        print(f"Provisioning device {device_id} on {sd_path}")
        if not args.dry_run:
            prov_dir = create_provision_dir(
                sd_path, device_id, args.device_name, args.certs,
                args.server, args.mqtt, args.mqtt_port,
                args.wifi_ssid, args.wifi_pass,
            )
            print(f"Created: {prov_dir}")
            for f in sorted(prov_dir.iterdir()):
                size = f.stat().st_size
                print(f"  {f.name:20s} {size:>8d} bytes")

    if args.dry_run:
        print("\n(dry run — no files written)")


if __name__ == "__main__":
    main()
