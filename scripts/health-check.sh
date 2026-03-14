#!/usr/bin/env bash
# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
#
# health-check.sh — Quick health check for tritium-edge fleet server.
#
# Returns exit code 0 if healthy, non-zero if unhealthy.
# Designed for use by watchdog.sh, Docker HEALTHCHECK, or systemd.
#
# Usage:
#   ./scripts/health-check.sh [--url URL] [--timeout SECONDS] [--verbose]
#
# Checks:
#   1. Fleet server HTTP health endpoint
#   2. MQTT broker connectivity (if MQTT_HOST set)
#   3. System resource thresholds
#   4. Process file descriptor limits

set -euo pipefail

# -- Configuration -------------------------------------------------------------

HEALTH_URL="${TRITIUM_HEALTH_URL:-http://localhost:8080/health}"
TIMEOUT=10
VERBOSE=false
MQTT_HOST="${MQTT_HOST:-}"
MQTT_PORT="${MQTT_PORT:-1883}"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --url) HEALTH_URL="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --verbose) VERBOSE=true; shift ;;
        --mqtt-host) MQTT_HOST="$2"; shift 2 ;;
        *) shift ;;
    esac
done

# -- Helpers -------------------------------------------------------------------

CHECKS_PASSED=0
CHECKS_FAILED=0
CHECKS_WARNED=0

pass() {
    CHECKS_PASSED=$((CHECKS_PASSED + 1))
    if $VERBOSE; then echo "[PASS] $1"; fi
}

fail() {
    CHECKS_FAILED=$((CHECKS_FAILED + 1))
    echo "[FAIL] $1" >&2
}

warn() {
    CHECKS_WARNED=$((CHECKS_WARNED + 1))
    if $VERBOSE; then echo "[WARN] $1"; fi
}

# -- Check: HTTP health endpoint -----------------------------------------------

check_http() {
    if command -v curl &>/dev/null; then
        local http_code
        http_code="$(curl -s -o /dev/null -w '%{http_code}' --max-time "$TIMEOUT" "$HEALTH_URL" 2>/dev/null || echo "000")"
        if [[ "$http_code" == "200" ]]; then
            pass "HTTP health: $HEALTH_URL -> 200"
            return 0
        else
            fail "HTTP health: $HEALTH_URL -> $http_code"
            return 1
        fi
    elif command -v wget &>/dev/null; then
        if wget -q --spider --timeout="$TIMEOUT" "$HEALTH_URL" 2>/dev/null; then
            pass "HTTP health: $HEALTH_URL -> OK"
            return 0
        else
            fail "HTTP health: $HEALTH_URL -> unreachable"
            return 1
        fi
    else
        warn "No curl or wget — cannot check HTTP health"
        return 0
    fi
}

# -- Check: MQTT broker --------------------------------------------------------

check_mqtt() {
    if [[ -z "$MQTT_HOST" ]]; then
        if $VERBOSE; then echo "[SKIP] MQTT check (MQTT_HOST not set)"; fi
        return 0
    fi

    # Check TCP connectivity to MQTT broker
    if command -v nc &>/dev/null; then
        if nc -z -w "$TIMEOUT" "$MQTT_HOST" "$MQTT_PORT" 2>/dev/null; then
            pass "MQTT broker: $MQTT_HOST:$MQTT_PORT reachable"
            return 0
        else
            fail "MQTT broker: $MQTT_HOST:$MQTT_PORT unreachable"
            return 1
        fi
    elif command -v bash &>/dev/null; then
        # Bash TCP check fallback
        if timeout "$TIMEOUT" bash -c "echo >/dev/tcp/$MQTT_HOST/$MQTT_PORT" 2>/dev/null; then
            pass "MQTT broker: $MQTT_HOST:$MQTT_PORT reachable"
            return 0
        else
            fail "MQTT broker: $MQTT_HOST:$MQTT_PORT unreachable"
            return 1
        fi
    else
        warn "No nc or bash TCP — cannot check MQTT"
        return 0
    fi
}

