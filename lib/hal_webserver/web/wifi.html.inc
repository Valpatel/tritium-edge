R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tritium-OS // WiFi Config</title>
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
.shell { position: relative; z-index: 1; max-width: 1000px; margin: 0 auto; padding: 20px; }
.grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
@media (max-width: 700px) { .grid-2 { grid-template-columns: 1fr; } .header { padding: 8px 12px; } .shell { padding: 12px; } }

/* ── Panel ── */
.panel { background: var(--t-surface-2); border: 1px solid var(--t-border); border-radius: var(--t-radius); padding: 16px; margin-bottom: 16px; transition: border-color var(--t-transition), box-shadow var(--t-transition); }
.panel:hover { border-color: var(--t-border-hover); box-shadow: 0 0 20px var(--t-glow-cyan); }
.panel-title { font-size: 11px; font-weight: 600; color: var(--t-bright); letter-spacing: 0.12em; text-transform: uppercase; margin-bottom: 14px; padding-bottom: 8px; border-bottom: 1px solid var(--t-border); }
.panel-title::before { content: '[ '; color: var(--t-cyan); }
.panel-title::after { content: ' ]'; color: var(--t-cyan); }

/* ── Data rows ── */
.data-row { display: flex; justify-content: space-between; align-items: center; padding: 5px 0; border-bottom: 1px solid rgba(255,255,255,0.02); }
.data-row .label { color: var(--t-ghost); font-size: 11px; text-transform: uppercase; letter-spacing: 0.08em; }
.data-row .value { color: var(--t-bright); font-size: 12px; text-align: right; }
.cyan { color: var(--t-cyan) !important; }
.green { color: var(--t-green) !important; }
.yellow { color: var(--t-yellow) !important; }
.mag { color: var(--t-magenta) !important; }

/* ── Buttons ── */
.btn { background: transparent; border: 1px solid var(--t-border-hover); color: var(--t-cyan); padding: 7px 16px; border-radius: var(--t-radius); cursor: pointer; font-family: inherit; font-size: 11px; text-transform: uppercase; letter-spacing: 0.08em; transition: all var(--t-transition); white-space: nowrap; }
.btn:hover { background: rgba(0,240,255,0.1); border-color: var(--t-cyan); box-shadow: 0 0 12px var(--t-glow-cyan); transform: translateY(-1px); }
.btn.danger { border-color: rgba(255,42,109,0.3); color: var(--t-magenta); }
.btn.danger:hover { background: rgba(255,42,109,0.1); border-color: var(--t-magenta); box-shadow: 0 0 12px var(--t-glow-magenta); }
.btn.green { border-color: rgba(5,255,161,0.3); color: var(--t-green); }
.btn.green:hover { background: rgba(5,255,161,0.1); border-color: var(--t-green); box-shadow: 0 0 12px var(--t-glow-green); }
.btn:disabled { opacity: 0.3; cursor: not-allowed; transform: none !important; box-shadow: none !important; }
.btn.sm { padding: 4px 10px; font-size: 10px; }
.btn-row { display: flex; gap: 8px; margin-top: 12px; flex-wrap: wrap; }

/* ── Connection dot ── */
.conn-dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; vertical-align: middle; transition: all var(--t-transition); }
.conn-dot.on { background: var(--t-green); box-shadow: 0 0 8px var(--t-glow-green); animation: breathe 1.5s ease-in-out infinite; }
.conn-dot.off { background: var(--t-magenta); box-shadow: 0 0 8px var(--t-glow-magenta); }
@keyframes breathe { 0%,100% { opacity: 1; } 50% { opacity: 0.5; } }

/* ── Signal bars ── */
.signal-bars { display: inline-flex; align-items: flex-end; gap: 2px; height: 14px; vertical-align: middle; }
.signal-bars .bar { width: 3px; border-radius: 1px; }
.signal-bars .bar.active { background: var(--t-green); box-shadow: 0 0 4px var(--t-glow-green); }
.signal-bars .bar.inactive { background: var(--t-surface-3); }
.signal-bars .bar.weak { background: var(--t-magenta); box-shadow: 0 0 4px var(--t-glow-magenta); }
.signal-bars .bar.fair { background: var(--t-yellow); box-shadow: 0 0 4px var(--t-glow-yellow); }

