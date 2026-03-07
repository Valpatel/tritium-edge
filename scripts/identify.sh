#!/usr/bin/env bash
set -euo pipefail

# Identify connected ESP32 boards via USB.
# Sends "IDENTIFY\n" to each serial port and prints the JSON response.
#
# Usage: ./scripts/identify.sh

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== Board Detection ==="

found=0
for dev in /dev/ttyACM0 /dev/ttyUSB0 /dev/ttyACM1 /dev/ttyUSB1; do
    if [ -e "$dev" ]; then
        # Fix permissions if needed
        if [ ! -w "$dev" ]; then
            sudo chmod 666 "$dev" 2>/dev/null || continue
        fi

        echo -n "$dev: "

        # Send IDENTIFY and read response with timeout
        response=$(echo "IDENTIFY" > "$dev" && timeout 2 head -1 "$dev" 2>/dev/null) || response=""

        if [ -n "$response" ] && echo "$response" | grep -q '"board"'; then
            echo "$response"
            found=$((found + 1))
        else
            echo "(no firmware response)"
        fi
    fi
done

if [ $found -eq 0 ]; then
    echo "No boards responded. They may not be running our firmware."
fi

# Also run the Python detector if available
if [ -f "$PROJECT_DIR/tools/detect_boards.py" ]; then
    echo ""
    echo "--- MAC-based detection ---"
    python3 "$PROJECT_DIR/tools/detect_boards.py" 2>/dev/null || true
fi
