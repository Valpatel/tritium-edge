#!/usr/bin/env bash
set -euo pipefail

# Auto-detect connected ESP32 board and flash firmware.
# Usage: ./scripts/flash.sh [BOARD_ENV]
#
# If BOARD_ENV is provided, flashes that environment directly.
# Otherwise, detects the connected ESP32 via USB VID/PID and selects
# the default environment.

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PIO="${PIO:-pio}"
BOARD="${1:-}"

# Known ESP32-S3 USB identifiers
# VID:PID -> description
declare -A USB_IDS=(
    ["303a:1001"]="ESP32-S3 native USB (JTAG+CDC)"
    ["10c4:ea60"]="CP2102/CP2104 USB-UART bridge"
    ["1a86:7523"]="CH340/CH341 USB-UART bridge"
    ["0403:6001"]="FTDI USB-UART bridge"
)

detect_port() {
    local port=""
    # Try PlatformIO's device detection first
    port=$($PIO device list --serial --json-output 2>/dev/null | \
        python3 -c "
import sys, json
try:
    devices = json.load(sys.stdin)
    for d in devices:
        desc = d.get('description', '').lower()
        hwid = d.get('hwid', '').lower()
        if 'esp32' in desc or '303a' in hwid or '10c4' in hwid or '1a86' in hwid:
            print(d['port'])
            break
except:
    pass
" 2>/dev/null) || true

    if [ -n "$port" ]; then
        echo "$port"
        return 0
    fi

    # Fallback: check common serial device paths
    for dev in /dev/ttyACM0 /dev/ttyUSB0 /dev/ttyACM1 /dev/ttyUSB1; do
        if [ -e "$dev" ]; then
            echo "$dev"
            return 0
        fi
    done

    return 1
}

echo "=== ESP32 Flash Tool ==="

# Detect serial port
PORT=$(detect_port) || {
    echo "ERROR: No ESP32 device detected."
    echo "Check USB connection and ensure the device is in download mode."
    echo ""
    echo "Tip: For ESP32-S3 native USB, hold BOOT while pressing RESET."
    exit 1
}
echo "Detected device on: $PORT"

# Use provided board or default
if [ -z "$BOARD" ]; then
    BOARD="touch-amoled-241b"
    echo "No board specified, using default: $BOARD"
fi

echo "Building and flashing: $BOARD"
echo ""

cd "$PROJECT_DIR"
$PIO run -e "$BOARD" -t upload --upload-port "$PORT"

echo ""
echo "Flash complete. Run 'make monitor' to see serial output."
