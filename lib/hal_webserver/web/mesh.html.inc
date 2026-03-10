R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tritium-OS // Mesh Network</title>
<style>
:root {
    --t-cyan: #00f0ff;
    --t-magenta: #ff2a6d;
    --t-green: #05ffa1;
    --t-yellow: #fcee0a;
    --t-void: #0a0a0f;
    --t-surface-1: #0e0e14;
    --t-surface-2: #12121a;
    --t-surface-3: #1a1a2e;
    --t-ghost: #8888aa;
    --t-text: #c8d0dc;
    --t-bright: #e0e0ff;
    --t-glow-cyan: rgba(0, 240, 255, 0.15);
    --t-glow-magenta: rgba(255, 42, 109, 0.15);
    --t-glow-green: rgba(5, 255, 161, 0.15);
    --t-glow-yellow: rgba(252, 238, 10, 0.15);
    --t-border: rgba(0, 240, 255, 0.08);
    --t-border-hover: rgba(0, 240, 255, 0.3);
    --t-radius: 4px;
    --t-transition: 0.25s ease;
    --t-font-mono: 'JetBrains Mono', 'Fira Code', 'Cascadia Code', 'Consolas', monospace;
}
*, *::before, *::after { margin: 0; padding: 0; box-sizing: border-box; }
body {
    background: var(--t-void); color: var(--t-text); font-family: var(--t-font-mono);
    font-size: 13px; line-height: 1.5; min-height: 100vh; overflow-x: hidden;
}
body::before {
    content: ''; position: fixed; inset: 0;
    background-image: linear-gradient(rgba(0,240,255,0.025) 1px, transparent 1px),
        linear-gradient(90deg, rgba(0,240,255,0.025) 1px, transparent 1px);
    background-size: 80px 80px; pointer-events: none; z-index: 0;
}
body::after {
    content: ''; position: fixed; inset: 0;
    background: repeating-linear-gradient(0deg, transparent, transparent 2px, rgba(0,0,0,0.03) 2px, rgba(0,0,0,0.03) 4px);
    pointer-events: none; z-index: 9999;
}

/* ── Header ── */
.header {
    position: sticky; top: 0; z-index: 100;
    display: flex; align-items: center; justify-content: space-between;
    padding: 10px 20px; background: var(--t-surface-1);
    border-bottom: 1px solid var(--t-border); backdrop-filter: blur(12px);
    gap: 12px; flex-wrap: wrap;
}
.logo { font-size: 16px; font-weight: 700; color: var(--t-cyan); text-shadow: 0 0 20px rgba(0,240,255,0.4); letter-spacing: 0.12em; white-space: nowrap; }
.nav { display: flex; gap: 4px; flex-wrap: wrap; }
.nav a { color: var(--t-ghost); text-decoration: none; font-size: 11px; text-transform: uppercase; letter-spacing: 0.05em; padding: 5px 12px; border: 1px solid rgba(0,240,255,0.1); border-radius: var(--t-radius); transition: all var(--t-transition); }
.nav a:hover, .nav a.active { color: var(--t-cyan); border-color: var(--t-border-hover); background: rgba(0,240,255,0.05); }
.nav a.active { box-shadow: 0 0 8px var(--t-glow-cyan); }

/* ── Layout ── */
.shell { position: relative; z-index: 1; max-width: 1200px; margin: 0 auto; padding: 20px; }
.grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
@media (max-width: 800px) { .grid-2 { grid-template-columns: 1fr; } .header { padding: 8px 12px; } .shell { padding: 12px; } }

