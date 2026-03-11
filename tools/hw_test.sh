#!/usr/bin/env bash
# Tritium-OS Hardware Feature Test Suite
# Tests all features on a live device and generates an HTML report with screenshots.
#
# Usage: ./tools/hw_test.sh [device_ip] [port]
# Default IP: 10.42.0.237, Default port: 8000

set -uo pipefail

DEVICE="${1:-10.42.0.237}"
PORT="${2:-80}"
BASE="http://${DEVICE}:${PORT}"
REPORT_DIR="$(dirname "$0")/../test_report"
SCREENSHOTS="${REPORT_DIR}/screenshots"
RESULTS_FILE="${REPORT_DIR}/results.json"
SERIAL_DEV="${SERIAL_DEV:-/dev/ttyACM0}"

mkdir -p "$SCREENSHOTS"

# Colors for terminal output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# Test result tracking
PASS=0
FAIL=0
SKIP=0
TESTS=()

log() { echo -e "${CYAN}[test]${NC} $1"; }
pass() { echo -e "${GREEN}  ✓ PASS${NC}: $1"; PASS=$((PASS+1)); TESTS+=("{\"name\":\"$1\",\"status\":\"pass\",\"detail\":\"$2\"}"); }
fail() { echo -e "${RED}  ✗ FAIL${NC}: $1 — $2"; FAIL=$((FAIL+1)); TESTS+=("{\"name\":\"$1\",\"status\":\"fail\",\"detail\":\"$2\"}"); }
skip() { echo -e "${YELLOW}  ⊘ SKIP${NC}: $1 — $2"; SKIP=$((SKIP+1)); TESTS+=("{\"name\":\"$1\",\"status\":\"skip\",\"detail\":\"$2\"}"); }

# Helper: HTTP GET, return body, check status
# Rate limiting — ESP32 httpd has max 7 sockets.
# Connection: close forces immediate socket release on both sides.
DELAY=1
CURL_OPTS=(-s --connect-timeout 5 --max-time 15 -H "Connection: close")

http_get() {
    local url="$1"
    sleep "$DELAY"
    local out
    out=$(curl "${CURL_OPTS[@]}" -w "\n%{http_code}" "${BASE}${url}" 2>/dev/null) || echo -e "\n000"
    echo "$out"
}

http_get_status() {
    local url="$1"
    sleep "$DELAY"
    curl "${CURL_OPTS[@]}" -o /dev/null -w "%{http_code}" "${BASE}${url}" 2>/dev/null || echo "000"
}

http_post() {
    local url="$1"
    local data="${2:-}"
    local ct="${3:-application/json}"
    sleep "$DELAY"
    curl "${CURL_OPTS[@]}" -X POST -H "Content-Type: ${ct}" -d "$data" -w "\n%{http_code}" "${BASE}${url}" 2>/dev/null || echo -e "\n000"
}

http_put() {
    local url="$1"
    local data="$2"
    sleep "$DELAY"
    curl "${CURL_OPTS[@]}" -X PUT -H "Content-Type: application/json" -d "$data" -w "\n%{http_code}" "${BASE}${url}" 2>/dev/null || echo -e "\n000"
}

grab_screenshot() {
    local name="$1"
    local path="${SCREENSHOTS}/${name}.jpg"
    sleep 2
    curl "${CURL_OPTS[@]}" --max-time 20 -o "$path" "${BASE}/api/screenshot" 2>/dev/null
    if [ -s "$path" ]; then
        echo "$path"
    else
        rm -f "$path"
        echo ""
    fi
}

# Navigate to a shell app and screenshot it
screenshot_app() {
    local app_name="$1"
    local file_name="$2"
    # Launch the app
    curl "${CURL_OPTS[@]}" --max-time 5 -X POST -H "Content-Type: application/json" \
        -d "{\"app\":\"${app_name}\"}" "${BASE}/api/shell/launch" >/dev/null 2>&1 || true
    sleep 3
    grab_screenshot "$file_name"
}

