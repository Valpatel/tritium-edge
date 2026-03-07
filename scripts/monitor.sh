#!/usr/bin/env bash
set -euo pipefail

# Open serial monitor for a connected ESP32 board.
#
# Usage:
#   ./scripts/monitor.sh          # Auto-detect port
#   ./scripts/monitor.sh /dev/ttyACM0 115200   # Explicit port + baud

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PIO="${PIO:-pio}"
PORT="${1:-}"
BAUD="${2:-115200}"

# Auto-detect if no port given
if [ -z "$PORT" ]; then
    for dev in /dev/ttyACM0 /dev/ttyUSB0 /dev/ttyACM1 /dev/ttyUSB1; do
        if [ -e "$dev" ]; then
            PORT="$dev"
            break
        fi
    done
fi

if [ -z "$PORT" ]; then
    echo "ERROR: No serial device found."
    exit 1
fi

# Fix permissions if needed
if [ ! -w "$PORT" ]; then
    echo "Fixing permissions on $PORT..."
    sudo chmod 666 "$PORT"
fi

echo "=== Monitor: $PORT @ ${BAUD}baud ==="
echo "Press Ctrl+C to exit"
echo "---"

cd "$PROJECT_DIR"
exec $PIO device monitor -b "$BAUD" -p "$PORT" --filter colorize --filter time
