#!/usr/bin/env bash
set -euo pipefail

# Build, flash, and immediately open serial monitor.
#
# Usage:
#   ./scripts/flash-monitor.sh BOARD [APP]
#   ./scripts/flash-monitor.sh touch-lcd-35bc camera

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

"$SCRIPT_DIR/flash.sh" "$@"
echo ""
"$SCRIPT_DIR/monitor.sh"
