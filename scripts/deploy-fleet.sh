#!/usr/bin/env bash
# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
# TRITIUM EDGE Fleet Deployment Script
# Flashes Tritium-OS to all connected ESP32 boards, configures WiFi/MQTT,
# and registers them with the SC fleet server.
#
# Usage:
#   ./scripts/deploy-fleet.sh                          # Flash all connected boards
#   ./scripts/deploy-fleet.sh --env production         # Use production build env
#   ./scripts/deploy-fleet.sh --wifi "SSID" "PASS"     # Override WiFi credentials
#   ./scripts/deploy-fleet.sh --mqtt host:port          # Override MQTT broker
#   ./scripts/deploy-fleet.sh --sc-url http://host:8000 # SC server URL
#   ./scripts/deploy-fleet.sh --dry-run                 # Show what would be done
#   ./scripts/deploy-fleet.sh --list                    # List connected boards only
#
# Prerequisites:
#   - PlatformIO CLI installed (pip install platformio)
#   - ESP32-S3 boards connected via USB
#   - WiFi credentials configured (or passed via flags)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TRITIUM_DIR="$(cd "$EDGE_DIR/.." && pwd)"

# Defaults
PIO_ENV="touch-lcd-43c-box-os"
WIFI_SSID="${TRITIUM_WIFI_SSID:-}"
WIFI_PASS="${TRITIUM_WIFI_PASS:-}"
MQTT_HOST="${TRITIUM_MQTT_HOST:-}"
MQTT_PORT="${TRITIUM_MQTT_PORT:-1883}"
SC_URL="${TRITIUM_SC_URL:-http://localhost:8000}"
DRY_RUN=false
LIST_ONLY=false
MONITOR_AFTER=false

# Colors
CYAN='\033[0;36m'
GREEN='\033[0;32m'
MAGENTA='\033[0;35m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${CYAN}[FLEET]${NC} $1"; }
ok()  { echo -e "${GREEN}  [OK]${NC} $1"; }
warn(){ echo -e "${YELLOW}  [WARN]${NC} $1"; }
fail(){ echo -e "${MAGENTA}  [FAIL]${NC} $1"; }

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --env) PIO_ENV="$2"; shift 2 ;;
        --wifi) WIFI_SSID="$2"; WIFI_PASS="$3"; shift 3 ;;
        --mqtt) IFS=':' read -r MQTT_HOST MQTT_PORT <<< "$2"; shift 2 ;;
        --sc-url) SC_URL="$2"; shift 2 ;;
        --dry-run) DRY_RUN=true; shift ;;
        --list) LIST_ONLY=true; shift ;;
        --monitor) MONITOR_AFTER=true; shift ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo "  --env ENV          PlatformIO environment (default: $PIO_ENV)"
            echo "  --wifi SSID PASS   WiFi credentials"
            echo "  --mqtt HOST:PORT   MQTT broker address"
            echo "  --sc-url URL       SC server URL for registration"
            echo "  --dry-run          Show what would be done"
            echo "  --list             List connected boards only"
            echo "  --monitor          Monitor serial output after flashing"
            exit 0
            ;;
        *) warn "Unknown argument: $1"; shift ;;
    esac
done

# Check prerequisites
if ! command -v pio &>/dev/null; then
    fail "PlatformIO CLI not found. Install with: pip install platformio"
    exit 1
fi

# Detect connected boards
log "Scanning for connected ESP32 boards..."
PORTS=()

# Linux: /dev/ttyUSB* or /dev/ttyACM*
for port in /dev/ttyUSB* /dev/ttyACM*; do
    if [ -e "$port" ]; then
        PORTS+=("$port")
    fi
done

# macOS: /dev/cu.usbserial* or /dev/cu.usbmodem*
for port in /dev/cu.usbserial* /dev/cu.usbmodem*; do
    if [ -e "$port" ]; then
        PORTS+=("$port")
    fi
done

if [ ${#PORTS[@]} -eq 0 ]; then
    fail "No ESP32 boards detected. Check USB connections."
    echo "  Expected: /dev/ttyUSB*, /dev/ttyACM*, or /dev/cu.usb*"
    exit 1
fi

log "Found ${#PORTS[@]} board(s):"
for port in "${PORTS[@]}"; do
    echo "  - $port"
done

if [ "$LIST_ONLY" = true ]; then
    exit 0
fi

# Build firmware
log "Building firmware (env: $PIO_ENV)..."
cd "$EDGE_DIR"

if [ "$DRY_RUN" = true ]; then
    ok "[DRY RUN] Would build: pio run -e $PIO_ENV"
else
    if pio run -e "$PIO_ENV" 2>&1; then
        ok "Firmware built successfully"
    else
        fail "Firmware build failed"
        exit 1
    fi
fi

# Flash each board
FLASH_COUNT=0
FAIL_COUNT=0

for port in "${PORTS[@]}"; do
    log "Flashing $port..."

    if [ "$DRY_RUN" = true ]; then
        ok "[DRY RUN] Would flash: pio run -e $PIO_ENV -t upload --upload-port $port"
        FLASH_COUNT=$((FLASH_COUNT + 1))
        continue
    fi

    if pio run -e "$PIO_ENV" -t upload --upload-port "$port" 2>&1; then
        ok "Flashed $port"
        FLASH_COUNT=$((FLASH_COUNT + 1))

        # Wait for board to reboot
        sleep 3

        # Register with SC fleet server
        if [ -n "$SC_URL" ]; then
            DEVICE_ID="esp32_$(basename "$port")"
            log "Registering $DEVICE_ID with SC..."

            REGISTER_DATA=$(cat <<REGEOF
{
    "device_id": "$DEVICE_ID",
    "port": "$port",
    "firmware_env": "$PIO_ENV",
    "flash_time": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
REGEOF
)
            if curl -sf -X POST "$SC_URL/api/fleet/register" \
                -H "Content-Type: application/json" \
                -d "$REGISTER_DATA" >/dev/null 2>&1; then
                ok "Registered $DEVICE_ID with SC"
            else
                warn "Could not register with SC at $SC_URL (server may not be running)"
            fi
        fi
    else
        fail "Failed to flash $port"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
done

echo ""
log "Fleet deployment complete:"
ok "Flashed: $FLASH_COUNT"
if [ $FAIL_COUNT -gt 0 ]; then
    fail "Failed: $FAIL_COUNT"
fi

# Monitor first board if requested
if [ "$MONITOR_AFTER" = true ] && [ ${#PORTS[@]} -gt 0 ]; then
    log "Monitoring ${PORTS[0]}..."
    pio device monitor --port "${PORTS[0]}" --baud 115200
fi
