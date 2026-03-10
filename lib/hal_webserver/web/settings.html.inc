R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TRITIUM-OS | Settings</title>
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
    --t-border: rgba(0, 240, 255, 0.08);
    --t-border-hover: rgba(0, 240, 255, 0.3);
    --t-radius: 4px;
    --t-transition: 0.25s ease;
    --t-font-body: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    --t-font-mono: 'JetBrains Mono', 'Fira Code', 'Cascadia Code', 'Consolas', monospace;
}
*, *::before, *::after { margin: 0; padding: 0; box-sizing: border-box; }
body {
    background: var(--t-void);
    color: var(--t-text);
    font-family: var(--t-font-body);
    font-size: 13px;
    line-height: 1.5;
    min-height: 100vh;
}
body::before {
    content: '';
    position: fixed;
    inset: 0;
    background-image:
        linear-gradient(rgba(0,240,255,0.025) 1px, transparent 1px),
        linear-gradient(90deg, rgba(0,240,255,0.025) 1px, transparent 1px);
    background-size: 24px 24px;
    pointer-events: none;
    z-index: -1;
}
a { color: var(--t-cyan); text-decoration: none; }
a:hover { text-decoration: underline; }

/* Nav */
.nav {
    display: flex;
    gap: 2px;
    padding: 8px 12px;
    background: var(--t-surface-1);
    border-bottom: 1px solid var(--t-border);
    flex-wrap: wrap;
}
.nav a {
    padding: 6px 14px;
    border-radius: var(--t-radius);
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    transition: var(--t-transition);
    color: var(--t-ghost);
}
.nav a:hover { color: var(--t-cyan); background: var(--t-surface-3); text-decoration: none; }
.nav a.active { color: var(--t-cyan); background: var(--t-surface-3); border: 1px solid rgba(0,240,255,0.15); }

/* Header */
.header {
    padding: 16px 20px;
    border-bottom: 1px solid var(--t-border);
    display: flex;
    align-items: center;
    justify-content: space-between;
}
.header h1 {
    font-family: var(--t-font-mono);
    font-size: 18px;
    color: var(--t-cyan);
    font-weight: 600;
    letter-spacing: 2px;
}
.header .actions { display: flex; gap: 8px; }

/* Buttons */
.btn {
    padding: 6px 16px;
    border: 1px solid rgba(0,240,255,0.3);
    background: var(--t-surface-2);
    color: var(--t-cyan);
    font-family: var(--t-font-mono);
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 1px;
    cursor: pointer;
    border-radius: var(--t-radius);
    transition: var(--t-transition);
}
.btn:hover { background: var(--t-surface-3); border-color: var(--t-cyan); }
.btn.save { border-color: rgba(5,255,161,0.4); color: var(--t-green); }
.btn.save:hover { border-color: var(--t-green); }
.btn.danger { border-color: rgba(255,42,109,0.3); color: var(--t-magenta); }
.btn.danger:hover { border-color: var(--t-magenta); }

/* Content */
.content { padding: 16px 20px; max-width: 800px; }

/* Domain panels */
.domain {
    margin-bottom: 16px;
    border: 1px solid var(--t-border);
    border-radius: var(--t-radius);
    background: var(--t-surface-1);
}
.domain-header {
    padding: 10px 16px;
    background: var(--t-surface-2);
    border-bottom: 1px solid var(--t-border);
    font-family: var(--t-font-mono);
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 2px;
    color: var(--t-cyan);
    cursor: pointer;
    display: flex;
    justify-content: space-between;
    align-items: center;
}
.domain-header:hover { background: var(--t-surface-3); }
.domain-header .chevron { transition: transform 0.2s; }
.domain-header.collapsed .chevron { transform: rotate(-90deg); }
.domain-body { padding: 8px 16px; }
.domain-body.hidden { display: none; }

/* Setting rows */
.setting-row {
    display: flex;
    align-items: center;
    padding: 6px 0;
    border-bottom: 1px solid var(--t-border);
    gap: 12px;
}
.setting-row:last-child { border-bottom: none; }
.setting-key {
    font-family: var(--t-font-mono);
    font-size: 12px;
    color: var(--t-ghost);
    min-width: 150px;
    flex-shrink: 0;
}
.setting-val { flex: 1; }
.setting-val input[type="text"],
.setting-val input[type="number"] {
    width: 100%;
    padding: 4px 8px;
    background: var(--t-surface-2);
    border: 1px solid var(--t-border);
    border-radius: var(--t-radius);
    color: var(--t-bright);
    font-family: var(--t-font-mono);
    font-size: 12px;
    outline: none;
    transition: var(--t-transition);
}
.setting-val input:focus { border-color: var(--t-cyan); }