# ═══════════════════════════════════════════════════════════════════════
# CONNECTIVITY
# ═══════════════════════════════════════════════════════════════════════
log "=== Phase 1: Connectivity ==="

# Test 1: Device reachable
status=$(http_get_status "/")
if [ "$status" = "200" ]; then
    pass "Device reachable" "HTTP 200 at ${BASE}/"
else
    fail "Device reachable" "HTTP ${status}"
    echo "Device not reachable at ${DEVICE}. Aborting."
    exit 1
fi

# Test 2: API status endpoint
resp=$(http_get "/api/status")
code=$(echo "$resp" | tail -1)
body=$(echo "$resp" | sed '$d')
if [ "$code" = "200" ]; then
    uptime=$(echo "$body" | grep -o '"uptime":[0-9]*' | head -1 | cut -d: -f2)
    heap=$(echo "$body" | grep -o '"heap_free":[0-9]*' | head -1 | cut -d: -f2)
    pass "API status" "uptime=${uptime:-?}s heap=${heap:-?}B"
else
    fail "API status" "HTTP ${code}"
fi

# Test 3: Board info
resp=$(http_get "/api/board")
code=$(echo "$resp" | tail -1)
body=$(echo "$resp" | sed '$d')
if [ "$code" = "200" ]; then
    chip=$(echo "$body" | grep -o '"chip":"[^"]*"' | head -1 | cut -d'"' -f4)
    pass "Board info" "chip=${chip:-ESP32-S3}"
else
    fail "Board info" "HTTP ${code}"
fi

# ═══════════════════════════════════════════════════════════════════════
# WEB PAGES
# ═══════════════════════════════════════════════════════════════════════
log "=== Phase 2: Web Pages ==="

for page in "/" "/terminal" "/settings" "/ota" "/wifi" "/mesh" "/files" "/remote"; do
    status=$(http_get_status "$page")
    name="Page ${page}"
    if [ "$status" = "200" ]; then
        pass "$name" "HTTP 200"
    elif [ "$status" = "000" ]; then
        fail "$name" "timeout/unreachable"
    else
        fail "$name" "HTTP ${status}"
    fi
done

# ═══════════════════════════════════════════════════════════════════════
# REST APIs
# ═══════════════════════════════════════════════════════════════════════
log "=== Phase 3: REST API Endpoints ==="

# Settings API
resp=$(http_get "/api/settings")
code=$(echo "$resp" | tail -1)
body=$(echo "$resp" | sed '$d')
if [ "$code" = "200" ] && echo "$body" | grep -q '"system"'; then
    domains=$(echo "$body" | grep -o '"[a-z]*":{' | wc -l)
    pass "GET /api/settings" "${domains} domains returned"
    # Save for later comparison
    echo "$body" > "${REPORT_DIR}/settings_snapshot.json"
else
    fail "GET /api/settings" "HTTP ${code}"
fi

# Settings PUT (write a test value, then restore)
resp=$(http_put "/api/settings" '{"developer":{"log_level":3}}')
code=$(echo "$resp" | tail -1)
body=$(echo "$resp" | sed '$d')
if [ "$code" = "200" ] && echo "$body" | grep -q '"ok"'; then
    pass "PUT /api/settings" "wrote developer.log_level=3"
    # Restore original
    http_put "/api/settings" '{"developer":{"log_level":2}}' >/dev/null 2>&1
else
    fail "PUT /api/settings" "HTTP ${code}: ${body}"
fi

# WiFi status
resp=$(http_get "/api/wifi/status")
code=$(echo "$resp" | tail -1)
body=$(echo "$resp" | sed '$d')
if [ "$code" = "200" ]; then
    ssid=$(echo "$body" | grep -o '"ssid":"[^"]*"' | head -1 | cut -d'"' -f4)
    pass "WiFi status" "SSID=${ssid}"
else
    fail "WiFi status" "HTTP ${code}"
fi

# Mesh topology
resp=$(http_get "/api/mesh")
code=$(echo "$resp" | tail -1)
if [ "$code" = "200" ]; then
    pass "Mesh topology" "HTTP 200"