/* ── Panel ── */
.panel { background: var(--t-surface-2); border: 1px solid var(--t-border); border-radius: var(--t-radius); padding: 16px; margin-bottom: 16px; transition: border-color var(--t-transition), box-shadow var(--t-transition); }
.panel:hover { border-color: var(--t-border-hover); box-shadow: 0 0 20px var(--t-glow-cyan); }
.panel-title { font-size: 11px; font-weight: 600; color: var(--t-bright); letter-spacing: 0.12em; text-transform: uppercase; margin-bottom: 14px; padding-bottom: 8px; border-bottom: 1px solid var(--t-border); }
.panel-title::before { content: '[ '; color: var(--t-cyan); }
.panel-title::after { content: ' ]'; color: var(--t-cyan); }

/* ── Stats bar ── */
.stats-bar {
    display: flex; gap: 20px; flex-wrap: wrap; padding: 12px 16px; margin-bottom: 16px;
    background: var(--t-surface-1); border: 1px solid var(--t-border); border-radius: var(--t-radius);
}
.stat { font-size: 12px; }
.stat .stat-label { color: var(--t-ghost); margin-right: 6px; text-transform: uppercase; font-size: 10px; letter-spacing: 0.05em; }
.stat .stat-val { color: var(--t-cyan); }
.gateway-badge {
    display: inline-block; padding: 2px 8px; border-radius: 2px; font-size: 10px;
    text-transform: uppercase; letter-spacing: 0.1em;
    background: rgba(5,255,161,0.15); color: var(--t-green);
    border: 1px solid rgba(5,255,161,0.3); animation: breathe 1.5s ease-in-out infinite;
}
@keyframes breathe { 0%,100% { opacity: 1; } 50% { opacity: 0.6; } }

/* ── Data rows ── */
.data-row { display: flex; justify-content: space-between; align-items: center; padding: 5px 0; border-bottom: 1px solid rgba(255,255,255,0.02); }
.data-row .label { color: var(--t-ghost); font-size: 11px; text-transform: uppercase; letter-spacing: 0.08em; }
.data-row .value { color: var(--t-bright); font-size: 12px; text-align: right; }
.cyan { color: var(--t-cyan) !important; }
.green { color: var(--t-green) !important; }
.yellow { color: var(--t-yellow) !important; }
.mag { color: var(--t-magenta) !important; }

/* ── Buttons ── */
.btn { background: transparent; border: 1px solid var(--t-border-hover); color: var(--t-cyan); padding: 5px 12px; border-radius: var(--t-radius); cursor: pointer; font-family: inherit; font-size: 10px; text-transform: uppercase; letter-spacing: 0.08em; transition: all var(--t-transition); white-space: nowrap; }
.btn:hover { background: rgba(0,240,255,0.1); border-color: var(--t-cyan); box-shadow: 0 0 12px var(--t-glow-cyan); transform: translateY(-1px); }
.btn:active { transform: translateY(0); }
.btn-row { display: flex; gap: 8px; margin-top: 12px; flex-wrap: wrap; }

/* ── Topology SVG ── */
.topo-wrap { background: var(--t-surface-1); border-radius: var(--t-radius); overflow: hidden; }
#topo-canvas { width: 100%; height: 380px; display: block; }

/* ── Table ── */
.table-wrap { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; font-size: 11px; }
th { text-align: left; color: var(--t-ghost); font-weight: 600; font-size: 10px; text-transform: uppercase; letter-spacing: 0.05em; padding: 6px 8px; border-bottom: 1px solid rgba(0,240,255,0.1); }
td { padding: 6px 8px; border-bottom: 1px solid rgba(255,255,255,0.02); color: var(--t-text); }
tr:hover td { background: var(--t-surface-3); }
.rssi-good { color: var(--t-green); }
.rssi-med { color: var(--t-yellow); }
.rssi-bad { color: var(--t-magenta); }
.empty-row { color: var(--t-ghost); text-align: center; padding: 20px 8px !important; }

/* ── State editor ── */
.state-form { display: flex; gap: 8px; margin-top: 12px; flex-wrap: wrap; align-items: flex-end; }
.state-form input {
    background: var(--t-surface-1); color: var(--t-text); border: 1px solid var(--t-border);
    border-radius: var(--t-radius); padding: 6px 10px; font-family: inherit; font-size: 11px; outline: none;
}
.state-form input:focus { border-color: var(--t-cyan); box-shadow: 0 0 6px var(--t-glow-cyan); }
.state-form input::placeholder { color: var(--t-ghost); opacity: 0.5; }