/* ── Forms ── */
.form-group { margin-bottom: 12px; }
.form-group label { display: block; color: var(--t-ghost); font-size: 10px; text-transform: uppercase; letter-spacing: 0.08em; margin-bottom: 4px; }
.form-group input, .form-group select {
    width: 100%; background: var(--t-surface-3); border: 1px solid rgba(0,240,255,0.15);
    color: var(--t-text); padding: 8px 12px; border-radius: var(--t-radius);
    font-family: inherit; font-size: 12px; outline: none;
}
.form-group input:focus, .form-group select:focus { border-color: var(--t-cyan); box-shadow: 0 0 8px var(--t-glow-cyan); }
.form-group input::placeholder { color: var(--t-ghost); opacity: 0.5; }
.form-group select { cursor: pointer; }
.form-group select option { background: var(--t-surface-2); color: var(--t-text); }
.form-row { display: flex; gap: 12px; }
.form-row .form-group { flex: 1; }
@media (max-width: 500px) { .form-row { flex-direction: column; } }

/* ── Toggle switch ── */
.toggle-row { display: flex; align-items: center; justify-content: space-between; padding: 8px 0; }
.toggle-row .toggle-label { color: var(--t-ghost); font-size: 11px; text-transform: uppercase; letter-spacing: 0.08em; }
.toggle { position: relative; width: 40px; height: 20px; cursor: pointer; display: inline-block; }
.toggle input { opacity: 0; width: 0; height: 0; }
.toggle .slider { position: absolute; inset: 0; background: var(--t-surface-3); border: 1px solid rgba(0,240,255,0.15); border-radius: 10px; transition: all var(--t-transition); }
.toggle .slider::before { content: ''; position: absolute; width: 14px; height: 14px; border-radius: 50%; left: 2px; top: 2px; background: var(--t-ghost); transition: all var(--t-transition); }
.toggle input:checked + .slider { background: rgba(0,240,255,0.15); border-color: var(--t-cyan); }
.toggle input:checked + .slider::before { transform: translateX(20px); background: var(--t-cyan); box-shadow: 0 0 6px var(--t-glow-cyan); }

/* ── Saved networks list ── */
.net-list { list-style: none; }
.net-item {
    display: flex; align-items: center; gap: 12px; padding: 10px 12px;
    border-bottom: 1px solid rgba(255,255,255,0.02); cursor: grab;
    transition: background var(--t-transition);
}
.net-item:hover { background: var(--t-surface-3); }
.net-item.dragging { opacity: 0.5; background: rgba(0,240,255,0.05); }
.net-item.drag-over { border-top: 2px solid var(--t-cyan); }
.net-item .priority { color: var(--t-cyan); font-size: 14px; font-weight: bold; min-width: 20px; text-align: center; }
.net-item .net-ssid { flex: 1; color: var(--t-bright); font-size: 12px; }
.net-item .net-status { font-size: 10px; text-transform: uppercase; letter-spacing: 0.05em; }
.net-item .net-status.connected { color: var(--t-green); }
.net-item .net-status.saved { color: var(--t-ghost); }
.empty-list { text-align: center; padding: 20px; color: var(--t-ghost); font-size: 12px; }