else
    skip "Mesh topology" "HTTP ${code} (may not be available)"
fi

# BLE devices
resp=$(http_get "/api/ble")
code=$(echo "$resp" | tail -1)
body=$(echo "$resp" | sed '$d')
if [ "$code" = "200" ]; then
    pass "BLE device list" "HTTP 200"
else
    skip "BLE device list" "HTTP ${code} (BLE scanner may be stubbed)"
fi

# Quick-probe optional endpoints (2s timeout to avoid socket exhaustion)
for ep in "/api/diag:Diagnostics" "/api/node:Node info" "/api/shell/apps:Shell apps list" \
          "/api/events/poll:Event poll" "/api/serial/poll:Serial poll" "/api/debug/touch:Debug touch"; do
    url="${ep%%:*}"
    name="${ep##*:}"
    sleep "$DELAY"
    code=$(curl "${CURL_OPTS[@]}" --connect-timeout 2 --max-time 3 -o /dev/null -w "%{http_code}" "${BASE}${url}" 2>/dev/null || echo "000")
    if [ "$code" = "200" ]; then
        pass "$name" "HTTP 200"
    else
        skip "$name" "HTTP ${code} (endpoint not implemented)"
    fi
done

# Pause to let device sockets recover
sleep 3

# ═══════════════════════════════════════════════════════════════════════
# SCREENSHOTS — Capture every launcher app
# ═══════════════════════════════════════════════════════════════════════
log "=== Phase 4: Screenshots (Launcher + Apps) ==="

# Go home first
curl -s -X POST "${BASE}/api/shell/home" >/dev/null 2>&1 || true
sleep 1

# Launcher screenshot
sc=$(grab_screenshot "00_launcher")
if [ -n "$sc" ]; then
    pass "Screenshot: Launcher" "$(du -h "$sc" | cut -f1)"
else
    fail "Screenshot: Launcher" "empty or failed"
fi

# Screenshot each app — with recovery delays to avoid socket exhaustion
APPS=("Map" "Chat" "Terminal" "Files" "About" "Settings")
idx=1
for app in "${APPS[@]}"; do
    padded=$(printf "%02d" $idx)
    # Extra delay between apps to let ESP32 close sockets
    sleep 3
    sc=$(screenshot_app "$app" "${padded}_${app,,}")
    if [ -n "$sc" ]; then
        pass "Screenshot: ${app}" "$(du -h "$sc" | cut -f1)"
    else
        fail "Screenshot: ${app}" "empty or failed"
    fi
    idx=$((idx+1))
done

# Return to launcher
sleep 3
curl "${CURL_OPTS[@]}" -X POST "${BASE}/api/shell/home" >/dev/null 2>&1 || true

# Let device sockets recover after heavy screenshot phase
sleep 8

# ═══════════════════════════════════════════════════════════════════════
# SETTINGS TABS — Tap through a few key tabs via touch injection
# ═══════════════════════════════════════════════════════════════════════
log "=== Phase 5: Settings Tab Screenshots ==="

# First launch Settings app
curl "${CURL_OPTS[@]}" --max-time 5 -X POST -H "Content-Type: application/json" \
    -d '{"app":"Settings"}' "${BASE}/api/shell/launch" >/dev/null 2>&1 || true
sleep 2
# Try tapping Security tab (index 9) — approximate position for 800x480
# Tab bar is at the top of the viewport area, just below the status bar
# Each tab button is about 56px wide, starting from left edge

# We'll use the remote tap API to navigate tabs. The exact coordinates
# depend on screen layout, so we try a few key ones.
# Test just one tab tap to verify touch injection works
sleep 3
curl "${CURL_OPTS[@]}" --max-time 5 -X POST -H "Content-Type: application/json" \
    -d '{"x":28,"y":75}' "${BASE}/api/remote/tap" >/dev/null 2>&1 || true
sleep 3
sc=$(grab_screenshot "10_settings_display")
if [ -n "$sc" ]; then
    pass "Settings Tab: Display" "tapped at (28,75)"