/* ── Toast ── */
.toast { position: fixed; bottom: 20px; right: 20px; z-index: 300; padding: 10px 18px; border-radius: var(--t-radius); font-size: 12px; animation: toastIn 0.3s ease; pointer-events: none; border: 1px solid rgba(5,255,161,0.3); background: rgba(5,255,161,0.08); color: var(--t-green); }
.toast.error { border-color: rgba(255,42,109,0.3); background: rgba(255,42,109,0.08); color: var(--t-magenta); }
@keyframes toastIn { from { opacity: 0; transform: translateY(10px); } to { opacity: 1; transform: translateY(0); } }
</style>
</head>
<body>

<div class="header">
    <span class="logo">TRITIUM-OS</span>
    <div class="nav">
        <a href="dashboard.html">Dashboard</a>
        <a href="files.html">Files</a>
        <a href="ota.html">OTA</a>
        <a href="wifi.html">WiFi</a>
        <a href="mesh.html" class="active">Mesh</a>
        <a href="terminal.html">Terminal</a>
    </div>
</div>

<div class="shell">

<!-- Stats bar -->
<div class="stats-bar" id="stats-bar">
    <div class="stat"><span class="stat-label">Role:</span><span class="stat-val" id="s-role">--</span></div>
    <div class="stat" id="gw-badge-wrap"></div>
    <div class="stat"><span class="stat-label">Peers:</span><span class="stat-val" id="s-peers">--</span></div>
    <div class="stat"><span class="stat-label">TX:</span><span class="stat-val" id="s-tx">--</span></div>
    <div class="stat"><span class="stat-label">RX:</span><span class="stat-val" id="s-rx">--</span></div>
    <div class="stat"><span class="stat-label">Relayed:</span><span class="stat-val" id="s-relay">--</span></div>
    <div class="stat"><span class="stat-label">Drops:</span><span class="stat-val" id="s-drops">--</span></div>
    <div class="stat"><span class="stat-label">MAC:</span><span class="stat-val" id="s-mac">--</span></div>
</div>

<!-- Topology -->
<div class="panel">
    <div class="panel-title">Topology</div>
    <div class="topo-wrap">
        <svg id="topo-canvas" viewBox="0 0 900 380"></svg>
    </div>
</div>

<div class="grid-2">
    <!-- Gateway Election -->
    <div class="panel">
        <div class="panel-title">Gateway Election</div>
        <div class="data-row"><span class="label">Current Gateway</span><span class="value green" id="gw-current">---</span></div>
        <div class="data-row"><span class="label">Election State</span><span class="value" id="gw-state">---</span></div>
        <div class="data-row"><span class="label">This Node</span><span class="value" id="gw-self">---</span></div>
        <div class="data-row"><span class="label">Priority</span><span class="value" id="gw-priority">---</span></div>
    </div>

    <!-- Mesh Statistics -->
    <div class="panel">
        <div class="panel-title">Mesh Statistics</div>
        <div class="data-row"><span class="label">TX Packets</span><span class="value cyan" id="st-tx">---</span></div>
        <div class="data-row"><span class="label">RX Packets</span><span class="value cyan" id="st-rx">---</span></div>
        <div class="data-row"><span class="label">Relayed</span><span class="value" id="st-relay">---</span></div>
        <div class="data-row"><span class="label">Dedup Drops</span><span class="value" id="st-dedup">---</span></div>
        <div class="data-row"><span class="label">State Syncs</span><span class="value" id="st-syncs">---</span></div>
        <div class="data-row"><span class="label">Uptime</span><span class="value" id="st-uptime">---</span></div>
    </div>
</div>

