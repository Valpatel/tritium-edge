#!/usr/bin/env bash
# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
#
# watchdog.sh — Self-preservation watchdog for tritium-edge fleet server.
#
# Monitors the fleet server process and restarts it if it crashes or becomes
# unresponsive. Also monitors system resources (memory, disk, CPU temp) and
# takes corrective action when thresholds are exceeded.
#
# Usage:
#   ./scripts/watchdog.sh [--interval SECONDS] [--max-restarts N] [--pid-file PATH]
#
# Environment:
#   TRITIUM_SERVER_CMD    Command to start fleet server (default: python3 server/main.py)
#   TRITIUM_HEALTH_URL    Health check URL (default: http://localhost:8080/health)
#   TRITIUM_LOG_DIR       Log directory (default: /var/log/tritium)
#   TRITIUM_MAX_MEMORY_MB Max memory before restart (default: 512)
#   TRITIUM_MAX_DISK_PCT  Max disk usage before alert (default: 90)

set -euo pipefail

# -- Configuration -------------------------------------------------------------

INTERVAL="${1:-30}"
MAX_RESTARTS="${2:-5}"
PID_FILE="${3:-/tmp/tritium-server.pid}"

# Parse named arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --interval) INTERVAL="$2"; shift 2 ;;
        --max-restarts) MAX_RESTARTS="$2"; shift 2 ;;
        --pid-file) PID_FILE="$2"; shift 2 ;;
        *) shift ;;
    esac
done

SERVER_CMD="${TRITIUM_SERVER_CMD:-python3 server/main.py}"
HEALTH_URL="${TRITIUM_HEALTH_URL:-http://localhost:8080/health}"
LOG_DIR="${TRITIUM_LOG_DIR:-/var/log/tritium}"
MAX_MEMORY_MB="${TRITIUM_MAX_MEMORY_MB:-512}"
MAX_DISK_PCT="${TRITIUM_MAX_DISK_PCT:-90}"

RESTART_COUNT=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDGE_DIR="$(dirname "$SCRIPT_DIR")"

# -- Logging -------------------------------------------------------------------

mkdir -p "$LOG_DIR" 2>/dev/null || LOG_DIR="/tmp"
LOG_FILE="$LOG_DIR/watchdog.log"

log() {
    local level="$1"
    shift
    local msg="$*"
    local ts
    ts="$(date '+%Y-%m-%d %H:%M:%S')"
    echo "[$ts] [$level] $msg" | tee -a "$LOG_FILE"
}

log_info()  { log "INFO"  "$@"; }
log_warn()  { log "WARN"  "$@"; }
log_error() { log "ERROR" "$@"; }

# -- Health check --------------------------------------------------------------

check_health() {
    # Check if process is running
    if [[ -f "$PID_FILE" ]]; then
        local pid
        pid="$(cat "$PID_FILE" 2>/dev/null || echo "")"
        if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
            log_error "Server process $pid is not running"
            return 1
        fi
    fi

    # HTTP health check
    if command -v curl &>/dev/null; then
        local http_code
        http_code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$HEALTH_URL" 2>/dev/null || echo "000")"
        if [[ "$http_code" != "200" ]]; then
            log_error "Health check failed: HTTP $http_code from $HEALTH_URL"
            return 1
        fi
    elif command -v wget &>/dev/null; then
        if ! wget -q --spider --timeout=10 "$HEALTH_URL" 2>/dev/null; then
            log_error "Health check failed: wget couldn't reach $HEALTH_URL"
            return 1
        fi
    else
        log_warn "No curl or wget available, skipping HTTP health check"
    fi

    return 0
}

# -- Resource monitoring -------------------------------------------------------