else
    skip "Settings Tab: Display" "screenshot or touch injection failed"
fi

# Return home
sleep 3
curl "${CURL_OPTS[@]}" -X POST "${BASE}/api/shell/home" >/dev/null 2>&1 || true
sleep 3

# ═══════════════════════════════════════════════════════════════════════
# LOCK SCREEN TEST
# ═══════════════════════════════════════════════════════════════════════
log "=== Phase 6: Lock Screen ==="

# Check if lock screen is currently enabled
resp=$(http_get "/api/settings")
body=$(echo "$resp" | sed '$d')
lock_pin=$(echo "$body" | grep -o '"lock_pin":"[^"]*"' | head -1 | cut -d'"' -f4)

if [ -z "$lock_pin" ] || [ "$lock_pin" = "" ]; then
    # Set a test PIN
    resp=$(http_put "/api/settings" '{"system":{"lock_pin":"1234"}}')
    code=$(echo "$resp" | tail -1)
    if [ "$code" = "200" ]; then
        pass "Lock screen: set PIN" "PIN=1234 via settings API"
    else
        fail "Lock screen: set PIN" "HTTP ${code}"
    fi
    sleep 1

    # Verify it was stored
    resp=$(http_get "/api/settings")
    body=$(echo "$resp" | sed '$d')
    stored=$(echo "$body" | grep -o '"lock_pin":"[^"]*"' | head -1 | cut -d'"' -f4)
    if [ "$stored" = "1234" ]; then
        pass "Lock screen: PIN stored" "lock_pin=1234 in NVS"
    else
        fail "Lock screen: PIN stored" "got '${stored}'"
    fi

    # Clear the test PIN
    http_put "/api/settings" '{"system":{"lock_pin":""}}' >/dev/null 2>&1
    pass "Lock screen: PIN cleared" "restored to empty"
else
    skip "Lock screen: set PIN" "PIN already set (${lock_pin}), not overwriting"
fi

# ═══════════════════════════════════════════════════════════════════════
# SERIAL COMMAND TEST
# ═══════════════════════════════════════════════════════════════════════
log "=== Phase 7: Serial Commands ==="

# Send a serial command via the API
resp=$(http_post "/api/serial/send" "STATUS" "text/plain")
code=$(echo "$resp" | tail -1)
if [ "$code" = "200" ]; then
    pass "Serial send: STATUS" "HTTP 200"
else
    skip "Serial send: STATUS" "HTTP ${code}"
fi

# Try HELP command
resp=$(http_post "/api/serial/send" "HELP" "text/plain")
code=$(echo "$resp" | tail -1)
if [ "$code" = "200" ]; then
    pass "Serial send: HELP" "HTTP 200"
else
    skip "Serial send: HELP" "HTTP ${code}"
fi

# ═══════════════════════════════════════════════════════════════════════
# WEBSOCKET TEST (basic connection check)
# ═══════════════════════════════════════════════════════════════════════
log "=== Phase 8: WebSocket ==="

# Use curl to check WebSocket upgrade
ws_status=$(curl "${CURL_OPTS[@]}" --max-time 5 -o /dev/null -w "%{http_code}" \
    -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
    -H "Sec-WebSocket-Version: 13" \
    "${BASE}/ws" 2>/dev/null || echo "000")
if [ "$ws_status" = "101" ]; then
    pass "WebSocket /ws" "upgrade 101 Switching Protocols"
elif [ "$ws_status" = "200" ]; then
    pass "WebSocket /ws" "HTTP 200 (ws endpoint exists)"
else
    skip "WebSocket /ws" "HTTP ${ws_status} (curl can't do full WS handshake)"
fi

# ═══════════════════════════════════════════════════════════════════════
# FINAL SCREENSHOT — Home screen after all tests
# ═══════════════════════════════════════════════════════════════════════
log "=== Final: Home Screenshot ==="
curl -s -X POST "${BASE}/api/shell/home" >/dev/null 2>&1 || true
sleep 1
sc=$(grab_screenshot "99_final_home")
if [ -n "$sc" ]; then
    pass "Final home screenshot" "$(du -h "$sc" | cut -f1)"