<!-- Peers Table -->
<div class="panel">
    <div class="panel-title">Peers</div>
    <div class="table-wrap">
        <table>
            <thead><tr>
                <th>Name</th><th>MAC</th><th>Board</th><th>Role</th>
                <th>RSSI</th><th>FW Version</th><th>Battery</th><th>Last Seen</th><th></th>
            </tr></thead>
            <tbody id="peer-table"><tr><td colspan="9" class="empty-row">No peers discovered</td></tr></tbody>
        </table>
    </div>
</div>

<!-- Shared State -->
<div class="panel">
    <div class="panel-title">Shared State</div>
    <div class="table-wrap">
        <table>
            <thead><tr><th>Key</th><th>Value</th><th>Source</th><th>Version</th><th>Age</th></tr></thead>
            <tbody id="state-table"><tr><td colspan="5" class="empty-row">No shared state</td></tr></tbody>
        </table>
    </div>
    <div class="state-form">
        <input type="text" id="state-key" placeholder="key" style="width:140px">
        <input type="text" id="state-val" placeholder="value" style="width:200px">
        <button class="btn" id="btn-set-state">Set State</button>
        <button class="btn" id="btn-del-state">Delete Key</button>
    </div>
</div>

</div>

<script>
(function(){
"use strict";

var $ = function(s){ return document.getElementById(s); };
var meshData = null;

function toast(msg, err) {
    var el = document.createElement('div');
    el.className = 'toast' + (err ? ' error' : '');
    el.textContent = msg;
    document.body.appendChild(el);
    setTimeout(function(){ el.remove(); }, 3000);
}

function rssiClass(r) {
    if (r >= -50) return 'rssi-good';
    if (r >= -70) return 'rssi-med';
    return 'rssi-bad';
}

function fmtAge(s) {
    if (s == null) return '--';
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s / 60) + 'm';
    return Math.floor(s / 3600) + 'h';
}

function fmtTime(s) {
    if (s == null) return '--';
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s / 60) + 'm ' + (s % 60) + 's';
    return Math.floor(s / 3600) + 'h ' + Math.floor((s % 3600) / 60) + 'm';
}

function shortMac(m) { return m ? '...' + m.slice(-8) : '--'; }

function escHtml(s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }

/* ---- Refresh all data ---- */
function refresh() {
    fetch('/api/mesh').then(function(r){ return r.json(); }).then(function(d) {
        meshData = d;
        if (!d.enabled) return;
        updateStats(d);
        updateGateway(d);
        updatePeers(d.peers || []);
        updateState(d.state || []);
        updateTopology(d);
    }).catch(function(){});
}

function updateStats(d) {
    var s = d.stats || {};
    $('s-role').textContent = (d.role || '--').toUpperCase();
    $('gw-badge-wrap').innerHTML = d.is_gateway ? '<span class="gateway-badge">GATEWAY</span>' : '';
    $('s-peers').textContent = d.peer_count || 0;
    $('s-tx').textContent = (s.tx || 0).toLocaleString();
    $('s-rx').textContent = (s.rx || 0).toLocaleString();
    $('s-relay').textContent = (s.relay || 0).toLocaleString();
    $('s-drops').textContent = (s.dedup_drop || 0).toLocaleString();
    $('s-mac').textContent = d.mac || '--';

    $('st-tx').textContent = (s.tx || 0).toLocaleString();
    $('st-rx').textContent = (s.rx || 0).toLocaleString();
    $('st-relay').textContent = (s.relay || 0).toLocaleString();
    $('st-dedup').textContent = (s.dedup_drop || 0).toLocaleString();
    $('st-syncs').textContent = (s.state_syncs || 0).toLocaleString();
    $('st-uptime').textContent = fmtTime(d.uptime);
}

