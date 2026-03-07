#!/usr/bin/env bash
set -euo pipefail

# Build and flash firmware to a connected ESP32 board.
#
# Usage:
#   ./scripts/flash.sh BOARD [APP]
#   ./scripts/flash.sh touch-lcd-35bc          # Flash starfield (default app)
#   ./scripts/flash.sh touch-lcd-35bc camera   # Flash camera app
#   ./scripts/flash.sh touch-lcd-35bc system   # Flash system dashboard
#
# The script auto-detects the serial port and fixes permissions if needed.

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PIO="${PIO:-pio}"
BOARD="${1:-}"
APP="${2:-}"

if [ -z "$BOARD" ]; then
    echo "Usage: $0 BOARD [APP]"
    echo ""
    echo "Boards: touch-amoled-241b, amoled-191m, touch-amoled-18,"
    echo "        touch-lcd-35bc, touch-lcd-43c-box, touch-lcd-349"
    echo ""
    echo "Apps:   (none)=starfield, camera, system, wifi, ui"
    exit 1
fi

# Build the environment name
if [ -n "$APP" ]; then
    ENV="${BOARD}-${APP}"
else
    ENV="${BOARD}"
fi

# Auto-detect serial port
detect_port() {
    for dev in /dev/ttyACM0 /dev/ttyUSB0 /dev/ttyACM1 /dev/ttyUSB1; do
        if [ -e "$dev" ]; then
            echo "$dev"
            return 0
        fi
    done
    return 1
}

PORT=$(detect_port) || {
    echo "ERROR: No ESP32 device found. Check USB connection."
    exit 1
}

# Fix permissions if needed
if [ ! -w "$PORT" ]; then
    echo "Fixing permissions on $PORT..."
    sudo chmod 666 "$PORT"
fi

echo "=== Flash: $ENV → $PORT ==="
cd "$PROJECT_DIR"
$PIO run -e "$ENV" -t upload --upload-port "$PORT"
echo ""
echo "Done. Run: ./scripts/monitor.sh"