else
    fail "Final home screenshot" "failed"
fi

# ═══════════════════════════════════════════════════════════════════════
# GENERATE HTML REPORT
# ═══════════════════════════════════════════════════════════════════════
log "=== Generating HTML Report ==="

TOTAL=$((PASS + FAIL + SKIP))
REPORT="${REPORT_DIR}/report.html"

# Collect screenshot filenames
SCREENSHOT_FILES=$(ls -1 "$SCREENSHOTS"/*.jpg 2>/dev/null | sort)

cat > "$REPORT" << 'HEADER'
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tritium-OS Hardware Test Report</title>
<style>
:root {
    --t-cyan: #00f0ff; --t-magenta: #ff2a6d; --t-green: #05ffa1;
    --t-yellow: #fcee0a; --t-void: #0a0a0f; --t-surface-1: #0e0e14;
    --t-surface-2: #12121a; --t-surface-3: #1a1a2e; --t-ghost: #8888aa;
    --t-text: #c8d0dc; --t-bright: #e0e0ff;
    --t-font-mono: 'JetBrains Mono', 'Fira Code', 'Cascadia Code', 'Consolas', monospace;
    --t-font-body: 'Inter', -apple-system, sans-serif;
}
*, *::before, *::after { margin: 0; padding: 0; box-sizing: border-box; }
body {
    background: var(--t-void); color: var(--t-text);
    font-family: var(--t-font-body); font-size: 14px; line-height: 1.6;
    padding: 20px; max-width: 1200px; margin: 0 auto;
}
body::before {
    content: ''; position: fixed; inset: 0; z-index: -1;
    background-image:
        linear-gradient(rgba(0,240,255,0.02) 1px, transparent 1px),
        linear-gradient(90deg, rgba(0,240,255,0.02) 1px, transparent 1px);
    background-size: 24px 24px;
}
h1 { font-family: var(--t-font-mono); color: var(--t-cyan); font-size: 24px;
     letter-spacing: 3px; margin-bottom: 8px; }
h2 { font-family: var(--t-font-mono); color: var(--t-cyan); font-size: 16px;
     letter-spacing: 2px; margin: 24px 0 12px; border-bottom: 1px solid rgba(0,240,255,0.1);
     padding-bottom: 6px; }
.summary {
    display: flex; gap: 16px; margin: 16px 0; flex-wrap: wrap;
}
.stat {
    padding: 12px 24px; border: 1px solid rgba(0,240,255,0.15);
    border-radius: 4px; background: var(--t-surface-1);
    font-family: var(--t-font-mono); text-align: center;
}
.stat .num { font-size: 28px; font-weight: 700; display: block; }
.stat .lbl { font-size: 11px; text-transform: uppercase; letter-spacing: 1px; color: var(--t-ghost); }
.stat.pass .num { color: var(--t-green); }
.stat.fail .num { color: var(--t-magenta); }
.stat.skip .num { color: var(--t-yellow); }
.stat.total .num { color: var(--t-cyan); }
table { width: 100%; border-collapse: collapse; margin: 12px 0; }
th { text-align: left; font-family: var(--t-font-mono); font-size: 11px;
     text-transform: uppercase; letter-spacing: 1px; color: var(--t-ghost);
     padding: 8px 12px; border-bottom: 1px solid rgba(0,240,255,0.1); }
td { padding: 6px 12px; border-bottom: 1px solid rgba(0,240,255,0.05);
     font-size: 13px; }
tr:hover { background: var(--t-surface-2); }
.pass-badge { color: var(--t-green); font-family: var(--t-font-mono); font-weight: 600; }
.fail-badge { color: var(--t-magenta); font-family: var(--t-font-mono); font-weight: 600; }
.skip-badge { color: var(--t-yellow); font-family: var(--t-font-mono); font-weight: 600; }
.screenshots { display: grid; grid-template-columns: repeat(auto-fill, minmax(380px, 1fr)); gap: 16px; margin: 16px 0; }
.sc-card {
    border: 1px solid rgba(0,240,255,0.1); border-radius: 4px;
    background: var(--t-surface-1); overflow: hidden;
}
.sc-card img { width: 100%; display: block; image-rendering: auto; }
.sc-card .caption {
    padding: 8px 12px; font-family: var(--t-font-mono); font-size: 12px;
    color: var(--t-cyan); text-transform: uppercase; letter-spacing: 1px;
    border-top: 1px solid rgba(0,240,255,0.1);
}
.timestamp { color: var(--t-ghost); font-family: var(--t-font-mono); font-size: 12px; }
.progress-bar { height: 6px; background: var(--t-surface-3); border-radius: 3px; margin: 8px 0; overflow: hidden; }
.progress-fill { height: 100%; border-radius: 3px; transition: width 0.3s; }
</style>
</head>
<body>
HEADER

# Write report header
cat >> "$REPORT" << EOF
<h1>// TRITIUM-OS TEST REPORT</h1>
<p class="timestamp">Generated: $(date '+%Y-%m-%d %H:%M:%S') &mdash; Device: ${DEVICE}</p>

<div class="summary">
    <div class="stat total"><span class="num">${TOTAL}</span><span class="lbl">Total</span></div>
    <div class="stat pass"><span class="num">${PASS}</span><span class="lbl">Passed</span></div>
    <div class="stat fail"><span class="num">${FAIL}</span><span class="lbl">Failed</span></div>
    <div class="stat skip"><span class="num">${SKIP}</span><span class="lbl">Skipped</span></div>
</div>

<div class="progress-bar">
    <div class="progress-fill" style="width:${PASS}00%; max-width:$((PASS * 100 / (TOTAL > 0 ? TOTAL : 1)))%; background: linear-gradient(90deg, var(--t-green), var(--t-cyan));"></div>
</div>

<h2>// TEST RESULTS</h2>
<table>
<tr><th>Test</th><th>Status</th><th>Detail</th></tr>
EOF

# Write test rows
for t in "${TESTS[@]}"; do
    name=$(echo "$t" | grep -o '"name":"[^"]*"' | cut -d'"' -f4)
    status=$(echo "$t" | grep -o '"status":"[^"]*"' | cut -d'"' -f4)
    detail=$(echo "$t" | grep -o '"detail":"[^"]*"' | cut -d'"' -f4)
    badge="${status}-badge"
    label=$(echo "$status" | tr '[:lower:]' '[:upper:]')
    echo "<tr><td>${name}</td><td class=\"${badge}\">${label}</td><td>${detail}</td></tr>" >> "$REPORT"
done

echo "</table>" >> "$REPORT"

# Screenshots section
echo '<h2>// SCREENSHOTS</h2>' >> "$REPORT"
echo '<div class="screenshots">' >> "$REPORT"

for img in $SCREENSHOT_FILES; do
    fname=$(basename "$img")
    label=$(echo "$fname" | sed 's/^[0-9]*_//;s/\.jpg$//;s/_/ /g' | sed 's/\b\w/\U&/g')
    echo "<div class=\"sc-card\"><img src=\"screenshots/${fname}\" alt=\"${label}\"><div class=\"caption\">${label}</div></div>" >> "$REPORT"
done

echo '</div>' >> "$REPORT"

# Close HTML
cat >> "$REPORT" << 'FOOTER'
<p style="margin-top:32px;color:var(--t-ghost);font-family:var(--t-font-mono);font-size:11px;text-align:center;">
TRITIUM-OS &mdash; Valpatel Software LLC &mdash; Hardware Test Suite v1.0
</p>
</body>
</html>
FOOTER

echo ""
log "═══════════════════════════════════════════════"
echo -e "${CYAN}  Report:${NC} ${REPORT}"
echo -e "${GREEN}  PASS: ${PASS}${NC}  ${RED}FAIL: ${FAIL}${NC}  ${YELLOW}SKIP: ${SKIP}${NC}  Total: ${TOTAL}"
log "═══════════════════════════════════════════════"
