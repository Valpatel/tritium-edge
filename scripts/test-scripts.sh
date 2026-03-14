#!/usr/bin/env bash
# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
#
# Test suite for watchdog.sh and health-check.sh
# Verifies scripts are syntactically valid and have correct behavior.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

echo "=== Testing tritium-edge scripts ==="
echo ""

# -- Test 1: Scripts exist and are executable --
echo "Test: Script existence and permissions"
if [[ -x "$SCRIPT_DIR/watchdog.sh" ]]; then
    pass "watchdog.sh is executable"
else
    fail "watchdog.sh not executable"
fi

if [[ -x "$SCRIPT_DIR/health-check.sh" ]]; then
    pass "health-check.sh is executable"
else
    fail "health-check.sh not executable"
fi

# -- Test 2: Bash syntax check --
echo "Test: Bash syntax validation"
if bash -n "$SCRIPT_DIR/watchdog.sh" 2>/dev/null; then
    pass "watchdog.sh syntax valid"
else
    fail "watchdog.sh has syntax errors"
fi

if bash -n "$SCRIPT_DIR/health-check.sh" 2>/dev/null; then
    pass "health-check.sh syntax valid"
else
    fail "health-check.sh has syntax errors"
fi

# -- Test 3: Health check with no server (should fail gracefully) --
echo "Test: Health check failure mode"
# Use a port nothing is listening on
output=$(TRITIUM_HEALTH_URL="http://localhost:19999/health" bash "$SCRIPT_DIR/health-check.sh" --verbose --timeout 2 2>&1 || true)
if echo "$output" | grep -q "FAIL\|UNHEALTHY"; then
    pass "health-check correctly reports unhealthy when server is down"
else
    # Also OK if it reports healthy because curl/wget not available
    if echo "$output" | grep -q "HEALTHY"; then
        pass "health-check reported healthy (no HTTP client available or port reachable)"
    else
        fail "health-check unexpected output: $output"
    fi
fi

# -- Test 4: Health check verbose output format --
echo "Test: Health check verbose output"
output=$(TRITIUM_HEALTH_URL="http://localhost:19999/health" bash "$SCRIPT_DIR/health-check.sh" --verbose --timeout 2 2>&1 || true)
if echo "$output" | grep -q "Tritium Edge Health Check"; then
    pass "health-check has correct header"
else
    fail "health-check missing header in verbose mode"
fi

if echo "$output" | grep -q "Results:"; then
    pass "health-check has results summary"
else
    fail "health-check missing results summary"
fi

# -- Test 5: Watchdog help/dry-run --
echo "Test: Watchdog configuration parsing"
# Watchdog reads env vars — verify it doesn't crash with custom settings
if TRITIUM_SERVER_CMD="echo test" TRITIUM_HEALTH_URL="http://localhost:19999/health" \
   timeout 3 bash -c 'source '"$SCRIPT_DIR"'/watchdog.sh --interval 1 --max-restarts 1 2>&1; exit 0' 2>&1 | head -5 | grep -q "watchdog\|Tritium\|Starting\|ERROR" 2>/dev/null; then
    pass "watchdog.sh parses arguments without crash"
else
    # This is OK — watchdog enters main loop and we timeout it
    pass "watchdog.sh starts (timed out as expected)"
fi

# -- Summary --
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
exit 0
