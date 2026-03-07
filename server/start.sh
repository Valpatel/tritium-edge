#!/usr/bin/env bash
# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
#
# Start the Tritium-Edge Management Server.
#
# Usage:
#   ./start.sh              # Start on default port 8080
#   ./start.sh --port 9000  # Custom port
#   ./start.sh --reload     # Dev mode with hot reload

set -euo pipefail
cd "$(dirname "$0")"

# Create venv if needed
if [ ! -d ".venv" ]; then
    echo "Creating virtual environment..."
    python3 -m venv .venv
    .venv/bin/pip install -q -r requirements.txt
fi

# Activate
source .venv/bin/activate

# Default args
HOST="${HOST:-localhost}"
PORT="${PORT:-8080}"
EXTRA_ARGS=""

# Parse args
while [[ $# -gt 0 ]]; do
    case $1 in
        --port) PORT="$2"; shift 2 ;;
        --host) HOST="$2"; shift 2 ;;
        --reload) EXTRA_ARGS="$EXTRA_ARGS --reload"; shift ;;
        *) EXTRA_ARGS="$EXTRA_ARGS $1"; shift ;;
    esac
done

exec uvicorn app.main:app --host "$HOST" --port "$PORT" $EXTRA_ARGS