/* ── Scan results grid ── */
.scan-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 8px; }
.scan-card {
    background: var(--t-surface-1); border: 1px solid rgba(0,240,255,0.06); border-radius: var(--t-radius);
    padding: 12px; cursor: pointer; transition: all var(--t-transition);
}
.scan-card:hover { border-color: var(--t-border-hover); box-shadow: 0 0 12px var(--t-glow-cyan); transform: translateY(-1px); }
.scan-card .sc-ssid { color: var(--t-bright); font-size: 12px; margin-bottom: 6px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.scan-card .sc-details { display: flex; justify-content: space-between; align-items: center; font-size: 10px; color: var(--t-ghost); text-transform: uppercase; letter-spacing: 0.05em; }
.scan-card .sc-rssi { font-size: 10px; color: var(--t-ghost); margin-top: 4px; }
.scan-card.open .sc-ssid { color: var(--t-yellow); }
.scanning-indicator { text-align: center; padding: 20px; color: var(--t-cyan); font-size: 12px; animation: breathe 1.5s ease-in-out infinite; }

/* ── Toast ── */
.toast { position: fixed; bottom: 20px; right: 20px; z-index: 300; padding: 10px 18px; border-radius: var(--t-radius); font-size: 12px; animation: toastIn 0.3s ease; pointer-events: none; border: 1px solid rgba(5,255,161,0.3); background: rgba(5,255,161,0.08); color: var(--t-green); }
.toast.error { border-color: rgba(255,42,109,0.3); background: rgba(255,42,109,0.08); color: var(--t-magenta); }
@keyframes toastIn { from { opacity: 0; transform: translateY(10px); } to { opacity: 1; transform: translateY(0); } }

/* ── Confirm overlay ── */
.confirm-overlay { display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.7); backdrop-filter: blur(6px); z-index: 200; align-items: center; justify-content: center; }
.confirm-overlay.active { display: flex; }
.confirm-box {
    background: var(--t-surface-2); border: 1px solid rgba(255,42,109,0.2); border-radius: 6px;
    padding: 24px; max-width: 380px; width: 90%; text-align: center;
    box-shadow: 0 0 30px var(--t-glow-magenta); animation: modalIn 0.25s ease;
}
@keyframes modalIn { from { opacity: 0; transform: scale(0.92); } to { opacity: 1; transform: scale(1); } }
.confirm-box h3 { color: var(--t-magenta); font-size: 13px; margin-bottom: 12px; text-transform: uppercase; letter-spacing: 0.1em; }
.confirm-box p { color: var(--t-ghost); font-size: 12px; margin-bottom: 16px; line-height: 1.6; }
.confirm-box .btn-row { justify-content: center; }
</style>
</head>
<body>

<div class="header">
    <span class="logo">TRITIUM-OS</span>
    <div class="nav">
        <a href="dashboard.html">Dashboard</a>
        <a href="files.html">Files</a>
        <a href="ota.html">OTA</a>
        <a href="wifi.html" class="active">WiFi</a>
        <a href="mesh.html">Mesh</a>
        <a href="terminal.html">Terminal</a>
    </div>
</div>

<div class="shell">

<!-- Connection Status -->
<div class="panel">
    <div class="panel-title">Connection Status</div>
    <div class="grid-2">
        <div>
            <div class="data-row"><span class="label">Status</span><span class="value" id="wifi-status"><span class="conn-dot off"></span> Disconnected</span></div>
            <div class="data-row"><span class="label">SSID</span><span class="value cyan" id="wifi-ssid">---</span></div>
            <div class="data-row"><span class="label">BSSID</span><span class="value" id="wifi-bssid">---</span></div>
            <div class="data-row"><span class="label">MAC</span><span class="value" id="wifi-mac">---</span></div>
        </div>
        <div>
            <div class="data-row"><span class="label">IP Address</span><span class="value green" id="wifi-ip">---</span></div>
            <div class="data-row"><span class="label">RSSI / Channel</span><span class="value" id="wifi-rssi">---</span></div>
            <div class="data-row"><span class="label">Gateway</span><span class="value" id="wifi-gw">---</span></div>
            <div class="data-row"><span class="label">DNS</span><span class="value" id="wifi-dns">---</span></div>
        </div>
    </div>
    <div class="btn-row">
        <button class="btn" id="btn-disconnect">Disconnect</button>
        <button class="btn" id="btn-reconnect">Reconnect</button>
    </div>
</div>