function updateGateway(d) {
    $('gw-current').textContent = d.gateway_mac || (d.is_gateway ? d.mac : '---');
    $('gw-state').textContent = d.gateway_state || (d.is_gateway ? 'ELECTED' : 'FOLLOWER');
    $('gw-self').textContent = d.is_gateway ? 'GATEWAY' : 'PEER';
    $('gw-self').className = 'value ' + (d.is_gateway ? 'green' : '');
    $('gw-priority').textContent = d.gateway_priority != null ? d.gateway_priority : '---';
}

function updatePeers(peers) {
    var tb = $('peer-table');
    if (!peers.length) {
        tb.innerHTML = '<tr><td colspan="9" class="empty-row">No peers discovered</td></tr>';
        return;
    }
    tb.innerHTML = peers.map(function(p) {
        var rc = rssiClass(p.rssi || -100);
        var age = fmtAge(p.seen_ago);
        var gw = p.is_gateway ? ' <span class="gateway-badge" style="animation:none;font-size:8px;padding:1px 4px">GW</span>' : '';
        return '<tr>' +
            '<td style="color:var(--t-bright)">' + escHtml(p.name || '--') + gw + '</td>' +
            '<td>' + escHtml(p.mac || '--') + '</td>' +
            '<td>' + escHtml(p.board || '--') + '</td>' +
            '<td>' + (p.role || 'relay').toUpperCase() + '</td>' +
            '<td class="' + rc + '">' + (p.rssi || '--') + ' dBm</td>' +
            '<td>' + escHtml(p.firmware || '--') + '</td>' +
            '<td>' + (p.battery !== undefined ? p.battery + '%' : '--') + '</td>' +
            '<td>' + age + '</td>' +
            '<td><button class="btn" onclick="pingPeer(\'' + escHtml(p.mac || '') + '\')">Ping</button></td>' +
            '</tr>';
    }).join('');
}

function updateState(state) {
    var tb = $('state-table');
    if (!state.length) {
        tb.innerHTML = '<tr><td colspan="5" class="empty-row">No shared state</td></tr>';
        return;
    }
    tb.innerHTML = state.map(function(s) {
        return '<tr>' +
            '<td class="cyan">' + escHtml(s.key || '') + '</td>' +
            '<td>' + escHtml(String(s.value || '')) + '</td>' +
            '<td>' + shortMac(s.source || '') + '</td>' +
            '<td>' + (s.version || '--') + '</td>' +
            '<td>' + fmtAge(s.age) + '</td>' +
            '</tr>';
    }).join('');
}