check_resources() {
    local issues=0

    # Memory check
    if command -v free &>/dev/null; then
        local mem_used_mb
        mem_used_mb="$(free -m | awk '/^Mem:/ {print $3}')"
        local mem_total_mb
        mem_total_mb="$(free -m | awk '/^Mem:/ {print $2}')"
        local mem_available_mb
        mem_available_mb="$(free -m | awk '/^Mem:/ {print $7}')"

        if [[ -n "$mem_available_mb" ]] && [[ "$mem_available_mb" -lt 500 ]]; then
            log_warn "Low memory: ${mem_available_mb}MB available"
            issues=$((issues + 1))
        fi
    fi

    # Disk check
    local disk_pct
    disk_pct="$(df -h / 2>/dev/null | awk 'NR==2 {gsub(/%/,""); print $5}' || echo "0")"
    if [[ -n "$disk_pct" ]] && [[ "$disk_pct" -gt "$MAX_DISK_PCT" ]]; then
        log_warn "Disk usage high: ${disk_pct}%"
        issues=$((issues + 1))
    fi

    # CPU temperature (ESP32 host or Linux)
    if [[ -f /sys/class/thermal/thermal_zone0/temp ]]; then
        local temp_mc
        temp_mc="$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo "0")"
        local temp_c=$((temp_mc / 1000))
        if [[ "$temp_c" -gt 80 ]]; then
            log_warn "CPU temperature high: ${temp_c}C"
            issues=$((issues + 1))
        fi
    fi

    # Server process memory
    if [[ -f "$PID_FILE" ]]; then
        local pid
        pid="$(cat "$PID_FILE" 2>/dev/null || echo "")"
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            local rss_kb
            rss_kb="$(ps -o rss= -p "$pid" 2>/dev/null || echo "0")"
            local rss_mb=$((rss_kb / 1024))
            if [[ "$rss_mb" -gt "$MAX_MEMORY_MB" ]]; then
                log_warn "Server using ${rss_mb}MB (limit: ${MAX_MEMORY_MB}MB)"
                issues=$((issues + 1))
            fi
        fi
    fi

    return "$issues"
}

# -- Server management ---------------------------------------------------------

start_server() {
    log_info "Starting fleet server: $SERVER_CMD"
    cd "$EDGE_DIR"

    # Start server in background
    $SERVER_CMD &
    local pid=$!
    echo "$pid" > "$PID_FILE"
    log_info "Server started with PID $pid"

    # Wait briefly and check it didn't crash immediately
    sleep 2
    if ! kill -0 "$pid" 2>/dev/null; then
        log_error "Server crashed immediately after start"
        return 1
    fi

    return 0
}

stop_server() {
    if [[ -f "$PID_FILE" ]]; then
        local pid
        pid="$(cat "$PID_FILE" 2>/dev/null || echo "")"
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            log_info "Stopping server PID $pid"
            kill -TERM "$pid" 2>/dev/null || true
            # Wait up to 10s for graceful shutdown
            local waited=0
            while kill -0 "$pid" 2>/dev/null && [[ "$waited" -lt 10 ]]; do
                sleep 1
                waited=$((waited + 1))
            done
            # Force kill if still running
            if kill -0 "$pid" 2>/dev/null; then
                log_warn "Force killing server PID $pid"
                kill -9 "$pid" 2>/dev/null || true
            fi
        fi
        rm -f "$PID_FILE"
    fi
}

restart_server() {
    RESTART_COUNT=$((RESTART_COUNT + 1))
    if [[ "$RESTART_COUNT" -gt "$MAX_RESTARTS" ]]; then
        log_error "Max restarts ($MAX_RESTARTS) exceeded. Entering cooldown."
        sleep 300  # 5 minute cooldown
        RESTART_COUNT=0
    fi

    log_warn "Restarting server (attempt $RESTART_COUNT/$MAX_RESTARTS)"
    stop_server
    sleep 2
    start_server
}

# -- Signal handling -----------------------------------------------------------

cleanup() {
    log_info "Watchdog shutting down"
    stop_server
    exit 0
}

trap cleanup SIGTERM SIGINT SIGHUP

# -- Main loop -----------------------------------------------------------------

main() {
    log_info "Tritium watchdog starting (interval=${INTERVAL}s, max_restarts=${MAX_RESTARTS})"
    log_info "Server command: $SERVER_CMD"
    log_info "Health URL: $HEALTH_URL"

    # Initial server start
    if ! check_health 2>/dev/null; then
        start_server || {
            log_error "Initial server start failed"
            exit 1
        }
        # Give server time to initialize
        sleep 5
    else
        log_info "Server already running and healthy"
    fi

    # Main monitoring loop
    while true; do
        sleep "$INTERVAL"

        # Health check
        if ! check_health; then
            log_error "Health check FAILED"
            restart_server
            continue
        fi

        # Resource monitoring (non-fatal)
        check_resources || true

        # Reset restart counter on sustained health
        if [[ "$RESTART_COUNT" -gt 0 ]]; then
            RESTART_COUNT=$((RESTART_COUNT - 1))
        fi

        log_info "Health check OK (restarts_remaining=$((MAX_RESTARTS - RESTART_COUNT)))"
    done
}

main "$@"