<div class="grid-2">
    <!-- Saved Networks -->
    <div class="panel">
        <div class="panel-title">Saved Networks</div>
        <ul class="net-list" id="saved-list">
            <li class="empty-list">No saved networks</li>
        </ul>
        <div style="font-size:10px;color:var(--t-ghost);margin-top:8px;text-transform:uppercase;letter-spacing:0.05em">
            Drag to reorder priority
        </div>
    </div>

    <!-- Add Network -->
    <div class="panel">
        <div class="panel-title">Add Network</div>
        <div class="form-group">
            <label>SSID</label>
            <input type="text" id="add-ssid" placeholder="Network name" autocomplete="off">
        </div>
        <div class="form-group">
            <label>Password</label>
            <input type="password" id="add-pass" placeholder="Password (leave empty for open)">
        </div>
        <div class="form-group">
            <label>Priority (1 = highest)</label>
            <input type="number" id="add-priority" value="5" min="1" max="10">
        </div>
        <div class="btn-row">
            <button class="btn green" id="btn-add-net">Save &amp; Connect</button>
        </div>
    </div>
</div>

<!-- Scan Networks -->
<div class="panel">
    <div class="panel-title">Scan Networks</div>
    <div class="btn-row" style="margin-top:0;margin-bottom:12px">
        <button class="btn" id="btn-scan">Scan Networks</button>
        <span id="scan-count" style="font-size:11px;color:var(--t-ghost);align-self:center"></span>
    </div>
    <div id="scan-results">
        <div class="empty-list">Press Scan to discover nearby networks</div>
    </div>
</div>

<!-- AP Mode -->
<div class="panel">
    <div class="panel-title">Access Point Mode</div>
    <div class="toggle-row" style="margin-bottom:12px">
        <span class="toggle-label" id="ap-status-label">AP Inactive</span>
        <label class="toggle">
            <input type="checkbox" id="ap-active-toggle">
            <span class="slider"></span>
        </label>
    </div>
    <div class="form-row">
        <div class="form-group">
            <label>AP SSID</label>
            <input type="text" id="ap-ssid" placeholder="Tritium-AP">
        </div>
        <div class="form-group">
            <label>AP Password</label>
            <input type="password" id="ap-pass" placeholder="Min 8 characters">
        </div>
        <div class="form-group">
            <label>Channel</label>
            <select id="ap-channel">
                <option value="1">1</option><option value="2">2</option><option value="3">3</option>
                <option value="4">4</option><option value="5">5</option><option value="6" selected>6</option>
                <option value="7">7</option><option value="8">8</option><option value="9">9</option>
                <option value="10">10</option><option value="11">11</option>
            </select>
        </div>
    </div>
    <div class="btn-row">
        <button class="btn" id="btn-ap-save">Save AP Settings</button>
    </div>
</div>

<!-- Auto-Failover -->
<div class="panel">
    <div class="panel-title">Auto-Failover</div>
    <div class="toggle-row">
        <span class="toggle-label">Enable auto-failover to AP mode when WiFi is unavailable</span>
        <label class="toggle">
            <input type="checkbox" id="failover-toggle">
            <span class="slider"></span>
        </label>
    </div>
    <div class="form-row" style="margin-top:10px">
        <div class="form-group">
            <label>Timeout (seconds)</label>
            <input type="number" id="failover-timeout" value="30" min="5" max="300">
        </div>
        <div class="form-group">
            <label>Max Retries</label>
            <input type="number" id="failover-retries" value="3" min="1" max="10">
        </div>
    </div>
    <div class="btn-row">
        <button class="btn" id="btn-failover-save">Save Failover Settings</button>
    </div>
</div>

</div>

<!-- Delete confirmation overlay -->
<div class="confirm-overlay" id="confirm-overlay">
    <div class="confirm-box">
        <h3>Remove Network</h3>
        <p>Remove <strong id="confirm-ssid"></strong> from saved networks?</p>
        <div class="btn-row">
            <button class="btn" id="confirm-cancel">Cancel</button>
            <button class="btn danger" id="confirm-ok">Remove</button>
        </div>
    </div>
</div>