/* Toggle switch */
.toggle {
    position: relative;
    width: 40px;
    height: 20px;
    cursor: pointer;
}
.toggle input { display: none; }
.toggle .slider {
    position: absolute;
    inset: 0;
    background: var(--t-surface-3);
    border: 1px solid var(--t-ghost);
    border-radius: 10px;
    transition: var(--t-transition);
}
.toggle .slider::before {
    content: '';
    position: absolute;
    width: 14px;
    height: 14px;
    left: 2px;
    top: 2px;
    background: var(--t-ghost);
    border-radius: 50%;
    transition: var(--t-transition);
}
.toggle input:checked + .slider { background: rgba(0,240,255,0.15); border-color: var(--t-cyan); }
.toggle input:checked + .slider::before { transform: translateX(20px); background: var(--t-cyan); }

/* Range slider */
.range-wrap { display: flex; align-items: center; gap: 8px; }
.range-wrap input[type="range"] {
    flex: 1;
    -webkit-appearance: none;
    height: 4px;
    background: var(--t-surface-3);
    border-radius: 2px;
    outline: none;
}
.range-wrap input[type="range"]::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 14px;
    height: 14px;
    background: var(--t-cyan);
    border-radius: 50%;
    cursor: pointer;
}
.range-val {
    font-family: var(--t-font-mono);
    font-size: 11px;
    color: var(--t-cyan);
    min-width: 32px;
    text-align: right;
}

/* Toast */
.toast {
    position: fixed;
    top: 12px;
    right: 12px;
    padding: 8px 16px;
    background: var(--t-surface-2);
    border: 1px solid var(--t-cyan);
    border-radius: var(--t-radius);
    font-family: var(--t-font-mono);
    font-size: 12px;
    color: var(--t-cyan);
    z-index: 1000;
    opacity: 0;
    transform: translateY(-8px);
    transition: all 0.3s ease;
}
.toast.show { opacity: 1; transform: translateY(0); }
.toast.error { border-color: var(--t-magenta); color: var(--t-magenta); }
.toast.success { border-color: var(--t-green); color: var(--t-green); }

/* Loading */
.loading {
    text-align: center;
    padding: 40px;
    font-family: var(--t-font-mono);
    color: var(--t-ghost);
}
.spin { display: inline-block; animation: spin 1s linear infinite; }
@keyframes spin { to { transform: rotate(360deg); } }
</style>
</head>
<body>

<div class="nav">
    <a href="/">Dashboard</a>
    <a href="/files">Files</a>
    <a href="/ota">OTA</a>
    <a href="/wifi">WiFi</a>
    <a href="/mesh">Mesh</a>
    <a href="/terminal">Terminal</a>
    <a href="/settings" class="active">Settings</a>
</div>

<div class="header">
    <h1>// SETTINGS</h1>
    <div class="actions">
        <button class="btn save" onclick="saveAll()">Save All</button>
        <button class="btn danger" onclick="resetAll()">Factory Reset</button>
    </div>
</div>

<div class="content" id="content">
    <div class="loading"><span class="spin">&#x25E0;</span> Loading settings...</div>
</div>

<div class="toast" id="toast"></div>

<script>
var S = {};        // current settings state
var ORIG = {};     // original loaded state (for dirty detection)
var SPECIAL = {    // keys that get special UI treatment
    brightness: { type: 'range', min: 0, max: 255 },
    log_level:  { type: 'range', min: 0, max: 5 },
    sf_star_size: { type: 'range', min: 1, max: 5 }
};
var HIDDEN = ['lock_pin']; // don't show these in web UI

function $(id) { return document.getElementById(id); }

function toast(msg, cls) {
    var t = $('toast');
    t.textContent = msg;
    t.className = 'toast show ' + (cls || '');
    clearTimeout(t._tid);
    t._tid = setTimeout(function() { t.className = 'toast'; }, 3000);
}

function inferType(val) {
    if (typeof val === 'boolean') return 'bool';
    if (typeof val === 'number') return Number.isInteger(val) ? 'int' : 'float';
    return 'string';
}

