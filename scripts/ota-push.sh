#!/bin/bash
# Push firmware to a device via serial OTA
# Usage: ./scripts/ota-push.sh PORT BOARD [APP]
#   PORT  = /dev/ttyACM0, /dev/ttyACM1, etc.
#   BOARD = touch-lcd-349, touch-amoled-241b, etc.
#   APP   = ota, starfield (default), system, etc.
#
# Example: ./scripts/ota-push.sh /dev/ttyACM1 touch-lcd-349 starfield

set -e

PORT="${1:?Usage: $0 PORT BOARD [APP]}"
BOARD="${2:?Usage: $0 PORT BOARD [APP]}"
APP="${3:-starfield}"

ENV="${BOARD}-${APP}-ota"
# Special case: OTA app env doesn't have the app suffix twice
if [ "$APP" = "ota" ]; then
    ENV="${BOARD}-ota"
fi

FW=".pio/build/${ENV}/firmware.bin"

echo "=== OTA Push ==="
echo "  Port:  $PORT"
echo "  Board: $BOARD"
echo "  App:   $APP"
echo "  Env:   $ENV"
echo ""

# Build
echo "Building ${ENV}..."
pio run -e "${ENV}" || { echo "Build failed!"; exit 1; }

if [ ! -f "$FW" ]; then
    echo "ERROR: ${FW} not found"
    exit 1
fi

echo "Firmware: $(stat -c%s "$FW") bytes"
echo ""

# Fix permissions
sudo chmod 666 "$PORT" 2>/dev/null || true

# Push via serial OTA
python3 tools/serial_ota.py "$PORT" "$FW"