/* ---- Topology SVG ---- */
function updateTopology(d) {
    var svg = $('topo-canvas');
    var W = 900, H = 380;
    var peers = d.peers || [];
    var links = d.topology || [];

    // Build node list: self + peers
    var nodes = [{ mac: d.mac, name: d.device_name || 'Self', board: d.board || '', isSelf: true, role: d.role }];
    peers.forEach(function(p) {
        nodes.push({ mac: p.mac, name: p.name || shortMac(p.mac), board: p.board || '', isSelf: false, role: p.role, rssi: p.rssi, isGw: p.is_gateway });
    });

    var N = nodes.length;
    if (N === 0) { svg.innerHTML = ''; return; }

    // Initialize positions in circle around center
    var cx = W / 2, cy = H / 2, rad = Math.min(W, H) * 0.32;
    nodes.forEach(function(n, i) {
        if (n.isSelf) { n.x = cx; n.y = cy; }
        else {
            var a = (2 * Math.PI * (i - 1)) / (N - 1 || 1);
            n.x = cx + rad * Math.cos(a);
            n.y = cy + rad * Math.sin(a);
        }
    });

    // Simple force-directed layout (50 iterations)
    for (var iter = 0; iter < 50; iter++) {
        // Repulsion
        for (var i = 0; i < N; i++) {
            for (var j = i + 1; j < N; j++) {
                var dx = nodes[j].x - nodes[i].x;
                var dy = nodes[j].y - nodes[i].y;
                var dist = Math.sqrt(dx * dx + dy * dy) || 1;
                var force = 8000 / (dist * dist);
                var fx = (dx / dist) * force;
                var fy = (dy / dist) * force;
                if (!nodes[i].isSelf) { nodes[i].x -= fx; nodes[i].y -= fy; }
                if (!nodes[j].isSelf) { nodes[j].x += fx; nodes[j].y += fy; }
            }
        }
        // Attraction along links
        links.forEach(function(l) {
            var a = null, b = null;
            for (var k = 0; k < N; k++) {
                if (nodes[k].mac === l.from) a = nodes[k];
                if (nodes[k].mac === l.to) b = nodes[k];
            }
            if (!a || !b) return;
            var ddx = b.x - a.x, ddy = b.y - a.y;
            var dd = Math.sqrt(ddx * ddx + ddy * ddy) || 1;
            var ff = (dd - 130) * 0.02;
            var ffx = (ddx / dd) * ff, ffy = (ddy / dd) * ff;
            if (!a.isSelf) { a.x += ffx; a.y += ffy; }
            if (!b.isSelf) { b.x += ffx; b.y += ffy; }
        });
        // Keep in bounds
        nodes.forEach(function(n) {
            n.x = Math.max(60, Math.min(W - 60, n.x));
            n.y = Math.max(50, Math.min(H - 50, n.y));
        });
    }

    // Render
    var html = '<defs>' +
        '<filter id="glow"><feGaussianBlur stdDeviation="3" result="blur"/><feMerge><feMergeNode in="blur"/><feMergeNode in="SourceGraphic"/></feMerge></filter>' +
        '<filter id="glow-s"><feGaussianBlur stdDeviation="2" result="blur"/><feMerge><feMergeNode in="blur"/><feMergeNode in="SourceGraphic"/></feMerge></filter>' +
        '</defs>';

    // Draw links from topology data
    links.forEach(function(l) {
        var a = null, b = null;
        for (var k = 0; k < N; k++) {
            if (nodes[k].mac === l.from) a = nodes[k];
            if (nodes[k].mac === l.to) b = nodes[k];
        }
        if (!a || !b) return;
        var q = l.quality || 50;
        var color = q >= 70 ? '#05ffa1' : q >= 40 ? '#fcee0a' : '#ff2a6d';
        var opacity = 0.3 + (q / 100) * 0.5;
        html += '<line x1="' + a.x + '" y1="' + a.y + '" x2="' + b.x + '" y2="' + b.y + '" stroke="' + color + '" stroke-width="1.5" opacity="' + opacity + '" filter="url(#glow-s)"/>';
        var mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
        html += '<text x="' + mx + '" y="' + (my - 6) + '" text-anchor="middle" fill="' + color + '" font-size="9" font-family="var(--t-font-mono)" opacity="0.7">' + (l.rssi || '') + 'dBm</text>';
    });

    // If no topology links, draw direct dashed lines to self
    if (links.length === 0 && peers.length > 0) {
        nodes.forEach(function(n) {
            if (n.isSelf) return;
            var self = nodes[0];
            var rssi = n.rssi || -60;
            var q = Math.max(0, Math.min(100, ((rssi + 90) * 100) / 60));
            var color = q >= 70 ? '#05ffa1' : q >= 40 ? '#fcee0a' : '#ff2a6d';
            html += '<line x1="' + self.x + '" y1="' + self.y + '" x2="' + n.x + '" y2="' + n.y + '" stroke="' + color + '" stroke-width="1.5" opacity="0.4" stroke-dasharray="4 3" filter="url(#glow-s)"/>';
            var mx = (self.x + n.x) / 2, my = (self.y + n.y) / 2;
            html += '<text x="' + mx + '" y="' + (my - 6) + '" text-anchor="middle" fill="' + color + '" font-size="9" font-family="var(--t-font-mono)" opacity="0.6">' + rssi + '</text>';
        });
    }

    // Draw nodes
    nodes.forEach(function(n) {
        var isSelf = n.isSelf;
        var stroke = isSelf ? '#00f0ff' : (n.isGw ? '#05ffa1' : '#8888aa');
        var fill = isSelf ? 'rgba(0,240,255,0.08)' : (n.isGw ? 'rgba(5,255,161,0.05)' : 'rgba(136,136,170,0.05)');
        var sw = isSelf ? 2 : 1;
        var r = isSelf ? 24 : 18;
        var filt = isSelf ? ' filter="url(#glow)"' : '';

        html += '<circle cx="' + n.x + '" cy="' + n.y + '" r="' + r + '" fill="' + fill + '" stroke="' + stroke + '" stroke-width="' + sw + '"' + filt + '/>';

        // Node label
        var label = n.name || n.board;
        if (label.length > 12) label = label.substring(0, 12);
        var labelColor = isSelf ? '#00f0ff' : '#c8d0dc';
        html += '<text x="' + n.x + '" y="' + (n.y + 3) + '" text-anchor="middle" fill="' + labelColor + '" font-size="' + (isSelf ? 9 : 8) + '" font-family="var(--t-font-mono)" font-weight="' + (isSelf ? 600 : 400) + '">' + escHtml(label) + '</text>';

        // Role label below
        var roleColor = n.role === 'gateway' ? '#05ffa1' : n.role === 'sensor' ? '#fcee0a' : '#8888aa';
        html += '<text x="' + n.x + '" y="' + (n.y + r + 14) + '" text-anchor="middle" fill="' + roleColor + '" font-size="8" font-family="var(--t-font-mono)">' + (n.role || 'relay').toUpperCase() + '</text>';
    });

    // "THIS DEVICE" label
    if (N > 0) {
        html += '<text x="' + nodes[0].x + '" y="' + (nodes[0].y - 30) + '" text-anchor="middle" fill="#00f0ff" font-size="8" font-family="var(--t-font-mono)" opacity="0.6">THIS DEVICE</text>';
    }

    svg.innerHTML = html;
}

