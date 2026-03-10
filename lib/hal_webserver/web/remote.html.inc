R"rawliteral(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Remote Control — Tritium</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;600&family=JetBrains+Mono:wght@400;500&display=swap');
*{margin:0;padding:0;box-sizing:border-box}
:root{--cyan:#00f0ff;--magenta:#ff2a6d;--green:#05ffa1;--yellow:#fcee0a;
  --void:#0a0a0f;--s1:#0e0e14;--s2:#12121a;--s3:#1a1a2e;--text:#c8d0dc;--dim:#666}
body{background:var(--void);color:var(--text);font-family:'Inter',sans-serif;
  font-size:14px;display:flex;flex-direction:column;height:100vh;overflow:hidden;
  user-select:none;-webkit-user-select:none}
.header{display:flex;align-items:center;justify-content:space-between;
  padding:8px 16px;background:var(--s1);border-bottom:1px solid var(--cyan);
  box-shadow:0 1px 12px #00f0ff18}
.header h1{font-family:'JetBrains Mono',monospace;font-size:14px;color:var(--cyan);
  text-transform:uppercase;letter-spacing:2px;font-weight:500;
  text-shadow:0 0 8px #00f0ff44}
.header-btns{display:flex;gap:8px}
.header-btns button{background:transparent;border:1px solid var(--cyan);color:var(--cyan);
  padding:4px 10px;font-family:'JetBrains Mono',monospace;font-size:12px;cursor:pointer;
  transition:all .2s}
.header-btns button:hover{background:#00f0ff22;box-shadow:0 0 8px #00f0ff44}
.canvas-wrap{flex:1;display:flex;align-items:center;justify-content:center;
  background:var(--void);position:relative;overflow:hidden;min-height:0}
canvas{border:1px solid var(--cyan);box-shadow:0 0 20px #00f0ff18;
  image-rendering:pixelated;cursor:crosshair;max-width:100%;max-height:100%;
  object-fit:contain}
.touch-dot{position:absolute;width:20px;height:20px;border-radius:50%;
  background:var(--magenta);opacity:0;pointer-events:none;
  transform:translate(-50%,-50%);transition:opacity .15s;
  box-shadow:0 0 12px var(--magenta)}
.touch-dot.active{opacity:.8}
.footer{background:var(--s1);border-top:1px solid #00f0ff33;padding:8px 16px;
  display:flex;align-items:center;gap:16px;flex-wrap:wrap}
.stat{font-family:'JetBrains Mono',monospace;font-size:12px;color:var(--dim)}
.stat .val{color:var(--cyan)}
.fps-btns{display:flex;gap:4px;align-items:center}
.fps-btns span{font-family:'JetBrains Mono',monospace;font-size:11px;color:var(--dim);
  text-transform:uppercase;margin-right:4px}
.fps-btn{background:var(--s2);border:1px solid #00f0ff33;color:var(--text);
  padding:3px 8px;font-family:'JetBrains Mono',monospace;font-size:11px;
  cursor:pointer;transition:all .2s}
.fps-btn:hover{border-color:var(--cyan);background:#00f0ff11}
.fps-btn.active{background:#00f0ff22;border-color:var(--cyan);color:var(--cyan);
  box-shadow:0 0 6px #00f0ff33}
.status-dot{width:8px;height:8px;border-radius:50%;display:inline-block;
  margin-right:4px;vertical-align:middle}
.status-dot.ok{background:var(--green);box-shadow:0 0 6px var(--green)}
.status-dot.err{background:var(--magenta);box-shadow:0 0 6px var(--magenta)}
.status-dot.loading{background:var(--yellow);animation:blink 1s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}
.info-panel{position:absolute;top:48px;right:8px;background:var(--s2);
  border:1px solid #00f0ff33;padding:12px;font-family:'JetBrains Mono',monospace;
  font-size:11px;min-width:200px;z-index:10;display:none}
.info-panel.open{display:block}
.info-panel h3{color:var(--cyan);font-size:11px;text-transform:uppercase;
  letter-spacing:1px;margin-bottom:8px}
.info-row{display:flex;justify-content:space-between;padding:2px 0;
  border-bottom:1px solid #ffffff08}
.info-row .k{color:var(--dim)}
.info-row .v{color:var(--text)}
.scanline{position:fixed;top:0;left:0;right:0;bottom:0;pointer-events:none;
  background:repeating-linear-gradient(transparent,transparent 2px,rgba(0,0,0,.03) 2px,
  rgba(0,0,0,.03) 4px);z-index:9999}
</style>
</head><body>
<div class="scanline"></div>

<div class="header">
  <h1>[ TRITIUM REMOTE CONTROL ]</h1>
  <div class="header-btns">
    <button onclick="toggleInfo()" title="Device info">INFO</button>
    <button onclick="toggleFullscreen()" title="Fullscreen">FULL</button>
    <button onclick="window.location='/'">EXIT</button>
  </div>
</div>

<div class="info-panel" id="infoPanel">
  <h3>// DEVICE INFO</h3>
  <div id="infoContent">Loading...</div>
</div>

<div class="canvas-wrap" id="canvasWrap">
  <canvas id="screen"></canvas>
  <div class="touch-dot" id="touchDot"></div>
</div>

<div class="footer">
  <div class="stat"><span class="status-dot loading" id="statusDot"></span>
    <span id="connStatus">CONNECTING</span></div>
  <div class="stat">FPS: <span class="val" id="fpsDisplay">0</span></div>
  <div class="stat"><span class="val" id="resDisplay">---</span></div>
  <div class="stat">Latency: <span class="val" id="latDisplay">---</span></div>
  <div class="fps-btns">
    <span>Target:</span>
    <button class="fps-btn" data-fps="5" onclick="setFps(5)">5</button>
    <button class="fps-btn" data-fps="10" onclick="setFps(10)">10</button>
    <button class="fps-btn active" data-fps="15" onclick="setFps(15)">15</button>
    <button class="fps-btn" data-fps="30" onclick="setFps(30)">30</button>
  </div>
  <button class="fps-btn" onclick="toggleFullscreen()">FULLSCREEN</button>
</div>

<script>
// ── State ──────────────────────────────────────────────────────────────────
let deviceW = 0, deviceH = 0;
let targetFps = 15;
let refreshTimer = null;
let frameCount = 0;
let fpsCounter = 0;
let lastFpsUpdate = 0;
let isConnected = false;
let isSwiping = false;
let swipeStartX = 0, swipeStartY = 0;
let swipeStartTime = 0;
let pendingRefresh = false;

const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');
const touchDot = document.getElementById('touchDot');
const canvasWrap = document.getElementById('canvasWrap');

// ── Screenshot refresh ─────────────────────────────────────────────────────
async function refreshScreen() {
    if (pendingRefresh) return;
    pendingRefresh = true;
    const t0 = performance.now();
    try {
        const resp = await fetch('/api/remote/screenshot?t=' + Date.now());
        if (!resp.ok) throw new Error(resp.status);

        const w = parseInt(resp.headers.get('X-Width')) || deviceW;
        const h = parseInt(resp.headers.get('X-Height')) || deviceH;
        const buffer = await resp.arrayBuffer();

        if (w > 0 && h > 0 && buffer.byteLength >= w * h * 2) {
            if (canvas.width !== w || canvas.height !== h) {
                canvas.width = w;
                canvas.height = h;
                deviceW = w;
                deviceH = h;
                document.getElementById('resDisplay').textContent = w + 'x' + h;
            }

            const rgb565 = new Uint16Array(buffer);
            const imageData = ctx.createImageData(w, h);
            const d = imageData.data;

            for (let i = 0; i < rgb565.length; i++) {
                const px = rgb565[i];
                const i4 = i << 2;
                d[i4]     = (px >> 8 & 0xF8) | (px >> 13 & 0x07);
                d[i4 + 1] = (px >> 3 & 0xFC) | (px >> 9 & 0x03);
                d[i4 + 2] = (px << 3 & 0xF8) | (px >> 2 & 0x07);
                d[i4 + 3] = 255;
            }
            ctx.putImageData(imageData, 0, 0);
            frameCount++;
            fpsCounter++;

            setConnected(true);
        }

        const lat = Math.round(performance.now() - t0);
        document.getElementById('latDisplay').textContent = lat + 'ms';
    } catch (e) {
        setConnected(false);
    }
    pendingRefresh = false;
}

// ── FPS tracking ───────────────────────────────────────────────────────────
function updateFps() {
    const now = performance.now();
    if (now - lastFpsUpdate >= 1000) {
        document.getElementById('fpsDisplay').textContent = fpsCounter;
        fpsCounter = 0;
        lastFpsUpdate = now;
    }
}
setInterval(updateFps, 500);

// ── FPS control ────────────────────────────────────────────────────────────
function setFps(fps) {
    targetFps = fps;
    document.querySelectorAll('.fps-btn[data-fps]').forEach(b => {
        b.classList.toggle('active', parseInt(b.dataset.fps) === fps);
    });
    startRefreshLoop();
}

function startRefreshLoop() {
    if (refreshTimer) clearInterval(refreshTimer);
    const interval = Math.round(1000 / targetFps);
    refreshTimer = setInterval(refreshScreen, interval);
}

// ── Connection status ──────────────────────────────────────────────────────
function setConnected(ok) {
    if (ok === isConnected) return;
    isConnected = ok;
    const dot = document.getElementById('statusDot');
    const txt = document.getElementById('connStatus');
    dot.className = 'status-dot ' + (ok ? 'ok' : 'err');
    txt.textContent = ok ? 'CONNECTED' : 'DISCONNECTED';
}

// ── Canvas coordinate mapping ──────────────────────────────────────────────
function canvasToDevice(e) {
    const rect = canvas.getBoundingClientRect();
    const x = Math.round((e.clientX - rect.left) * (deviceW / rect.width));
    const y = Math.round((e.clientY - rect.top) * (deviceH / rect.height));
    return {
        x: Math.max(0, Math.min(deviceW - 1, x)),
        y: Math.max(0, Math.min(deviceH - 1, y))
    };
}

function showTouchDot(clientX, clientY) {
    touchDot.style.left = clientX + 'px';
    touchDot.style.top = clientY + 'px';
    touchDot.classList.add('active');
}

function hideTouchDot() {
    touchDot.classList.remove('active');
}

// ── Touch/mouse events ─────────────────────────────────────────────────────
function sendTouch(x, y, pressed) {
    fetch('/api/remote/touch', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({x, y, pressed})
    }).catch(() => {});
}

function sendTap(x, y) {
    fetch('/api/remote/tap', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({x, y})
    }).catch(() => {});
}

function sendSwipe(x1, y1, x2, y2, duration_ms) {
    fetch('/api/remote/swipe', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({x1, y1, x2, y2, duration_ms})
    }).catch(() => {});
}

// Mouse events
canvas.addEventListener('mousedown', e => {
    e.preventDefault();
    const p = canvasToDevice(e);
    isSwiping = true;
    swipeStartX = p.x;
    swipeStartY = p.y;
    swipeStartTime = Date.now();
    sendTouch(p.x, p.y, true);
    showTouchDot(e.clientX, e.clientY);
});

canvas.addEventListener('mousemove', e => {
    if (!isSwiping) return;
    e.preventDefault();
    const p = canvasToDevice(e);
    sendTouch(p.x, p.y, true);
    showTouchDot(e.clientX, e.clientY);
});

window.addEventListener('mouseup', e => {
    if (!isSwiping) return;
    isSwiping = false;
    const p = canvasToDevice(e);
    const dx = p.x - swipeStartX;
    const dy = p.y - swipeStartY;
    const dist = Math.sqrt(dx * dx + dy * dy);
    const duration = Date.now() - swipeStartTime;

    if (dist < 10 && duration < 300) {
        // Short click = tap
        sendTouch(p.x, p.y, false);
        sendTap(swipeStartX, swipeStartY);
    } else if (dist >= 10) {
        // Drag = swipe (touch events already sent during drag)
        sendTouch(p.x, p.y, false);
    } else {
        sendTouch(p.x, p.y, false);
    }
    hideTouchDot();
});

// Touch events (mobile)
canvas.addEventListener('touchstart', e => {
    e.preventDefault();
    const t = e.touches[0];
    const p = canvasToDevice(t);
    isSwiping = true;
    swipeStartX = p.x;
    swipeStartY = p.y;
    swipeStartTime = Date.now();
    sendTouch(p.x, p.y, true);
    showTouchDot(t.clientX, t.clientY);
}, {passive: false});

canvas.addEventListener('touchmove', e => {
    e.preventDefault();
    if (!isSwiping) return;
    const t = e.touches[0];
    const p = canvasToDevice(t);
    sendTouch(p.x, p.y, true);
    showTouchDot(t.clientX, t.clientY);
}, {passive: false});

canvas.addEventListener('touchend', e => {
    e.preventDefault();
    if (!isSwiping) return;
    isSwiping = false;
    // Use last known position for release
    sendTouch(swipeStartX, swipeStartY, false);
    hideTouchDot();
});

// Prevent context menu on canvas
canvas.addEventListener('contextmenu', e => e.preventDefault());

// ── Info panel ─────────────────────────────────────────────────────────────
function toggleInfo() {
    document.getElementById('infoPanel').classList.toggle('open');
    loadInfo();
}

async function loadInfo() {
    try {
        const resp = await fetch('/api/remote/info');
        const info = await resp.json();
        deviceW = info.width || deviceW;
        deviceH = info.height || deviceH;
        document.getElementById('resDisplay').textContent = deviceW + 'x' + deviceH;

        let html = '';
        const rows = [
            ['Resolution', info.width + 'x' + info.height],
            ['Touch', info.touch ? 'YES' : 'NO'],
            ['Shell', info.shell ? 'YES' : 'NO'],
            ['Target FPS', info.fps || '---'],
            ['Free Heap', info.free_heap ? Math.round(info.free_heap/1024)+'KB' : '---'],
            ['PSRAM Free', info.psram_free ? Math.round(info.psram_free/1024)+'KB' : '---'],
            ['Uptime', info.uptime_s ? Math.round(info.uptime_s)+'s' : '---'],
            ['IP', info.ip || '---'],
            ['RSSI', info.rssi ? info.rssi+'dBm' : '---']
        ];
        rows.forEach(r => {
            html += '<div class="info-row"><span class="k">'+r[0]+'</span><span class="v">'+r[1]+'</span></div>';
        });
        document.getElementById('infoContent').innerHTML = html;
    } catch(e) {
        document.getElementById('infoContent').textContent = 'Failed to load info';
    }
}

// ── Fullscreen ─────────────────────────────────────────────────────────────
function toggleFullscreen() {
    if (!document.fullscreenElement) {
        document.documentElement.requestFullscreen().catch(() => {});
    } else {
        document.exitFullscreen().catch(() => {});
    }
}

// ── Init ───────────────────────────────────────────────────────────────────
(async function() {
    // Fetch device info first to set canvas size
    try {
        const resp = await fetch('/api/remote/info');
        const info = await resp.json();
        deviceW = info.width || 480;
        deviceH = info.height || 320;
        canvas.width = deviceW;
        canvas.height = deviceH;
        document.getElementById('resDisplay').textContent = deviceW + 'x' + deviceH;

        // Draw placeholder
        ctx.fillStyle = '#0a0a0f';
        ctx.fillRect(0, 0, deviceW, deviceH);
        ctx.fillStyle = '#00f0ff';
        ctx.font = '16px monospace';
        ctx.textAlign = 'center';
        ctx.fillText('Connecting...', deviceW/2, deviceH/2);
    } catch(e) {
        canvas.width = 480;
        canvas.height = 320;
    }

    // Start refresh loop
    lastFpsUpdate = performance.now();
    startRefreshLoop();
    refreshScreen();
})();
</script>
</body></html>
)rawliteral"