# -- Check: Memory -------------------------------------------------------------

check_memory() {
    if ! command -v free &>/dev/null; then
        return 0
    fi

    local available_mb
    available_mb="$(free -m | awk '/^Mem:/ {print $7}')"

    if [[ -z "$available_mb" ]]; then
        return 0
    fi

    if [[ "$available_mb" -lt 100 ]]; then
        fail "Memory critically low: ${available_mb}MB available"
        return 1
    elif [[ "$available_mb" -lt 500 ]]; then
        warn "Memory low: ${available_mb}MB available"
        return 0
    else
        pass "Memory: ${available_mb}MB available"
        return 0
    fi
}

# -- Check: Disk ---------------------------------------------------------------

check_disk() {
    local disk_pct
    disk_pct="$(df -h / 2>/dev/null | awk 'NR==2 {gsub(/%/,""); print $5}' || echo "0")"

    if [[ "$disk_pct" -gt 95 ]]; then
        fail "Disk critically full: ${disk_pct}%"
        return 1
    elif [[ "$disk_pct" -gt 90 ]]; then
        warn "Disk usage high: ${disk_pct}%"
        return 0
    else
        pass "Disk: ${disk_pct}% used"
        return 0
    fi
}

# -- Check: CPU temperature ----------------------------------------------------

check_temperature() {
    if [[ ! -f /sys/class/thermal/thermal_zone0/temp ]]; then
        return 0
    fi

    local temp_mc
    temp_mc="$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo "0")"
    local temp_c=$((temp_mc / 1000))

    if [[ "$temp_c" -gt 85 ]]; then
        fail "CPU temperature critical: ${temp_c}C"
        return 1
    elif [[ "$temp_c" -gt 75 ]]; then
        warn "CPU temperature high: ${temp_c}C"
        return 0
    else
        pass "CPU temperature: ${temp_c}C"
        return 0
    fi
}

# -- Check: File descriptors ---------------------------------------------------

check_fds() {
    local pid_file="${TRITIUM_PID_FILE:-/tmp/tritium-server.pid}"
    if [[ ! -f "$pid_file" ]]; then
        return 0
    fi

    local pid
    pid="$(cat "$pid_file" 2>/dev/null || echo "")"
    if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
        return 0
    fi

    local fd_count
    fd_count="$(ls -1 /proc/"$pid"/fd 2>/dev/null | wc -l || echo "0")"
    local fd_limit
    fd_limit="$(cat /proc/"$pid"/limits 2>/dev/null | awk '/Max open files/ {print $4}' || echo "65536")"

    local fd_pct=$((fd_count * 100 / fd_limit))
    if [[ "$fd_pct" -gt 90 ]]; then
        fail "File descriptors: ${fd_count}/${fd_limit} (${fd_pct}%)"
        return 1
    elif [[ "$fd_pct" -gt 75 ]]; then
        warn "File descriptors high: ${fd_count}/${fd_limit} (${fd_pct}%)"
        return 0
    else
        pass "File descriptors: ${fd_count}/${fd_limit}"
        return 0
    fi
}

# -- Main ----------------------------------------------------------------------

main() {
    if $VERBOSE; then
        echo "=== Tritium Edge Health Check ==="
        echo "Time: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "Health URL: $HEALTH_URL"
        echo ""
    fi

    check_http || true
    check_mqtt || true
    check_memory || true
    check_disk || true
    check_temperature || true
    check_fds || true

    if $VERBOSE; then
        echo ""
        echo "Results: $CHECKS_PASSED passed, $CHECKS_FAILED failed, $CHECKS_WARNED warned"
    fi

    if [[ "$CHECKS_FAILED" -gt 0 ]]; then
        if $VERBOSE; then echo "STATUS: UNHEALTHY"; fi
        exit 1
    else
        if $VERBOSE; then echo "STATUS: HEALTHY"; fi
        exit 0
    fi
}

main "$@"