<script>
(function(){
"use strict";

var $ = function(s){ return document.getElementById(s); };
var pendingRemove = '';
var savedNetworks = [];
var dragSrcIdx = null;

function apiFetch(p) { return fetch('/api/wifi/' + p); }
function apiPost(p, b) {
    return fetch('/api/wifi/' + p, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: b ? JSON.stringify(b) : undefined
    });
}

function escHtml(s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;'); }

function toast(msg, err) {
    var el = document.createElement('div');
    el.className = 'toast' + (err ? ' error' : '');
    el.textContent = msg;
    document.body.appendChild(el);
    setTimeout(function(){ el.remove(); }, 3000);
}

/* ---- Signal bars ---- */
function signalBars(rssi) {
    var level;
    if (rssi >= -50) level = 4;
    else if (rssi >= -60) level = 3;
    else if (rssi >= -70) level = 2;
    else if (rssi >= -80) level = 1;
    else level = 0;
    var html = '<span class="signal-bars">';
    for (var i = 0; i < 4; i++) {
        var h = 4 + i * 3;
        var cls = i < level ? (level <= 1 ? 'weak' : level <= 2 ? 'fair' : 'active') : 'inactive';
        html += '<span class="bar ' + cls + '" style="height:' + h + 'px"></span>';
    }
    return html + '</span>';
}

function authStr(a) {
    var m = {'OPEN':'Open','WEP':'WEP','WPA_PSK':'WPA','WPA2_PSK':'WPA2','WPA_WPA2_PSK':'WPA/2','WPA3_PSK':'WPA3','UNKNOWN':'?'};
    return m[a] || a || '?';
}

/* ---- Connection Status ---- */
function loadStatus() {
    apiFetch('status').then(function(r){ return r.json(); }).then(function(d) {
        var connected = d.connected || false;
        $('wifi-status').innerHTML = '<span class="conn-dot ' + (connected ? 'on' : 'off') + '"></span> ' + (connected ? 'Connected' : d.state || 'Disconnected');
        $('wifi-ssid').textContent = d.ssid || '---';
        $('wifi-bssid').textContent = d.bssid || '---';
        $('wifi-mac').textContent = d.mac || '---';
        $('wifi-ip').textContent = d.ip || '---';
        if (d.rssi != null) {
            $('wifi-rssi').innerHTML = signalBars(d.rssi) + ' ' + d.rssi + ' dBm / CH ' + (d.channel || '?');
        } else {
            $('wifi-rssi').textContent = '---';
        }
        $('wifi-gw').textContent = d.gateway || '---';
        $('wifi-dns').textContent = d.dns || '---';

        // AP state
        var apActive = d.ap_active || false;
        $('ap-active-toggle').checked = apActive;
        $('ap-status-label').textContent = apActive ? 'AP Active' : 'AP Inactive';
        $('ap-status-label').style.color = apActive ? 'var(--t-green)' : 'var(--t-ghost)';
        if (d.ap_ssid) $('ap-ssid').value = d.ap_ssid;

        // Failover
        if (d.failover !== undefined) $('failover-toggle').checked = d.failover;

        // Saved networks
        savedNetworks = d.saved || [];
        savedNetworks.sort(function(a, b) { return a.priority - b.priority; });
        renderSaved();
    }).catch(function(){});
}

/* ---- Saved Networks ---- */
function renderSaved() {
    var list = $('saved-list');
    if (savedNetworks.length === 0) {
        list.innerHTML = '<li class="empty-list">No saved networks</li>';
        return;
    }
    list.innerHTML = '';
    savedNetworks.forEach(function(net, idx) {
        var li = document.createElement('li');
        li.className = 'net-item';
        li.draggable = true;
        li.dataset.idx = idx;

        var statusCls = net.connected ? 'connected' : 'saved';
        var statusText = net.connected ? 'CONNECTED' : 'SAVED';
        li.innerHTML =
            '<span class="priority">' + (net.priority != null ? net.priority : idx + 1) + '</span>' +
            '<span class="net-ssid">' + escHtml(net.ssid) + '</span>' +
            '<span class="net-status ' + statusCls + '">' + statusText + '</span>' +
            '<button class="btn sm" data-action="connect" data-ssid="' + escHtml(net.ssid) + '">Connect</button>' +
            '<button class="btn sm danger" data-action="delete" data-ssid="' + escHtml(net.ssid) + '">Del</button>';

        // Drag events for reordering
        li.addEventListener('dragstart', function(e) {
            dragSrcIdx = idx;
            li.classList.add('dragging');
            e.dataTransfer.effectAllowed = 'move';
        });
        li.addEventListener('dragend', function() {
            li.classList.remove('dragging');
            document.querySelectorAll('.net-item').forEach(function(el) { el.classList.remove('drag-over'); });
        });
        li.addEventListener('dragover', function(e) {
            e.preventDefault();
            e.dataTransfer.dropEffect = 'move';
            li.classList.add('drag-over');
        });
        li.addEventListener('dragleave', function() { li.classList.remove('drag-over'); });
        li.addEventListener('drop', function(e) {
            e.preventDefault();
            li.classList.remove('drag-over');
            if (dragSrcIdx !== null && dragSrcIdx !== idx) {
                var item = savedNetworks.splice(dragSrcIdx, 1)[0];
                savedNetworks.splice(idx, 0, item);
                renderSaved();
                savePriority();
            }
            dragSrcIdx = null;
        });

        list.appendChild(li);
    });
}

// Delegate button clicks on saved list
$('saved-list').addEventListener('click', function(e) {
    var btn = e.target.closest('button[data-action]');
    if (!btn) return;
    var ssid = btn.dataset.ssid;
    if (btn.dataset.action === 'connect') {
        apiPost('connect', { ssid: ssid }).then(function(r){ return r.json(); }).then(function(d) {
            toast(d.msg || 'Connecting to ' + ssid);
            setTimeout(loadStatus, 3000);
        }).catch(function() { toast('Connect failed', true); });
    } else if (btn.dataset.action === 'delete') {
        pendingRemove = ssid;
        $('confirm-ssid').textContent = ssid;
        $('confirm-overlay').classList.add('active');
    }
});

function savePriority() {
    var order = savedNetworks.map(function(n, i) { return { ssid: n.ssid, priority: i }; });
    order.forEach(function(o) {
        apiPost('reorder', o);
    });
    toast('Priority updated');
}

/* ---- Delete confirmation ---- */
$('confirm-cancel').addEventListener('click', function() { $('confirm-overlay').classList.remove('active'); pendingRemove = ''; });
$('confirm-ok').addEventListener('click', function() {
    $('confirm-overlay').classList.remove('active');
    apiPost('remove', { ssid: pendingRemove }).then(function() {
        toast('Network removed');
        pendingRemove = '';
        setTimeout(loadStatus, 500);
    }).catch(function() { toast('Remove failed', true); });
});
$('confirm-overlay').addEventListener('click', function(e) {
    if (e.target === $('confirm-overlay')) { $('confirm-overlay').classList.remove('active'); pendingRemove = ''; }
});

/* ---- Add Network ---- */
$('btn-add-net').addEventListener('click', function() {
    var ssid = $('add-ssid').value.trim();
    if (!ssid) { $('add-ssid').focus(); return; }
    apiPost('connect', {
        ssid: ssid,
        password: $('add-pass').value,
        priority: parseInt($('add-priority').value) || 5
    }).then(function(r){ return r.json(); }).then(function(d) {
        if (d.connected) toast('Connected to ' + ssid);
        else toast(d.msg || 'Saved ' + ssid);
        $('add-ssid').value = '';
        $('add-pass').value = '';
        setTimeout(loadStatus, 2000);
    }).catch(function() { toast('Failed to connect', true); });
});

/* ---- Scan ---- */
$('btn-scan').addEventListener('click', function() {
    var btn = this;
    btn.disabled = true;
    $('scan-count').textContent = '';
    $('scan-results').innerHTML = '<div class="scanning-indicator">Scanning for networks...</div>';

    apiFetch('scan').then(function(r){ return r.json(); }).then(function(data) {
        var networks = data.networks || data || [];
        btn.disabled = false;
        if (networks.length === 0) {
            $('scan-results').innerHTML = '<div class="empty-list">No networks found</div>';
            $('scan-count').textContent = '';
            return;
        }
        networks.sort(function(a, b) { return (b.rssi || -100) - (a.rssi || -100); });
        $('scan-count').textContent = networks.length + ' networks found';

        var html = '<div class="scan-grid">';
        networks.forEach(function(n) {
            var isOpen = !n.auth || n.auth === 'OPEN' || n.auth === 0;
            var secText = isOpen ? 'OPEN' : authStr(n.auth);
            var lockIcon = isOpen ? '' : '&#x1F512; ';
            html += '<div class="scan-card' + (isOpen ? ' open' : '') + '" data-ssid="' + escHtml(n.ssid || '') + '">' +
                '<div class="sc-ssid">' + escHtml(n.ssid || '(hidden)') + '</div>' +
                '<div class="sc-details">' +
                    '<span>' + lockIcon + secText + '</span>' +
                    '<span>CH ' + (n.channel || '?') + '</span>' +
                '</div>' +
                '<div class="sc-rssi">' + signalBars(n.rssi || -100) + ' ' + (n.rssi || '?') + ' dBm' +
                (n.known ? ' <span class="green">*</span>' : '') + '</div>' +
                '</div>';
        });
        html += '</div>';
        $('scan-results').innerHTML = html;
    }).catch(function() {
        btn.disabled = false;
        $('scan-results').innerHTML = '<div class="empty-list mag">Scan failed</div>';
    });
});

// Click scan card to fill add-network form
$('scan-results').addEventListener('click', function(e) {
    var card = e.target.closest('.scan-card');
    if (!card) return;
    var ssid = card.dataset.ssid;
    if (ssid) {
        $('add-ssid').value = ssid;
        $('add-pass').value = '';
        $('add-pass').focus();
        var panel = $('add-ssid').closest('.panel');
        if (panel) panel.scrollIntoView({ behavior: 'smooth', block: 'center' });
    }
});

/* ---- Disconnect / Reconnect ---- */
$('btn-disconnect').addEventListener('click', function() {
    apiPost('disconnect').then(function() { toast('Disconnected'); setTimeout(loadStatus, 1000); })
      .catch(function() { toast('Failed', true); });
});
$('btn-reconnect').addEventListener('click', function() {
    apiPost('reconnect').then(function() { toast('Reconnecting...'); setTimeout(loadStatus, 3000); })
      .catch(function() { toast('Failed', true); });
});

/* ---- AP Mode ---- */
$('ap-active-toggle').addEventListener('change', function() {
    var on = this.checked;
    apiPost('ap', { active: on }).then(function() {
        toast(on ? 'AP starting...' : 'AP stopping...');
        setTimeout(loadStatus, 2000);
    }).catch(function() { toast('Failed', true); });
});

$('btn-ap-save').addEventListener('click', function() {
    apiPost('ap/config', {
        ssid: $('ap-ssid').value.trim(),
        password: $('ap-pass').value,
        channel: parseInt($('ap-channel').value)
    }).then(function() { toast('AP settings saved'); })
      .catch(function() { toast('Failed to save', true); });
});

/* ---- Auto-Failover ---- */
$('btn-failover-save').addEventListener('click', function() {
    apiPost('failover', {
        enabled: $('failover-toggle').checked,
        timeout: parseInt($('failover-timeout').value) || 30,
        retries: parseInt($('failover-retries').value) || 3
    }).then(function() { toast('Failover settings saved'); })
      .catch(function() { toast('Failed to save', true); });
});

/* ---- WebSocket ---- */
function connectWs() {
    var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    var ws = new WebSocket(proto + '//' + location.host + '/ws');
    ws.onmessage = function(e) {
        var msg;
        try { msg = JSON.parse(e.data); } catch(err) { return; }
        if (msg.type === 'wifi_status' || msg.type === 'wifi_connected' || msg.type === 'wifi_disconnected') {
            loadStatus();
        }
    };
    ws.onclose = function() { setTimeout(connectWs, 3000); };
    ws.onerror = function() { ws.close(); };
}

/* ---- Init ---- */
loadStatus();
connectWs();
setInterval(loadStatus, 5000);

})();
</script>
</body>
</html>
)rawliteral"