function buildUI(settings) {
    var html = '';
    var domains = Object.keys(settings).sort();
    for (var d = 0; d < domains.length; d++) {
        var domain = domains[d];
        var keys = Object.keys(settings[domain]).sort();
        html += '<div class="domain">';
        html += '<div class="domain-header" onclick="toggleDomain(this)">';
        html += '<span>' + domain.toUpperCase() + '</span>';
        html += '<span class="chevron">&#x25BC;</span>';
        html += '</div>';
        html += '<div class="domain-body">';
        for (var k = 0; k < keys.length; k++) {
            var key = keys[k];
            if (HIDDEN.indexOf(key) >= 0) continue;
            var val = settings[domain][key];
            var type = inferType(val);
            var id = domain + '.' + key;
            html += '<div class="setting-row">';
            html += '<span class="setting-key">' + key + '</span>';
            html += '<div class="setting-val">';
            if (type === 'bool') {
                html += '<label class="toggle"><input type="checkbox" id="' + id + '"';
                if (val) html += ' checked';
                html += ' onchange="changed(\'' + domain + '\',\'' + key + '\',this.checked)">';
                html += '<span class="slider"></span></label>';
            } else if (SPECIAL[key]) {
                var sp = SPECIAL[key];
                if (sp.type === 'range') {
                    html += '<div class="range-wrap">';
                    html += '<input type="range" id="' + id + '" min="' + sp.min + '" max="' + sp.max + '" value="' + val + '"';
                    html += ' oninput="$(this.id+\'_v\').textContent=this.value;changed(\'' + domain + '\',\'' + key + '\',parseInt(this.value))">';
                    html += '<span class="range-val" id="' + id + '_v">' + val + '</span>';
                    html += '</div>';
                }
            } else if (type === 'int' || type === 'float') {
                html += '<input type="number" id="' + id + '" value="' + val + '"';
                if (type === 'float') html += ' step="0.1"';
                html += ' onchange="changed(\'' + domain + '\',\'' + key + '\',' + (type === 'float' ? 'parseFloat' : 'parseInt') + '(this.value))">';
            } else {
                html += '<input type="text" id="' + id + '" value="' + escHtml(String(val)) + '"';
                html += ' onchange="changed(\'' + domain + '\',\'' + key + '\',this.value)">';
            }
            html += '</div></div>';
        }
        html += '</div></div>';
    }
    $('content').innerHTML = html;
}

function escHtml(s) {
    return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function toggleDomain(el) {
    var body = el.nextElementSibling;
    body.classList.toggle('hidden');
    el.classList.toggle('collapsed');
}

function changed(domain, key, val) {
    if (!S[domain]) S[domain] = {};
    S[domain][key] = val;
}

function saveAll() {
    // Build delta — only send changed values
    var delta = {};
    var count = 0;
    for (var domain in S) {
        for (var key in S[domain]) {
            if (!ORIG[domain] || ORIG[domain][key] !== S[domain][key]) {
                if (!delta[domain]) delta[domain] = {};
                delta[domain][key] = S[domain][key];
                count++;
            }
        }
    }
    if (count === 0) {
        toast('No changes to save');
        return;
    }
    fetch('/api/settings', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(delta)
    })
    .then(function(r) { return r.json(); })
    .then(function(j) {
        if (j.ok) {
            toast('Saved ' + count + ' setting' + (count > 1 ? 's' : ''), 'success');
            // Update original state
            for (var d in delta) {
                if (!ORIG[d]) ORIG[d] = {};
                for (var k in delta[d]) ORIG[d][k] = delta[d][k];
            }
        } else {
            toast('Save failed: ' + (j.error || 'unknown'), 'error');
        }
    })
    .catch(function(e) { toast('Network error: ' + e, 'error'); });
}

function resetAll() {
    if (!confirm('Reset ALL settings to factory defaults? This cannot be undone.')) return;
    fetch('/api/settings/reset', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' })
    .then(function(r) { return r.json(); })
    .then(function(j) {
        if (j.ok) {
            toast('Factory reset complete — reloading...', 'success');
            setTimeout(loadSettings, 1000);
        } else {
            toast('Reset failed', 'error');
        }
    })
    .catch(function(e) { toast('Network error: ' + e, 'error'); });
}

function loadSettings() {
    $('content').innerHTML = '<div class="loading"><span class="spin">&#x25E0;</span> Loading settings...</div>';
    fetch('/api/settings')
    .then(function(r) { return r.json(); })
    .then(function(j) {
        S = j;
        ORIG = JSON.parse(JSON.stringify(j));
        buildUI(j);
    })
    .catch(function(e) {
        $('content').innerHTML = '<div class="loading" style="color:var(--t-magenta)">Failed to load settings: ' + e + '</div>';
    });
}

loadSettings();
</script>
</body>
</html>
)rawliteral"
