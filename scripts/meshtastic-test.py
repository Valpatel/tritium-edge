#!/usr/bin/env python3
# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Meshtastic End-to-End Test — send a test message and verify receipt.

Sends a test message from the local Meshtastic radio (e.g., Heltec LoRa V3)
and optionally verifies it arrives on a remote radio (e.g., T-LoRa Pager
on gb10-02).

Environment variables:
    LOCAL_PORT      Serial port for local radio (default: /dev/ttyUSB0)
    REMOTE_HOST     SSH host for remote radio verification (default: none)
    REMOTE_PORT     Serial port on remote host (default: /dev/ttyACM0)
    REMOTE_USER     SSH user for remote host (default: scubasonar)
    TEST_CHANNEL    Meshtastic channel index (default: 0)
    TIMEOUT         Seconds to wait for receipt (default: 60)

Usage:
    # Send test message from local radio
    python3 scripts/meshtastic-test.py

    # Send and verify on remote radio
    REMOTE_HOST=192.168.86.8 python3 scripts/meshtastic-test.py

    # Specify local port
    LOCAL_PORT=/dev/ttyACM0 python3 scripts/meshtastic-test.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
import uuid

LOCAL_PORT = os.environ.get("LOCAL_PORT", "/dev/ttyUSB0")
REMOTE_HOST = os.environ.get("REMOTE_HOST", "")
REMOTE_PORT = os.environ.get("REMOTE_PORT", "/dev/ttyACM0")
REMOTE_USER = os.environ.get("REMOTE_USER", "scubasonar")
TEST_CHANNEL = int(os.environ.get("TEST_CHANNEL", "0"))
TIMEOUT = int(os.environ.get("TIMEOUT", "60"))


def send_test_message() -> tuple[bool, str]:
    """Send a test message from the local Meshtastic radio.

    Returns (success, test_id).
    """
    test_id = f"TRITIUM-TEST-{uuid.uuid4().hex[:8].upper()}"
    message = f"{test_id} {time.strftime('%H:%M:%S')}"

    print(f"[TX] Sending test message: {message}")
    print(f"[TX] Port: {LOCAL_PORT}")

    try:
        import meshtastic
        from meshtastic.serial_interface import SerialInterface
    except ImportError:
        print("[ERROR] meshtastic package not installed. Install with: pip install meshtastic")
        return False, test_id

    try:
        interface = SerialInterface(devPath=LOCAL_PORT)
        time.sleep(2)  # Wait for connection to stabilize

        # Print local node info
        my_info = interface.myInfo
        if my_info:
            print(f"[TX] Local node: {my_info}")

        node_count = len(interface.nodes) if interface.nodes else 0
        print(f"[TX] Visible nodes: {node_count}")

        # Send the message
        interface.sendText(message, channelIndex=TEST_CHANNEL)
        print(f"[TX] Message sent on channel {TEST_CHANNEL}")

        time.sleep(1)
        interface.close()
        return True, test_id

    except Exception as exc:
        print(f"[ERROR] Send failed: {exc}")
        return False, test_id


def verify_on_remote(test_id: str) -> bool:
    """SSH to remote host and check if the test message arrived.

    Uses the meshtastic CLI on the remote host to check recent messages.
    """
    if not REMOTE_HOST:
        print("[SKIP] No REMOTE_HOST set — skipping remote verification")
        return True

    print(f"[RX] Checking remote host {REMOTE_USER}@{REMOTE_HOST}...")
    print(f"[RX] Remote port: {REMOTE_PORT}")
    print(f"[RX] Waiting up to {TIMEOUT}s for message arrival...")

    # Poll remote for the test message
    start = time.time()
    meshtastic_bin = "$HOME/.local/bin/meshtastic"

    while time.time() - start < TIMEOUT:
        try:
            # Use sg dialout to access the serial port on the remote host
            cmd = [
                "ssh", f"{REMOTE_USER}@{REMOTE_HOST}",
                f"sg dialout -c '{meshtastic_bin} --port {REMOTE_PORT} --info 2>/dev/null'"
            ]
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=30,
            )

            if test_id in result.stdout:
                elapsed = time.time() - start
                print(f"[RX] Message received on remote! ({elapsed:.1f}s)")
                return True

        except subprocess.TimeoutExpired:
            print("[RX] SSH command timed out, retrying...")
        except Exception as exc:
            print(f"[RX] Check error: {exc}")

        # Wait before next check
        time.sleep(5)

    print(f"[RX] Timeout: message not found after {TIMEOUT}s")
    print("[RX] Note: LoRa messages may take time to propagate through the mesh.")
    print("[RX] The message may still arrive — check the remote radio manually.")
    return False


def check_local_radio() -> bool:
    """Quick check that the local radio is accessible."""
    if not os.path.exists(LOCAL_PORT):
        print(f"[ERROR] Serial port {LOCAL_PORT} not found")
        print("[HINT] Available serial ports:")
        try:
            import serial.tools.list_ports
            ports = list(serial.tools.list_ports.comports())
            if ports:
                for p in ports:
                    print(f"  {p.device} — {p.description}")
            else:
                print("  (none found)")
        except ImportError:
            print("  (pyserial not installed)")
        return False
    return True


def main():
    print("=" * 50)
    print("Meshtastic End-to-End Test")
    print("=" * 50)

    # Check local radio
    if not check_local_radio():
        sys.exit(1)

    # Send test message
    ok, test_id = send_test_message()
    if not ok:
        print("[FAIL] Could not send test message")
        sys.exit(1)

    print(f"[OK] Test message sent: {test_id}")

    # Verify on remote
    if REMOTE_HOST:
        if verify_on_remote(test_id):
            print("[PASS] End-to-end LoRa test PASSED")
            sys.exit(0)
        else:
            print("[WARN] Remote verification timed out (message may still arrive)")
            sys.exit(2)
    else:
        print("[INFO] Set REMOTE_HOST to verify receipt on remote radio")
        print("[PASS] Local send test PASSED")
        sys.exit(0)


if __name__ == "__main__":
    main()
