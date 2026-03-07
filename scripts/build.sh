#!/usr/bin/env bash
set -euo pipefail

# Build firmware for a specific board and app.
#
# Usage:
#   ./scripts/build.sh BOARD [APP]
#   ./scripts/build.sh touch-lcd-35bc          # Build starfield (default)
#   ./scripts/build.sh touch-lcd-35bc camera   # Build camera app
#   ./scripts/build.sh touch-lcd-35bc system   # Build system dashboard

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

if [ -n "$APP" ]; then
    ENV="${BOARD}-${APP}"
else
    ENV="${BOARD}"
fi

echo "=== Build: $ENV ==="
cd "$PROJECT_DIR"
$PIO run -e "$ENV"