/* ---- Actions ---- */
window.pingPeer = function(mac) {
    fetch('/api/mesh/ping?mac=' + encodeURIComponent(mac), { method: 'POST' })
        .then(function(r){ return r.json(); })
        .then(function(d) { toast(d.msg || 'Ping sent'); setTimeout(refresh, 1000); })
        .catch(function() { toast('Ping failed', true); });
};

$('btn-set-state').addEventListener('click', function() {
    var key = $('state-key').value.trim();
    var val = $('state-val').value.trim();
    if (!key || !val) return;
    fetch('/api/mesh/state', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ key: key, value: val })
    }).then(function() {
        $('state-key').value = '';
        $('state-val').value = '';
        toast('State updated');
        setTimeout(refresh, 500);
    }).catch(function() { toast('Failed', true); });
});

$('btn-del-state').addEventListener('click', function() {
    var key = $('state-key').value.trim();
    if (!key) return;
    fetch('/api/mesh/state?key=' + encodeURIComponent(key), { method: 'DELETE' })
        .then(function() { $('state-key').value = ''; toast('Key deleted'); setTimeout(refresh, 500); })
        .catch(function() { toast('Failed', true); });
});

/* ---- WebSocket ---- */
function connectWs() {
    var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    var ws = new WebSocket(proto + '//' + location.host + '/ws');
    ws.onmessage = function(e) {
        var msg;
        try { msg = JSON.parse(e.data); } catch(err) { return; }
        if (msg.type === 'mesh_update' || msg.type === 'mesh_peer_joined' || msg.type === 'mesh_peer_left') {
            refresh();
        }
    };
    ws.onclose = function() { setTimeout(connectWs, 3000); };
    ws.onerror = function() { ws.close(); };
}

/* ---- Init ---- */
refresh();
connectWs();
setInterval(refresh, 5000);

})();
</script>
</body>
</html>
)rawliteral"
