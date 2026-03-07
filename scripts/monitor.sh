#!/usr/bin/env bash
set -euo pipefail

# Serial monitor wrapper with color output and timestamps.
# Usage: ./scripts/monitor.sh [PORT] [BAUD]

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PIO="${PIO:-pio}"
PORT="${1:-}"
BAUD="${2:-115200}"

# If no port specified, let PlatformIO auto-detect
if [ -n "$PORT" ]; then
    PORT_ARG="--port $PORT"
else
    PORT_ARG=""
fi

echo "=== ESP32 Serial Monitor ==="
echo "Baud: $BAUD"
[ -n "$PORT" ] && echo "Port: $PORT" || echo "Port: auto-detect"
echo "Press Ctrl+C to exit"
echo "---"

cd "$PROJECT_DIR"

# Use PlatformIO monitor with filters for color and timestamp
# shellcheck disable=SC2086
exec $PIO device monitor \
    -b "$BAUD" \
    $PORT_ARG \
    --filter colorize \
    --filter time
