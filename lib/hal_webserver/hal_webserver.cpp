#include "hal_webserver.h"
#include "debug_log.h"

#ifdef SIMULATOR

// ── Simulator stubs ─────────────────────────────────────────────────────────
bool WebServerHAL::init(uint16_t) { return false; }
void WebServerHAL::stop() {}
bool WebServerHAL::isRunning() const { return false; }
void WebServerHAL::process() {}
void WebServerHAL::on(const char*, const char*, WebRequestHandler) {}
void WebServerHAL::addDashboard() {}
void WebServerHAL::addOtaPage() {}
void WebServerHAL::addConfigEditor() {}
void WebServerHAL::addFileManager() {}
void WebServerHAL::addApiEndpoints() {}
void WebServerHAL::addWiFiSetup() {}
void WebServerHAL::addBleViewer() {}
void WebServerHAL::addCommissionPage() {}
void WebServerHAL::addSystemPage() {}
void WebServerHAL::addLogsPage() {}
void WebServerHAL::addTerminalPage() {}
void WebServerHAL::addMapPage() {}
void WebServerHAL::addErrorPages() {}
void WebServerHAL::addAllPages() {}
void WebServerHAL::captureLog(const char*) {}
int WebServerHAL::getLogJson(char*, size_t) { return 0; }
void WebServerHAL::setBleProvider(BleJsonProvider) {}
void WebServerHAL::setDiagProvider(DiagJsonProvider) {}
void WebServerHAL::setDiagHealthProvider(DiagJsonProvider) {}
void WebServerHAL::setDiagEventsProvider(DiagJsonProvider) {}
void WebServerHAL::setDiagAnomaliesProvider(DiagJsonProvider) {}
void WebServerHAL::setMeshProvider(MeshJsonProvider) {}
void WebServerHAL::setGisTileProvider(GisTileProvider) {}
void WebServerHAL::setGisLayerProvider(GisLayerProvider) {}
void WebServerHAL::startCaptivePortal() {}
void WebServerHAL::stopCaptivePortal() {}
void WebServerHAL::sendResponse(int, const char*, const char*) {}
void WebServerHAL::sendJson(int, const char*) {}
bool WebServerHAL::startMDNS(const char*) { return false; }
uint16_t WebServerHAL::getPort() const { return 0; }
uint32_t WebServerHAL::getRequestCount() const { return 0; }
const char* WebServerHAL::getIP() const { return "0.0.0.0"; }

WebServerHAL::TestResult WebServerHAL::runTest() {
    return {false, false, false, false, 0, "0.0.0.0", 0};
}

#else // ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Update.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include "wifi_classifier.h"

#if __has_include("serial_capture.h")
#include "serial_capture.h"
#endif

#if __has_include("wifi_manager.h")
#include "wifi_manager.h"
#define HAS_WIFI_MANAGER 1
#else
#define HAS_WIFI_MANAGER 0
#endif
#include "service_registry.h"

#if __has_include("os_shell.h")
#include "os_shell.h"
#include "shell_screensaver.h"
#define HAS_SHELL 1
#else
#define HAS_SHELL 0
#endif

// Shared API response buffer — allocated in PSRAM on first use.
// Safe because WebServer is single-threaded (one request at a time).
static const size_t API_BUF_SIZE = 8192;
static char* _apiBuf = nullptr;
// Small static fallback so api_buf() never returns null
static char _apiBufFallback[256];
static size_t _apiBufActualSize = sizeof(_apiBufFallback);
static char* api_buf() {
    if (!_apiBuf) {
        _apiBuf = (char*)heap_caps_malloc(API_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!_apiBuf) _apiBuf = (char*)malloc(API_BUF_SIZE);
        if (_apiBuf) {
            _apiBufActualSize = API_BUF_SIZE;
        } else {
            DBG_ERROR("web", "Failed to allocate API buffer, using fallback");
            _apiBuf = _apiBufFallback;
            _apiBufActualSize = sizeof(_apiBufFallback);
        }
    }
    return _apiBuf;
}

// Validate SD card path — reject path traversal attempts
static bool sd_path_is_safe(const char* path) {
    if (!path || path[0] == '\0') return false;
    if (strstr(path, "..")) return false;
    return true;
}

// Safe JSON integer extraction: finds "key":N and returns value, or dflt on failure.
// Handles colon, optional whitespace, and validates the pointer doesn't run off.
static int json_int(const char* json, const char* key, int dflt = 0) {
    if (!json || !key) return dflt;
    const char* p = strstr(json, key);
    if (!p) return dflt;
    p += strlen(key);
    // Skip past closing quote, colon, whitespace
    while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
    if (!*p) return dflt;
    return atoi(p);
}

// Safe JSON string extraction into buffer. Returns true if found.
static bool json_str(const char* json, const char* key, char* out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return false;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    size_t i = 0;
    while (p[i] && p[i] != '"' && i < out_size - 1) { out[i] = p[i]; i++; }
    out[i] = '\0';
    return i > 0;
}

// Persistent diagnostic log — optional
#if __has_include("hal_diaglog.h")
#include "hal_diaglog.h"
#define WEB_HAS_DIAGLOG 1
#else
#define WEB_HAS_DIAGLOG 0
#endif

// Diagnostics HAL — for cached health endpoints
#if defined(ENABLE_DIAG) && __has_include("hal_diag.h")
#include "hal_diag.h"
#endif

// Touch injection for remote control
#if __has_include("touch_input.h")
#include "touch_input.h"
#define WEB_HAS_TOUCH 1
#else
#define WEB_HAS_TOUCH 0
#endif

// Display framebuffer access for remote screenshot
#if __has_include("lvgl_driver.h")
#include "lvgl_driver.h"
#include "display.h"
#define WEB_HAS_DISPLAY 1
#else
#define WEB_HAS_DISPLAY 0
#endif

// Settings API
#if __has_include("os_settings.h")
#include "os_settings.h"
#endif

static WebServer* _server = nullptr;
static WebServerHAL* _instance = nullptr;
static DNSServer* _dnsServer = nullptr;

// ── Dark neon hacker theme ──────────────────────────────────────────────────
// Shared CSS used by all pages — black bg, neon cyan accents, monospace font

static const char THEME_CSS[] PROGMEM = R"rawliteral(
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0a0a0a;color:#c0c0c0;font-family:'Courier New',monospace;
  font-size:14px;padding:20px;max-width:900px;margin:0 auto}
h1{color:#00ffd0;font-size:22px;border-bottom:1px solid #00ffd044;
  padding-bottom:8px;margin-bottom:16px}
h2{color:#00ffd0;font-size:16px;margin:16px 0 8px}
a{color:#00ffd0;text-decoration:none}
a:hover{text-decoration:underline;color:#66ffe8}
.card{background:#111;border:1px solid #00ffd022;border-radius:6px;
  padding:16px;margin-bottom:12px}
table{width:100%;border-collapse:collapse;margin:8px 0}
th,td{text-align:left;padding:6px 10px;border-bottom:1px solid #1a1a1a}
th{color:#00ffd0;font-weight:normal;font-size:12px;text-transform:uppercase}
input[type=file]{color:#c0c0c0;margin:8px 0}
textarea{background:#0a0a0a;color:#00ffd0;border:1px solid #00ffd044;
  border-radius:4px;padding:10px;width:100%;font-family:'Courier New',monospace;
  font-size:13px;resize:vertical}
button,input[type=submit]{background:#00ffd0;color:#0a0a0a;border:none;
  padding:8px 20px;border-radius:4px;cursor:pointer;font-family:inherit;
  font-weight:bold;font-size:13px;margin:4px 2px}
button:hover,input[type=submit]:hover{background:#66ffe8}
button.danger{background:#ff3366;color:#fff}
button.danger:hover{background:#ff6690}
.bar-bg{background:#1a1a1a;border-radius:3px;height:14px;width:120px;
  display:inline-block;vertical-align:middle}
.bar-fill{background:#00ffd0;height:100%;border-radius:3px;transition:width .3s}
.label{color:#666;font-size:12px}
.nav{margin-bottom:16px;display:flex;gap:12px;flex-wrap:wrap}
.nav a{background:#111;border:1px solid #00ffd022;padding:6px 14px;
  border-radius:4px;font-size:13px}
.nav a:hover{background:#1a1a1a;border-color:#00ffd0}
.msg{padding:10px;border-radius:4px;margin:8px 0}
.msg.ok{background:#00ffd011;border:1px solid #00ffd044;color:#00ffd0}
.msg.err{background:#ff336611;border:1px solid #ff336644;color:#ff3366}
#progress{display:none;margin:10px 0}
#progress .bar-bg{width:100%;height:20px}
</style>
)rawliteral";

static const char NAV_HTML[] PROGMEM = R"rawliteral(
<div class="nav">
  <a href="/">Dashboard</a>
  <a href="/system">System</a>
  <a href="/wifi">WiFi</a>
  <a href="/ble">BLE Scan</a>
  <a href="/ota">OTA Update</a>
  <a href="/logs">Logs</a>
  <a href="/terminal">Terminal</a>
  <a href="/config">Config</a>
  <a href="/files">Files</a>
  <a href="/storage">SD Card</a>
  <a href="/remote">Remote</a>
  <a href="/commission">Commission</a>
  <a href="/map">Map</a>
  <a href="/api/status">API</a>
</div>
)rawliteral";

// ── Dashboard page (/) ──────────────────────────────────────────────────────

static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tritium Node</title>
%THEME%
<style>
.glow{text-shadow:0 0 10px #00ffd044,0 0 20px #00ffd022}
.metric{display:inline-block;text-align:center;padding:12px;min-width:120px}
.metric .val{font-size:24px;color:#00ffd0;font-weight:bold}
.metric .lbl{font-size:11px;color:#666;text-transform:uppercase;margin-top:4px}
.grid{display:flex;flex-wrap:wrap;gap:8px;margin:12px 0}
.pulse{animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
.scanline{position:fixed;top:0;left:0;right:0;bottom:0;pointer-events:none;
  background:repeating-linear-gradient(transparent,transparent 2px,rgba(0,255,208,0.015) 2px,rgba(0,255,208,0.015) 4px);z-index:999}
.cap-badge{display:inline-block;padding:3px 10px;margin:3px;border-radius:12px;font-size:11px;
  border:1px solid #00ffd044;color:#00ffd0;background:#00ffd00a}
.cap-badge.active{border-color:#00ffd0;background:#00ffd01a}
.cap-badge.inactive{border-color:#333;color:#555;background:transparent}
.status-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
.status-dot.on{background:#00ffd0;box-shadow:0 0 6px #00ffd066}
.status-dot.off{background:#333}
</style>
</head><body>
<div class="scanline"></div>
%NAV%
<h1 class="glow">// TRITIUM NODE</h1>
<div class="card">
<div class="grid">
  <div class="metric"><div class="val" id="v_uptime">%UPTIME%</div><div class="lbl">Uptime</div></div>
  <div class="metric"><div class="val" id="v_heap">%HEAP%</div><div class="lbl">Free Heap</div></div>
  <div class="metric"><div class="val" id="v_frag">--</div><div class="lbl">Heap Frag</div></div>
  <div class="metric"><div class="val" id="v_rssi">%RSSI%</div><div class="lbl">WiFi dBm</div></div>
  <div class="metric"><div class="val" id="v_tasks">--</div><div class="lbl">Tasks</div></div>
  <div class="metric"><div class="val" id="v_reqs">%REQCOUNT%</div><div class="lbl">Requests</div></div>
</div>
</div>
<div class="card">
<h2>Node Identity</h2>
<table>
<tr><td class="label">Firmware</td><td id="v_fw">--</td></tr>
<tr><td class="label">Board</td><td>%BOARD%</td></tr>
<tr><td class="label">MAC</td><td style="font-family:monospace">%MAC%</td></tr>
<tr><td class="label">IP</td><td>%IP%</td></tr>
<tr><td class="label">SSID</td><td id="v_ssid">--</td></tr>
<tr><td class="label">CPU</td><td><span id="v_cpu">--</span> MHz</td></tr>
<tr><td class="label">PSRAM</td><td><span id="v_psram">%PSRAM_FREE%</span> / %PSRAM_TOTAL% bytes</td></tr>
<tr><td class="label">Signal</td><td>
  <div class="bar-bg" style="width:200px"><div class="bar-fill" id="v_rssi_bar" style="width:%RSSI_PCT%%"></div></div>
</td></tr>
</table>
</div>
<div class="card">
<h2>Capabilities</h2>
<div id="caps">%CAPABILITIES%</div>
</div>
<div class="card" id="ble_card" style="display:%BLE_DISPLAY%">
<h2>BLE Scanner</h2>
<table>
<tr><td class="label">Status</td><td id="ble_status">%BLE_STATUS%</td></tr>
<tr><td class="label">Devices</td><td id="ble_count">%BLE_COUNT%</td></tr>
</table>
</div>
<script>
function fmt(n){return n>1048576?(n/1048576).toFixed(1)+'M':n>1024?(n/1024).toFixed(0)+'K':n}
function uptimeFmt(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d+'d '+h+'h '+m+'m'}
setInterval(function(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('v_uptime').textContent=uptimeFmt(d.uptime_s);
    document.getElementById('v_heap').textContent=fmt(d.free_heap);
    document.getElementById('v_rssi').textContent=d.rssi;
    document.getElementById('v_reqs').textContent=d.requests;
    document.getElementById('v_psram').textContent=fmt(d.psram_free);
    if(d.fw_version)document.getElementById('v_fw').textContent=d.fw_version;
    if(d.ssid)document.getElementById('v_ssid').textContent=d.ssid;
    if(d.cpu_freq)document.getElementById('v_cpu').textContent=d.cpu_freq;
    if(d.tasks)document.getElementById('v_tasks').textContent=d.tasks;
    if(d.largest_block&&d.free_heap){
      var fp=Math.round(100-d.largest_block*100/d.free_heap);
      var el=document.getElementById('v_frag');
      el.textContent=fp+'%';
      el.style.color=fp>50?'#ff2a6d':fp>25?'#fcee0a':'#05ffa1';
    }
    var pct=Math.min(100,Math.max(0,2*(d.rssi+100)));
    document.getElementById('v_rssi_bar').style.width=pct+'%';
  });
  fetch('/api/ble').then(r=>r.json()).then(d=>{
    if(d.active!==undefined){
      document.getElementById('ble_status').textContent=d.active?'Active':'Inactive';
      document.getElementById('ble_count').textContent=d.total+' total, '+d.known+' known';
    }
  }).catch(()=>{});
},3000);
</script>
</body></html>
)rawliteral";

// ── OTA Update page (/update and /ota) ──────────────────────────────────────
// Uses ota_manager for proper ESP-IDF OTA with rollback, history, and validation.

#if __has_include("ota_manager.h")
#include "ota_manager.h"
#define OTA_MANAGER_AVAILABLE 1
#else
#define OTA_MANAGER_AVAILABLE 0
#endif

static const char OTA_HTML_MODERN[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tritium-OS // Firmware Update</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--cyan:#00f0ff;--mag:#ff2a6d;--green:#05ffa1;--yellow:#fcee0a;
--void:#0a0a0f;--s1:#0e0e14;--s2:#12121a;--s3:#1a1a2e;
--ghost:#8888aa;--text:#c8d0dc;--bright:#e0e0ff}
body{background:var(--void);color:var(--text);font-family:'Courier New',monospace;
font-size:13px;padding:16px;max-width:720px;margin:0 auto}
h1{color:var(--cyan);font-size:16px;letter-spacing:0.15em;text-transform:uppercase;
padding:12px 0;border-bottom:1px solid rgba(0,240,255,0.15);margin-bottom:16px}
.panel{background:var(--s2);border:1px solid rgba(0,240,255,0.08);border-radius:4px;
padding:14px;margin-bottom:12px}
.panel-title{color:var(--bright);font-size:11px;letter-spacing:0.12em;
text-transform:uppercase;margin-bottom:10px}
.info-grid{display:grid;grid-template-columns:auto 1fr;gap:4px 16px;font-size:12px}
.info-grid .k{color:var(--ghost)}.info-grid .v{color:var(--cyan)}
.btn{padding:7px 16px;border:1px solid rgba(0,240,255,0.3);background:transparent;
color:var(--cyan);font-family:inherit;font-size:11px;cursor:pointer;border-radius:3px}
.btn:disabled{opacity:0.4;cursor:not-allowed}
.btn.danger{border-color:rgba(255,42,109,0.3);color:var(--mag)}
.btn-row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
.progress-wrap{margin:12px 0;display:none}.progress-wrap.show{display:block}
.progress-bar{height:20px;background:var(--s3);border-radius:3px;overflow:hidden}
.progress-fill{height:100%;width:0%;background:var(--cyan);transition:width 0.3s}
.progress-text{text-align:center;font-size:12px;margin-top:4px;color:var(--cyan)}
.status-msg{padding:8px 12px;border-radius:3px;font-size:12px;margin:8px 0;display:none}
.status-msg.show{display:block}
.status-msg.ok{background:rgba(5,255,161,0.08);color:var(--green)}
.status-msg.err{background:rgba(255,42,109,0.08);color:var(--mag)}
table{width:100%;border-collapse:collapse;font-size:12px;margin-top:6px}
th{text-align:left;color:var(--ghost);font-size:10px;padding:4px 8px;border-bottom:1px solid rgba(0,240,255,0.08)}
td{padding:5px 8px;border-bottom:1px solid rgba(255,255,255,0.03)}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:6px}
.dot.ok{background:var(--green)}.dot.fail{background:var(--mag)}
.state-label{display:inline-block;padding:2px 8px;border-radius:2px;font-size:10px;text-transform:uppercase}
.state-idle{background:rgba(136,136,170,0.15);color:var(--ghost)}
.state-active{background:rgba(0,240,255,0.15);color:var(--cyan)}
.state-ok{background:rgba(5,255,161,0.15);color:var(--green)}
.state-err{background:rgba(255,42,109,0.15);color:var(--mag)}
.danger-zone{border-color:rgba(255,42,109,0.15)}
.drop-zone{border:2px dashed rgba(0,240,255,0.2);border-radius:6px;padding:32px 16px;
text-align:center;position:relative;background:var(--s1)}
.drop-zone input{position:absolute;inset:0;opacity:0;cursor:pointer}
</style></head><body>
<h1>// Tritium-OS Firmware Update</h1>
<div class="panel"><div class="panel-title">System Info</div>
<div class="info-grid">
<span class="k">Current</span><span class="v" id="cur-ver">---</span>
<span class="k">Partition</span><span class="v" id="cur-part">---</span>
<span class="k">Next</span><span class="v" id="next-part">---</span>
<span class="k">Max Size</span><span class="v" id="part-size">---</span>
<span class="k">State</span><span class="v" id="ota-state"><span class="state-label state-idle">IDLE</span></span>
<span class="k">Uptime</span><span class="v" id="uptime">---</span>
</div></div>

<div class="panel"><div class="panel-title">Firmware Upload</div>
<div class="drop-zone">
<input type="file" id="fw-file" accept=".bin,.ota">
<div style="color:var(--ghost)">Drop <span style="color:var(--cyan)">firmware.bin</span> here or click</div>
</div>
<div id="file-info" style="display:none;margin-top:8px;font-size:11px;color:var(--ghost)">
<span id="file-name"></span> — <span id="file-size"></span></div>
<div class="progress-wrap" id="progress-wrap">
<div class="progress-bar"><div class="progress-fill" id="progress-fill"></div></div>
<div class="progress-text" id="progress-text">0%</div></div>
<div id="upload-msg" class="status-msg"></div>
<div class="btn-row"><button class="btn" id="btn-upload" disabled>Upload &amp; Flash</button></div>
</div>

<div class="panel"><div class="panel-title">URL Update</div>
<input type="text" style="width:100%;padding:7px;background:var(--s1);border:1px solid rgba(0,240,255,0.15);color:var(--cyan);font-family:inherit;font-size:12px;border-radius:3px;margin-bottom:8px"
id="url-input" placeholder="https://example.com/firmware.bin">
<div class="btn-row"><button class="btn" id="btn-url">Pull Update</button></div></div>

<div class="panel"><div class="panel-title">History</div>
<table><thead><tr><th>Version</th><th>Source</th><th>Status</th></tr></thead>
<tbody id="history-body"><tr><td colspan="3" style="color:var(--ghost)">Loading...</td></tr></tbody></table></div>

<div class="panel"><div class="panel-title">Flash from SD Card</div>
<div id="sd-status" style="font-size:11px;color:var(--ghost);margin-bottom:8px">Checking SD card...</div>
<div id="sd-files" style="max-height:200px;overflow-y:auto"></div>
<div id="sd-msg" class="status-msg"></div>
</div>

<div class="panel danger-zone"><div class="panel-title" style="color:var(--mag)">Danger Zone</div>
<div class="btn-row"><button class="btn" id="btn-validate" style="border-color:rgba(5,255,161,0.3);color:var(--green)">Validate Firmware</button>
<button class="btn danger" id="btn-rollback">Rollback</button>
<button class="btn danger" id="btn-reboot">Reboot</button></div></div>

<script>
(function(){
var $=function(s){return document.getElementById(s)};
var STATES=['IDLE','CHECKING','DOWNLOADING','WRITING','VERIFYING','READY_REBOOT','FAILED'];
var SC=['state-idle','state-active','state-active','state-active','state-active','state-ok','state-err'];
function fb(b){return b>=1048576?(b/1048576).toFixed(1)+'MB':b>=1024?(b/1024).toFixed(1)+'KB':b+'B';}
function ft(s){return s<60?s+'s':s<3600?Math.floor(s/60)+'m '+s%60+'s':Math.floor(s/3600)+'h';}
function sm(c,m){var e=$('upload-msg');e.className='status-msg show '+c;e.textContent=m;}
function rs(){fetch('/api/ota/status').then(function(r){return r.json()}).then(function(d){
$('cur-ver').textContent=d.cv||'?';$('cur-part').textContent=d.ap||'?';
$('next-part').textContent=d.np||'?';$('part-size').textContent=fb(d.ps||0);
$('uptime').textContent=ft(d.up||0);var si=d.st||0;
$('ota-state').innerHTML='<span class="state-label '+(SC[si]||'state-idle')+'">'+(STATES[si]||'?')+'</span>';
if(si>=3&&si<=4){$('progress-wrap').classList.add('show');$('progress-fill').style.width=d.pp+'%';$('progress-text').textContent=d.pp+'%';}
}).catch(function(){});}
var sf=null;
$('fw-file').onchange=function(){if(this.files.length){sf=this.files[0];
$('file-name').textContent=sf.name;$('file-size').textContent=fb(sf.size);
$('file-info').style.display='block';$('btn-upload').disabled=false;}};
$('btn-upload').onclick=function(){if(!sf)return;this.disabled=true;
$('progress-wrap').classList.add('show');
var fd=new FormData();fd.append('firmware',sf,sf.name);
var xhr=new XMLHttpRequest();xhr.open('POST','/api/ota/upload',true);
xhr.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);
$('progress-fill').style.width=p+'%';$('progress-text').textContent='Uploading: '+p+'%';}};
xhr.onload=function(){try{var d=JSON.parse(xhr.responseText);
if(d.ok){sm('ok',d.msg||'Done');$('progress-text').textContent='Complete!';}
else sm('err',d.msg||'Failed');}catch(x){sm('err','Failed');}
$('btn-upload').disabled=false;rs();};
xhr.onerror=function(){sm('err','Connection error');$('btn-upload').disabled=false;};
xhr.send(fd);};
$('btn-url').onclick=function(){var u=$('url-input').value.trim();if(!u)return;this.disabled=true;
fetch('/api/ota/url',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({url:u})}).then(function(r){return r.json()}).then(function(d){
sm(d.ok?'ok':'err',d.msg||'Failed');$('btn-url').disabled=false;rs();
}).catch(function(){sm('err','Failed');$('btn-url').disabled=false;});};
$('btn-rollback').onclick=function(){if(!confirm('Rollback to previous firmware?'))return;
fetch('/api/ota/rollback',{method:'POST'}).then(function(r){return r.json()}).then(function(d){
sm(d.ok?'ok':'err',d.msg);rs();});};
$('btn-reboot').onclick=function(){if(!confirm('Reboot device now?'))return;
fetch('/api/ota/reboot',{method:'POST'}).then(function(){sm('ok','Rebooting...');
setTimeout(function(){location.reload()},10000);});};
$('btn-validate').onclick=function(){
fetch('/api/ota/validate',{method:'POST'}).then(function(r){return r.json()}).then(function(d){
sm(d.ok?'ok':'err',d.msg);}).catch(function(){sm('err','Failed');});};
fetch('/api/ota/history').then(function(r){return r.json()}).then(function(es){
var tb=$('history-body');if(!es.length){tb.innerHTML='<tr><td colspan="3" style="color:var(--ghost)">No history</td></tr>';return;}
var h='';es.forEach(function(e){h+='<tr><td style="color:var(--cyan)">'+e.version+'</td><td>'+
e.source+'</td><td><span class="dot '+(e.success?'ok':'fail')+'"></span>'+
(e.success?'OK':'FAIL')+'</td></tr>';});tb.innerHTML=h;}).catch(function(){});
rs();setInterval(rs,5000);
function sdSm(c,m){var e=$('sd-msg');e.className='status-msg show '+c;e.textContent=m;}
function loadSdFiles(p){
fetch('/api/fs/sd/list?path='+(p||'/')).then(function(r){return r.json()}).then(function(d){
var c=$('sd-files');$('sd-status').textContent='SD Card: '+d.path;
var h='<table style="width:100%"><tr><th>Name</th><th>Size</th><th></th></tr>';
if(p&&p!=='/'){h+='<tr><td><a href="#" onclick="loadSdFiles(\''+
(p.replace(/\/[^/]+\/?$/,'/')||'/')+ '\');return false" style="color:var(--ghost)">..</a></td><td></td><td></td></tr>';}
d.files.sort(function(a,b){return a.dir===b.dir?a.name.localeCompare(b.name):(a.dir?-1:1);});
d.files.forEach(function(f){
var fp=(p||'/')+((p||'/').endsWith('/')?'':'/')+f.name;
if(f.dir){h+='<tr><td><a href="#" onclick="loadSdFiles(\''+fp+'\');return false" style="color:var(--cyan)">'+f.name+'/</a></td><td>DIR</td><td></td></tr>';}
else{var isBin=f.name.endsWith('.bin')||f.name.endsWith('.ota');
h+='<tr><td style="color:'+(isBin?'var(--green)':'var(--text)')+'">'+f.name+'</td><td>'+fb(f.size)+'</td><td>';
if(isBin){h+='<button class="btn" style="padding:3px 8px;font-size:10px" onclick="flashSd(\''+fp+'\')">Flash</button>';}
h+='</td></tr>';}});
h+='</table>';c.innerHTML=h;
}).catch(function(e){$('sd-status').textContent='SD card not available';$('sd-files').innerHTML='';});}
function flashSd(p){if(!confirm('Flash firmware from SD: '+p+'?'))return;
sdSm('ok','Flashing from SD...');
fetch('/api/ota/sd',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({path:p})}).then(function(r){return r.json()}).then(function(d){
sdSm(d.ok?'ok':'err',d.msg||'Failed');rs();}).catch(function(){sdSm('err','Connection error');});}
loadSdFiles('/');
})();
</script></body></html>)rawliteral";

// ── Config Editor page (/config) ────────────────────────────────────────────

static const char CONFIG_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Config Editor</title>
%THEME%
</head><body>
%NAV%
<h1>// Config Editor</h1>
<div class="card">
<form method="POST" action="/config">
  <p class="label">/config.json</p>
  <textarea name="json" rows="16">%CONFIG%</textarea>
  <br><br>
  <button type="submit">Save Config</button>
</form>
</div>
</body></html>
)rawliteral";

// ── File Manager page (/files) ──────────────────────────────────────────────

static const char FILES_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>File Manager</title>
%THEME%
</head><body>
%NAV%
<h1>// File Manager</h1>
<div class="card">
<form method="POST" action="/files/upload" enctype="multipart/form-data">
  <input type="file" name="file">
  <button type="submit">Upload</button>
</form>
</div>
<div class="card">
<table>
<tr><th>File</th><th>Size</th><th>Action</th></tr>
%FILELIST%
</table>
</div>
</body></html>
)rawliteral";

// ── Helper: replace template placeholder ────────────────────────────────────

static String templateReplace(const char* tpl, const char* key, const String& value) {
    String html(tpl);
    html.replace(key, value);
    return html;
}

// ── Helper: build uptime string ─────────────────────────────────────────────

static String uptimeString() {
    uint32_t sec = millis() / 1000;
    uint32_t d = sec / 86400; sec %= 86400;
    uint32_t h = sec / 3600;  sec %= 3600;
    uint32_t m = sec / 60;    sec %= 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ud %uh %um %us", d, h, m, sec);
    return String(buf);
}

// ── Helper: RSSI to percentage ──────────────────────────────────────────────

static int rssiToPercent(int rssi) {
    if (rssi >= -50) return 100;
    if (rssi <= -100) return 0;
    return 2 * (rssi + 100);
}

// ── init / stop / process ───────────────────────────────────────────────────

bool WebServerHAL::init(uint16_t port) {
    if (_running) return true;

    _port = port;
    _server = new WebServer(port);
    if (!_server) {
        DBG_ERROR("web", "Failed to allocate WebServer");
        return false;
    }
    _instance = this;

    _server->begin();
    _running = true;

    // Cache IP
    IPAddress ip = WiFi.localIP();
    snprintf(_ip, sizeof(_ip), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

    DBG_INFO("web", "Server started on port %u  IP: %s", _port, _ip);
    return true;
}

void WebServerHAL::stop() {
    if (!_running || !_server) return;
    _server->stop();
    delete _server;
    _server = nullptr;
    _running = false;
    DBG_INFO("web", "Server stopped");
}

bool WebServerHAL::isRunning() const { return _running; }

void WebServerHAL::process() {
    if (_running && _server) {
        if (_dnsServer) _dnsServer->processNextRequest();
        _server->handleClient();

        // Httpd watchdog: if WiFi is connected but no requests for 60s,
        // restart the server (recovers from socket exhaustion)
        static uint32_t _lastReqCount = 0;
        static uint32_t _lastCheckMs = 0;
        uint32_t now = millis();
        if (now - _lastCheckMs > 60000) {
            _lastCheckMs = now;
            if (_requestCount == _lastReqCount && WiFi.isConnected()) {
                DBG_WARN("web", "No requests in 60s — restarting httpd");
                _server->close();
                _server->begin();
            }
            _lastReqCount = _requestCount;
        }
    }
}

// ── Route registration ──────────────────────────────────────────────────────

void WebServerHAL::on(const char* uri, const char* method, WebRequestHandler handler) {
    if (!_server) return;

    // Capture handler and instance
    WebServerHAL* self = this;
    auto wrappedHandler = [handler, self]() {
        self->_requestCount++;
        handler((void*)_server);
    };

    if (strcmp(method, "POST") == 0) {
        _server->on(uri, HTTP_POST, wrappedHandler);
    } else {
        _server->on(uri, HTTP_GET, wrappedHandler);
    }
}

// ── Helpers ─────────────────────────────────────────────────────────────────

void WebServerHAL::setBleProvider(BleJsonProvider provider) {
    _bleProvider = provider;
}

void WebServerHAL::setDiagProvider(DiagJsonProvider provider) {
    _diagProvider = provider;
}

void WebServerHAL::setDiagHealthProvider(DiagJsonProvider provider) {
    _diagHealthProvider = provider;
}

void WebServerHAL::setDiagEventsProvider(DiagJsonProvider provider) {
    _diagEventsProvider = provider;
}

void WebServerHAL::setDiagAnomaliesProvider(DiagJsonProvider provider) {
    _diagAnomaliesProvider = provider;
}

void WebServerHAL::setMeshProvider(MeshJsonProvider provider) {
    _meshProvider = provider;
}

void WebServerHAL::setGisTileProvider(GisTileProvider provider) {
    _gisTileProvider = provider;
}

void WebServerHAL::setGisLayerProvider(GisLayerProvider provider) {
    _gisLayerProvider = provider;
}

void WebServerHAL::sendResponse(int code, const char* contentType, const char* body) {
    if (_server) _server->send(code, contentType, body);
}

void WebServerHAL::sendJson(int code, const char* json) {
    if (_server) _server->send(code, "application/json", json);
}

// ── Captive Portal ──────────────────────────────────────────────────────

void WebServerHAL::startCaptivePortal() {
    if (!_server) return;

    _dnsServer = new DNSServer();
    _dnsServer->start(53, "*", WiFi.softAPIP());

    // Note: onNotFound handler in addErrorPages() checks _dnsServer and
    // redirects to /wifi when captive portal is active. No need to set it here.

    // Android/iOS captive portal detection endpoints
    _server->on("/generate_204", HTTP_GET, []() {
        _server->sendHeader("Location", "http://192.168.4.1/wifi", true);
        _server->send(302, "text/plain", "");
    });
    _server->on("/hotspot-detect.html", HTTP_GET, []() {
        _server->sendHeader("Location", "http://192.168.4.1/wifi", true);
        _server->send(302, "text/plain", "");
    });
    _server->on("/connecttest.txt", HTTP_GET, []() {
        _server->sendHeader("Location", "http://192.168.4.1/wifi", true);
        _server->send(302, "text/plain", "");
    });

    DBG_INFO("web", "Captive portal started — all DNS queries redirect to 192.168.4.1");
}

void WebServerHAL::stopCaptivePortal() {
    if (_dnsServer) {
        _dnsServer->stop();
        delete _dnsServer;
        _dnsServer = nullptr;
        DBG_INFO("web", "Captive portal stopped");
    }
}

// ── mDNS ────────────────────────────────────────────────────────────────────

bool WebServerHAL::startMDNS(const char* hostname) {
    if (!MDNS.begin(hostname)) {
        DBG_ERROR("web", "mDNS failed for hostname '%s'", hostname);
        return false;
    }
    MDNS.addService("http", "tcp", _port);
    DBG_INFO("web", "mDNS started: %s.local", hostname);
    return true;
}

// ── Info ────────────────────────────────────────────────────────────────────

uint16_t WebServerHAL::getPort() const { return _port; }
uint32_t WebServerHAL::getRequestCount() const { return _requestCount; }

const char* WebServerHAL::getIP() const {
    return _ip;
}

// ── addDashboard() ──────────────────────────────────────────────────────────

void WebServerHAL::addDashboard() {
    if (!_server) return;

    WebServerHAL* self = this;

    _server->on("/", HTTP_GET, [self]() {
        self->_requestCount++;

        String html(FPSTR(DASHBOARD_HTML));
        html.replace("%THEME%",     FPSTR(THEME_CSS));
        html.replace("%NAV%",       FPSTR(NAV_HTML));
        html.replace("%BOARD%",     "ESP32-S3");

        uint8_t mac[6];
        WiFi.macAddress(mac);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        html.replace("%MAC%", macStr);
        html.replace("%IP%", WiFi.localIP().toString());
        html.replace("%UPTIME%", uptimeString());
        html.replace("%HEAP%", String(ESP.getFreeHeap()));
        html.replace("%PSRAM_TOTAL%", String(ESP.getPsramSize()));
        html.replace("%PSRAM_FREE%",  String(ESP.getFreePsram()));

        int rssi = WiFi.RSSI();
        html.replace("%RSSI%", String(rssi));
        html.replace("%RSSI_PCT%", String(rssiToPercent(rssi)));
        html.replace("%REQCOUNT%", String(self->_requestCount));

        // Build capabilities badges
        String caps;
        auto addBadge = [&](const char* name, bool active) {
            caps += "<span class=\"cap-badge ";
            caps += active ? "active" : "inactive";
            caps += "\"><span class=\"status-dot ";
            caps += active ? "on" : "off";
            caps += "\"></span>";
            caps += name;
            caps += "</span>";
        };

        addBadge("WiFi", WiFi.isConnected());
        addBadge("WebServer", true);
#if defined(ENABLE_BLE_SCANNER)
        addBadge("BLE Scanner", true);
#endif
#if defined(ENABLE_HEARTBEAT)
        addBadge("Heartbeat", true);
#endif
#if defined(HAS_CAMERA) && HAS_CAMERA
        addBadge("Camera", true);
#endif
#if defined(HAS_IMU) && HAS_IMU
        addBadge("IMU", true);
#endif
#if defined(HAS_AUDIO) && HAS_AUDIO
        addBadge("Audio", true);
#endif
#if defined(HAS_RTC) && HAS_RTC
        addBadge("RTC", true);
#endif
#if defined(HAS_PMIC) && HAS_PMIC
        addBadge("PMIC", true);
#endif
#if defined(HAS_SDCARD) && HAS_SDCARD
        addBadge("SD Card", true);
#endif
#if defined(ENABLE_ESPNOW)
        addBadge("ESP-NOW Mesh", true);
#endif
        html.replace("%CAPABILITIES%", caps);

        // BLE card visibility
#if defined(ENABLE_BLE_SCANNER)
        html.replace("%BLE_DISPLAY%", "block");
        html.replace("%BLE_STATUS%", "Checking...");
        html.replace("%BLE_COUNT%", "...");
#else
        html.replace("%BLE_DISPLAY%", "none");
        html.replace("%BLE_STATUS%", "");
        html.replace("%BLE_COUNT%", "");
#endif

        _server->send(200, "text/html", html);
    });

    DBG_INFO("web", "Dashboard page added at /");
}

// ── addOtaPage() ────────────────────────────────────────────────────────────

void WebServerHAL::addOtaPage() {
    if (!_server) return;

#if OTA_MANAGER_AVAILABLE
    ota_manager::init();
    ota_manager::markValid();  // Confirm current firmware is good (prevents rollback)

    WebServerHAL* self = this;

    // Serve the modern OTA page
    _server->on("/ota", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send(200, "text/html", FPSTR(OTA_HTML_MODERN));
    });
    _server->on("/update", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send(200, "text/html", FPSTR(OTA_HTML_MODERN));
    });

    // GET /api/ota/status — current OTA state
    _server->on("/api/ota/status", HTTP_GET, [self]() {
        self->_requestCount++;
        const auto st = ota_manager::getStatus();  // copy to avoid race
        char* buf = api_buf();
        snprintf(buf, API_BUF_SIZE,
            "{\"st\":%u,\"pp\":%u,\"bw\":%u,\"tb\":%u,"
            "\"cv\":\"%s\",\"nv\":\"%s\",\"err\":\"%s\","
            "\"ap\":\"%s\",\"np\":\"%s\",\"ps\":%u,\"up\":%lu}",
            (unsigned)st.state, (unsigned)st.progress_pct,
            st.bytes_written, st.total_bytes,
            st.current_version, st.new_version, st.error_msg,
            st.active_partition ? st.active_partition : "?",
            st.next_partition ? st.next_partition : "?",
            st.partition_size,
            (unsigned long)(millis() / 1000));
        _server->send(200, "application/json", buf);
    });

    // POST /api/ota/upload — firmware upload (raw binary or multipart)
    _server->on("/api/ota/upload", HTTP_POST,
        // Response handler
        [self]() {
            self->_requestCount++;
            const auto& st = ota_manager::getStatus();
            if (st.state == ota_manager::OTA_READY_REBOOT) {
                _server->send(200, "application/json",
                    "{\"ok\":true,\"msg\":\"Upload complete, ready to reboot\"}");
            } else {
                char buf[128];
                snprintf(buf, sizeof(buf), "{\"ok\":false,\"msg\":\"%s\"}", st.error_msg);
                _server->send(500, "application/json", buf);
            }
        },
        // Multipart upload handler (for form-based uploads)
        [self]() {
            HTTPUpload& upload = _server->upload();
            if (upload.status == UPLOAD_FILE_START) {
                DBG_INFO("web", "OTA multipart start: %s", upload.filename.c_str());
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                ota_manager::updateFromUpload(upload.buf, upload.currentSize, false);
            } else if (upload.status == UPLOAD_FILE_END) {
                ota_manager::updateFromUpload(nullptr, 0, true);
                DBG_INFO("web", "OTA multipart end: %u bytes", upload.totalSize);
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                DBG_WARN("web", "OTA upload aborted");
            }
        }
    );

    // Also handle the legacy /update endpoint for backwards compatibility
    _server->on("/update", HTTP_POST,
        [self]() {
            self->_requestCount++;
            const auto& st = ota_manager::getStatus();
            _server->sendHeader("Connection", "close");
            if (st.state == ota_manager::OTA_READY_REBOOT) {
                _server->send(200, "text/plain", "Update OK — rebooting...");
                delay(500);
                ota_manager::reboot();
            } else {
                _server->send(500, "text/plain", st.error_msg);
            }
        },
        [self]() {
            HTTPUpload& upload = _server->upload();
            if (upload.status == UPLOAD_FILE_START) {
                DBG_INFO("web", "OTA legacy start: %s", upload.filename.c_str());
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                ota_manager::updateFromUpload(upload.buf, upload.currentSize, false);
            } else if (upload.status == UPLOAD_FILE_END) {
                ota_manager::updateFromUpload(nullptr, 0, true);
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                DBG_WARN("web", "OTA legacy upload aborted");
            }
        }
    );

    // POST /api/ota/url — pull firmware from URL
    _server->on("/api/ota/url", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        char url[256];
        if (!json_str(body.c_str(), "url", url, sizeof(url))) {
            _server->send(400, "application/json",
                "{\"ok\":false,\"msg\":\"Missing url field\"}");
            return;
        }

        bool ok = ota_manager::updateFromUrl(url);
        if (ok) {
            _server->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"URL update complete\"}");
        } else {
            const auto& st = ota_manager::getStatus();
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ok\":false,\"msg\":\"%s\"}", st.error_msg);
            _server->send(500, "application/json", buf);
        }
    });

    // POST /api/ota/rollback
    _server->on("/api/ota/rollback", HTTP_POST, [self]() {
        self->_requestCount++;
        bool ok = ota_manager::rollback();
        if (ok) {
            _server->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"Rollback set, reboot to apply\"}");
        } else {
            const auto& st = ota_manager::getStatus();
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ok\":false,\"msg\":\"%s\"}", st.error_msg);
            _server->send(500, "application/json", buf);
        }
    });

    // POST /api/ota/reboot
    _server->on("/api/ota/reboot", HTTP_POST, [self]() {
        self->_requestCount++;
        _server->send(200, "application/json", "{\"ok\":true,\"msg\":\"Rebooting...\"}");
        delay(500);
        ota_manager::reboot();
    });

    // POST /api/ota/validate — manually mark current firmware as valid
    _server->on("/api/ota/validate", HTTP_POST, [self]() {
        self->_requestCount++;
        bool ok = ota_manager::markValid();
        if (ok) {
            _server->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"Firmware marked as valid\"}");
        } else {
            _server->send(500, "application/json",
                "{\"ok\":false,\"msg\":\"Failed to mark firmware valid\"}");
        }
    });

    // GET /api/ota/history
    _server->on("/api/ota/history", HTTP_GET, [self]() {
        self->_requestCount++;
        ota_manager::OtaHistoryEntry entries[5];
        int count = ota_manager::getHistory(entries, 5);
        char* buf = api_buf();
        int pos = snprintf(buf, API_BUF_SIZE, "[");
        for (int i = 0; i < count; i++) {
            if (i > 0) pos += snprintf(buf + pos, API_BUF_SIZE - pos, ",");
            pos += snprintf(buf + pos, API_BUF_SIZE - pos,
                "{\"version\":\"%s\",\"timestamp\":%u,\"success\":%s,\"source\":\"%s\"}",
                entries[i].version, entries[i].timestamp,
                entries[i].success ? "true" : "false", entries[i].source);
        }
        snprintf(buf + pos, API_BUF_SIZE - pos, "]");
        _server->send(200, "application/json", buf);
    });

    DBG_INFO("web", "OTA page + API added (ota_manager)");
#else
    DBG_WARN("web", "OTA manager not available");
#endif
}

// ── addConfigEditor() ───────────────────────────────────────────────────────

void WebServerHAL::addConfigEditor() {
    if (!_server) return;

    WebServerHAL* self = this;

    // GET — show editor with current config
    _server->on("/config", HTTP_GET, [self]() {
        self->_requestCount++;

        String config = "{}";
        File f = LittleFS.open("/config.json", "r");
        if (f) {
            config = f.readString();
            f.close();
        }

        String html(FPSTR(CONFIG_HTML));
        html.replace("%THEME%",  FPSTR(THEME_CSS));
        html.replace("%NAV%",    FPSTR(NAV_HTML));
        html.replace("%CONFIG%", config);
        _server->send(200, "text/html", html);
    });

    // POST — save config
    _server->on("/config", HTTP_POST, [self]() {
        self->_requestCount++;

        if (_server->hasArg("json")) {
            String json = _server->arg("json");
            File f = LittleFS.open("/config.json", "w");
            if (f) {
                f.print(json);
                f.close();
                DBG_INFO("web", "Config saved (%u bytes)", json.length());
            }
        }
        // Redirect back to editor
        _server->sendHeader("Location", "/config", true);
        _server->send(302, "text/plain", "Saved");
    });

    DBG_INFO("web", "Config editor added at /config");
}

// ── addFileManager() ────────────────────────────────────────────────────────

void WebServerHAL::addFileManager() {
    if (!_server) return;

    WebServerHAL* self = this;

    // GET — list files
    _server->on("/files", HTTP_GET, [self]() {
        self->_requestCount++;

        String fileList;
        File root = LittleFS.open("/");
        if (root && root.isDirectory()) {
            File entry = root.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    fileList += "<tr><td>";
                    fileList += entry.name();
                    fileList += "</td><td>";
                    fileList += String(entry.size());
                    fileList += " B</td><td>";
                    fileList += "<form method='POST' action='/files/delete' style='display:inline'>";
                    fileList += "<input type='hidden' name='path' value='/";
                    fileList += entry.name();
                    fileList += "'>";
                    fileList += "<button class='danger' type='submit'>Delete</button>";
                    fileList += "</form></td></tr>";
                }
                entry = root.openNextFile();
            }
        }
        if (fileList.length() == 0) {
            fileList = "<tr><td colspan='3' style='color:#666'>No files</td></tr>";
        }

        String html(FPSTR(FILES_HTML));
        html.replace("%THEME%",    FPSTR(THEME_CSS));
        html.replace("%NAV%",      FPSTR(NAV_HTML));
        html.replace("%FILELIST%", fileList);
        _server->send(200, "text/html", html);
    });

    // POST — upload file
    _server->on("/files/upload", HTTP_POST,
        [self]() {
            self->_requestCount++;
            _server->sendHeader("Location", "/files", true);
            _server->send(302, "text/plain", "Uploaded");
        },
        [self]() {
            HTTPUpload& upload = _server->upload();
            static File uploadFile;
            if (upload.status == UPLOAD_FILE_START) {
                String path = "/" + upload.filename;
                uploadFile = LittleFS.open(path.c_str(), "w");
                DBG_INFO("web", "File upload: %s", path.c_str());
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
            } else if (upload.status == UPLOAD_FILE_END) {
                if (uploadFile) {
                    uploadFile.close();
                    DBG_INFO("web", "File uploaded: %u bytes", upload.totalSize);
                }
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                if (uploadFile) {
                    uploadFile.close();
                    DBG_WARN("web", "File upload aborted");
                }
            }
        }
    );

    // POST — delete file
    _server->on("/files/delete", HTTP_POST, [self]() {
        self->_requestCount++;

        if (_server->hasArg("path")) {
            String path = _server->arg("path");
            if (LittleFS.remove(path.c_str())) {
                DBG_INFO("web", "File deleted: %s", path.c_str());
            } else {
                DBG_WARN("web", "File delete failed: %s", path.c_str());
            }
        }
        _server->sendHeader("Location", "/files", true);
        _server->send(302, "text/plain", "Deleted");
    });

    DBG_INFO("web", "File manager added at /files");
}

// ── SD Card Storage page (/storage) ──────────────────────────────────────

static const char STORAGE_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tritium-OS // SD Card</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--cyan:#00f0ff;--mag:#ff2a6d;--green:#05ffa1;--yellow:#fcee0a;
--void:#0a0a0f;--s1:#0e0e14;--s2:#12121a;--s3:#1a1a2e;
--ghost:#8888aa;--text:#c8d0dc;--bright:#e0e0ff}
body{background:var(--void);color:var(--text);font-family:'Courier New',monospace;
font-size:13px;padding:16px;max-width:800px;margin:0 auto}
h1{color:var(--cyan);font-size:16px;letter-spacing:0.15em;text-transform:uppercase;
padding:12px 0;border-bottom:1px solid rgba(0,240,255,0.15);margin-bottom:16px}
.panel{background:var(--s2);border:1px solid rgba(0,240,255,0.08);border-radius:4px;
padding:14px;margin-bottom:12px}
.panel-title{color:var(--bright);font-size:11px;letter-spacing:0.12em;
text-transform:uppercase;margin-bottom:10px}
.btn{padding:5px 12px;border:1px solid rgba(0,240,255,0.3);background:transparent;
color:var(--cyan);font-family:inherit;font-size:11px;cursor:pointer;border-radius:3px;margin:2px}
.btn:hover{background:rgba(0,240,255,0.1)}
.btn.danger{border-color:rgba(255,42,109,0.3);color:var(--mag)}
.btn.green{border-color:rgba(5,255,161,0.3);color:var(--green)}
.info-grid{display:grid;grid-template-columns:auto 1fr;gap:4px 16px;font-size:12px}
.info-grid .k{color:var(--ghost)}.info-grid .v{color:var(--cyan)}
table{width:100%;border-collapse:collapse;font-size:12px}
th{text-align:left;color:var(--ghost);font-size:10px;padding:4px 8px;border-bottom:1px solid rgba(0,240,255,0.08)}
td{padding:5px 8px;border-bottom:1px solid rgba(255,255,255,0.03)}
tr:hover{background:rgba(0,240,255,0.03)}
.path-bar{font-size:12px;color:var(--cyan);margin-bottom:10px;word-break:break-all}
.path-bar a{color:var(--cyan);text-decoration:none}.path-bar a:hover{text-decoration:underline}
.drop-zone{border:2px dashed rgba(0,240,255,0.2);border-radius:6px;padding:24px 16px;
text-align:center;position:relative;background:var(--s1);margin-top:10px}
.drop-zone input{position:absolute;inset:0;opacity:0;cursor:pointer}
.progress-bar{height:14px;background:var(--s3);border-radius:3px;overflow:hidden;margin:8px 0;display:none}
.progress-fill{height:100%;width:0%;background:var(--cyan);transition:width 0.3s}
.msg{padding:6px 10px;border-radius:3px;font-size:11px;margin:6px 0;display:none}
.msg.show{display:block}.msg.ok{background:rgba(5,255,161,0.08);color:var(--green)}
.msg.err{background:rgba(255,42,109,0.08);color:var(--mag)}
</style></head><body>
<h1>// SD Card Storage</h1>
<div class="panel"><div class="panel-title">Card Info</div>
<div class="info-grid">
<span class="k">Status</span><span class="v" id="sd-mounted">---</span>
<span class="k">Total</span><span class="v" id="sd-total">---</span>
<span class="k">Used</span><span class="v" id="sd-used">---</span>
<span class="k">Free</span><span class="v" id="sd-free">---</span>
</div></div>
<div class="panel"><div class="panel-title">File Browser</div>
<div class="path-bar" id="path-bar">/</div>
<div id="file-list"></div>
<div class="drop-zone" id="drop-zone">
<input type="file" id="upload-file" multiple>
<div style="color:var(--ghost)">Drop files here or click to <span style="color:var(--cyan)">upload</span></div>
</div>
<div class="progress-bar" id="up-bar"><div class="progress-fill" id="up-fill"></div></div>
<div class="msg" id="msg"></div>
</div>
<script>
(function(){
var $=function(s){return document.getElementById(s)};
var curPath='/';
function fb(b){return b>=1073741824?(b/1073741824).toFixed(1)+' GB':b>=1048576?(b/1048576).toFixed(1)+' MB':b>=1024?(b/1024).toFixed(1)+' KB':b+' B';}
function sm(c,m){var e=$('msg');e.className='msg show '+c;e.textContent=m;setTimeout(function(){e.className='msg';},4000);}
function loadInfo(){
fetch('/api/fs/sd/info').then(function(r){return r.json()}).then(function(d){
$('sd-mounted').textContent=d.mounted?'Mounted':'Not mounted';
if(d.mounted){$('sd-total').textContent=fb(d.total);$('sd-used').textContent=fb(d.used);
$('sd-free').textContent=fb(d.free);}
});}
function buildPathBar(p){
var parts=p.split('/').filter(function(x){return x;});
var h='<a href="#" onclick="nav(\'/\');return false">/</a>';
var acc='/';
parts.forEach(function(part){acc+=part+'/';
h+=' <a href="#" onclick="nav(\''+acc+'\');return false">'+part+'/</a>';});
$('path-bar').innerHTML=h;
}
function nav(p){curPath=p;loadDir();}
window.nav=nav;
function loadDir(){
buildPathBar(curPath);
fetch('/api/fs/sd/list?path='+encodeURIComponent(curPath)).then(function(r){return r.json()}).then(function(d){
var h='<table><tr><th>Name</th><th>Size</th><th>Actions</th></tr>';
if(curPath!=='/'){var parent=curPath.replace(/\/[^/]+\/$/,'/');if(!parent)parent='/';
h+='<tr><td><a href="#" onclick="nav(\''+parent+'\');return false" style="color:var(--ghost)">..</a></td><td></td><td></td></tr>';}
d.files.sort(function(a,b){return a.dir===b.dir?a.name.localeCompare(b.name):(a.dir?-1:1);});
d.files.forEach(function(f){
var fp=curPath+(curPath.endsWith('/')?'':'/')+f.name;
h+='<tr><td>';
if(f.dir){h+='<a href="#" onclick="nav(\''+fp+'/\');return false" style="color:var(--cyan)">'+f.name+'/</a>';}
else{var isBin=f.name.endsWith('.bin')||f.name.endsWith('.ota');
h+='<span style="color:'+(isBin?'var(--green)':'var(--text)')+'">'+f.name+'</span>';}
h+='</td><td>'+(f.dir?'DIR':fb(f.size))+'</td><td>';
if(!f.dir){h+='<a class="btn" href="/api/fs/sd/download?path='+encodeURIComponent(fp)+'" style="padding:2px 6px;font-size:10px">DL</a>';
h+=' <button class="btn danger" style="padding:2px 6px;font-size:10px" onclick="del(\''+fp+'\')">Del</button>';
if(isBin){h+=' <button class="btn green" style="padding:2px 6px;font-size:10px" onclick="flash(\''+fp+'\')">Flash</button>';}}
h+='</td></tr>';});
h+='</table>';$('file-list').innerHTML=h;
}).catch(function(){$('file-list').innerHTML='<div style="color:var(--ghost);padding:10px">SD card not available</div>';});}
window.del=function(p){if(!confirm('Delete '+p+'?'))return;
fetch('/api/fs/sd/delete',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({path:p})}).then(function(r){return r.json()}).then(function(d){
sm(d.ok?'ok':'err',d.ok?'Deleted':'Delete failed');loadDir();
}).catch(function(){sm('err','Error');});};
window.flash=function(p){if(!confirm('Flash firmware from: '+p+'?'))return;
sm('ok','Flashing from SD...');
fetch('/api/ota/sd',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({path:p})}).then(function(r){return r.json()}).then(function(d){
sm(d.ok?'ok':'err',d.msg||'Failed');}).catch(function(){sm('err','Connection error');});};
$('upload-file').onchange=function(){
var files=this.files;if(!files.length)return;
var i=0;var bar=$('up-bar');var fill=$('up-fill');bar.style.display='block';
function next(){if(i>=files.length){bar.style.display='none';sm('ok','Upload complete');loadDir();loadInfo();return;}
var fd=new FormData();fd.append('file',files[i],files[i].name);
var xhr=new XMLHttpRequest();xhr.open('POST','/api/fs/sd/upload?path='+encodeURIComponent(curPath),true);
xhr.upload.onprogress=function(e){if(e.lengthComputable){fill.style.width=Math.round(e.loaded/e.total*100)+'%';}};
xhr.onload=function(){i++;next();};xhr.onerror=function(){sm('err','Upload failed');bar.style.display='none';};
xhr.send(fd);}
next();};
loadInfo();loadDir();})();
</script></body></html>)rawliteral";

// ── addApiEndpoints() ───────────────────────────────────────────────────────

void WebServerHAL::addApiEndpoints() {
    if (!_server) return;

    WebServerHAL* self = this;

    // GET /api/status
    _server->on("/api/status", HTTP_GET, [self]() {
        self->_requestCount++;
        float tempC = temperatureRead();  // Internal ESP32 temp sensor
        char* buf = api_buf();
        const char* fwVer = "unknown";
#if OTA_MANAGER_AVAILABLE
        fwVer = ota_manager::getStatus().current_version;
#endif
        // Format temp as number or null if NaN (ESP32-S3 has no internal sensor)
        char temp_str[16];
        if (isnan(tempC)) {
            strcpy(temp_str, "null");
        } else {
            snprintf(temp_str, sizeof(temp_str), "%.1f", tempC);
        }
        uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snprintf(buf, API_BUF_SIZE,
            "{\"uptime_s\":%lu,\"free_heap\":%lu,\"min_free_heap\":%lu,"
            "\"largest_block\":%lu,\"psram_free\":%lu,"
            "\"rssi\":%d,\"ip\":\"%s\",\"requests\":%lu,"
            "\"ssid\":\"%s\",\"temp_c\":%s,"
            "\"fw_version\":\"%s\",\"cpu_freq\":%lu,\"tasks\":%u}",
            (unsigned long)(millis() / 1000),
            (unsigned long)ESP.getFreeHeap(),
            (unsigned long)min_free,
            (unsigned long)largest_block,
            (unsigned long)ESP.getFreePsram(),
            WiFi.RSSI(),
            WiFi.localIP().toString().c_str(),
            (unsigned long)self->_requestCount,
            WiFi.SSID().c_str(),
            temp_str,
            fwVer,
            (unsigned long)ESP.getCpuFreqMHz(),
            (unsigned)uxTaskGetNumberOfTasks());
        _server->send(200, "application/json", buf);
    });

    // GET /api/board
    _server->on("/api/board", HTTP_GET, [self]() {
        self->_requestCount++;
        uint8_t mac[6];
        WiFi.macAddress(mac);
        char buf[384];
        snprintf(buf, sizeof(buf),
            "{\"board\":\"ESP32-S3\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"flash_size\":%lu,\"psram_size\":%lu,\"cpu_freq\":%lu,"
            "\"sdk\":\"%s\"}",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            (unsigned long)ESP.getFlashChipSize(),
            (unsigned long)ESP.getPsramSize(),
            (unsigned long)ESP.getCpuFreqMHz(),
            ESP.getSdkVersion());
        _server->send(200, "application/json", buf);
    });

    // GET /api/services — list all registered services and their status
    _server->on("/api/services", HTTP_GET, [self]() {
        self->_requestCount++;
        char* buf = api_buf();
        if (!buf) { _server->send(500, "text/plain", "OOM"); return; }
        int n = ServiceRegistry::count();
        int pos = snprintf(buf, API_BUF_SIZE, "{\"count\":%d,\"services\":[", n);
        for (int i = 0; i < n && pos < (int)API_BUF_SIZE - 128; i++) {
            ServiceInterface* svc = ServiceRegistry::at(i);
            if (!svc) continue;
            if (i > 0) buf[pos++] = ',';
            uint8_t caps = svc->capabilities();
            pos += snprintf(buf + pos, API_BUF_SIZE - pos,
                "{\"name\":\"%s\",\"priority\":%d,\"caps\":{\"tick\":%s,"
                "\"cmd\":%s,\"web\":%s,\"shutdown\":%s}}",
                svc->name(), svc->initPriority(),
                (caps & SVC_TICK) ? "true" : "false",
                (caps & SVC_SERIAL_CMD) ? "true" : "false",
                (caps & SVC_WEB_API) ? "true" : "false",
                (caps & SVC_SHUTDOWN) ? "true" : "false");
        }
        snprintf(buf + pos, API_BUF_SIZE - pos, "]}");
        _server->send(200, "application/json", buf);
    });

    // GET /api/shell/apps — list registered shell apps
    _server->on("/api/shell/apps", HTTP_GET, [self]() {
        self->_requestCount++;
        char* buf = api_buf();
#if HAS_SHELL
        int count = tritium_shell::getAppCount();
        int active = tritium_shell::getActiveApp();
        int pos = snprintf(buf, API_BUF_SIZE,
            "{\"count\":%d,\"active\":%d,\"apps\":[", count, active);
        for (int i = 0; i < count && pos < (int)API_BUF_SIZE - 128; i++) {
            const auto* app = tritium_shell::getApp(i);
            if (!app) continue;
            if (i > 0) buf[pos++] = ',';
            pos += snprintf(buf + pos, API_BUF_SIZE - pos,
                "{\"index\":%d,\"name\":\"%s\",\"description\":\"%s\",\"system\":%s}",
                i,
                app->name ? app->name : "",
                app->description ? app->description : "",
                app->is_system ? "true" : "false");
        }
        snprintf(buf + pos, API_BUF_SIZE - pos, "]}");
#else
        snprintf(buf, API_BUF_SIZE, "{\"count\":0,\"active\":-1,\"apps\":[]}");
#endif
        _server->send(200, "application/json", buf);
    });

    // POST /api/shell/launch — launch an app by index
    _server->on("/api/shell/launch", HTTP_POST, [self]() {
        self->_requestCount++;
#if HAS_SHELL
        String body = _server->arg("plain");
        const char* s = body.c_str();
        int idx = json_int(s, "index", -1);
        int count = tritium_shell::getAppCount();
        if (idx < 0 || idx >= count) {
            _server->send(400, "application/json", "{\"error\":\"invalid index\"}");
            return;
        }
        tritium_shell::showApp(idx);
        _server->send(200, "application/json", "{\"ok\":true}");
#else
        _server->send(501, "application/json", "{\"error\":\"no shell\"}");
#endif
    });

    // POST /api/shell/home — return to launcher
    _server->on("/api/shell/home", HTTP_POST, [self]() {
        self->_requestCount++;
#if HAS_SHELL
        tritium_shell::showLauncher();
        _server->send(200, "application/json", "{\"ok\":true}");
#else
        _server->send(501, "application/json", "{\"error\":\"no shell\"}");
#endif
    });

    // POST /api/shell/screensaver — control screensaver (dismiss, disable, enable)
    _server->on("/api/shell/screensaver", HTTP_POST, [self]() {
        self->_requestCount++;
#if HAS_SHELL
        String _body = _server->arg("plain");
        const char* s = _body.c_str();
        char action[16];
        json_str(s, "action", action, sizeof(action));
        if (strcmp(action, "dismiss") == 0) {
            shell_screensaver::dismiss();
            _server->send(200, "application/json", "{\"ok\":true}");
        } else if (strcmp(action, "disable") == 0) {
            shell_screensaver::dismiss();
            shell_screensaver::setTimeoutS(0);
            _server->send(200, "application/json", "{\"ok\":true,\"timeout\":0}");
        } else if (strcmp(action, "enable") == 0) {
            shell_screensaver::reloadSettings();
            _server->send(200, "application/json", "{\"ok\":true}");
        } else {
            bool active = shell_screensaver::isActive();
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"active\":%s}", active ? "true" : "false");
            _server->send(200, "application/json", buf);
        }
#else
        _server->send(501, "application/json", "{\"error\":\"no shell\"}");
#endif
    });

    // GET /api/shell/screensaver — query screensaver state
    _server->on("/api/shell/screensaver", HTTP_GET, [self]() {
        self->_requestCount++;
#if HAS_SHELL
        bool active = shell_screensaver::isActive();
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"active\":%s}", active ? "true" : "false");
        _server->send(200, "application/json", buf);
#else
        _server->send(501, "application/json", "{\"error\":\"no shell\"}");
#endif
    });

    // POST /api/reboot
    _server->on("/api/reboot", HTTP_POST, [self]() {
        self->_requestCount++;
        _server->send(200, "application/json", "{\"status\":\"rebooting\"}");
        delay(500);
        ESP.restart();
    });

    // GET /api/scan — WiFi scan with SSID classification
    _server->on("/api/scan", HTTP_GET, [self]() {
        self->_requestCount++;
        int n = WiFi.scanNetworks();
        char* buf = api_buf();
        if (!buf) { _server->send(500, "text/plain", "OOM"); return; }
        int pos = snprintf(buf, API_BUF_SIZE, "{\"count\":%d,\"networks\":[", n);
        for (int i = 0; i < n && pos < (int)API_BUF_SIZE - 256; i++) {
            // Map wifi_auth_mode_t to our simple auth type
            uint8_t auth = 0;
            switch (WiFi.encryptionType(i)) {
                case WIFI_AUTH_OPEN:         auth = 0; break;
                case WIFI_AUTH_WEP:          auth = 1; break;
                case WIFI_AUTH_WPA_PSK:      auth = 2; break;
                case WIFI_AUTH_WPA2_PSK:     auth = 3; break;
                case WIFI_AUTH_WPA_WPA2_PSK: auth = 4; break;
                case WIFI_AUTH_WPA3_PSK:     auth = 5; break;
                default:                     auth = 3; break;
            }
            auto cl = wifi_classifier::classify(
                WiFi.SSID(i).c_str(), auth, WiFi.RSSI(i));
            if (i > 0) buf[pos++] = ',';
            pos += snprintf(buf + pos, API_BUF_SIZE - pos,
                "{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,"
                "\"encryption\":%d,\"type\":\"%s\",\"type_id\":%d,"
                "\"confidence\":%d}",
                WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                (int)WiFi.encryptionType(i), cl.type_name,
                (int)cl.type, cl.confidence);
        }
        snprintf(buf + pos, API_BUF_SIZE - pos, "]}");
        WiFi.scanDelete();
        _server->send(200, "application/json", buf);
    });

    // GET /api/node — full node identity and capabilities for fleet discovery
    _server->on("/api/node", HTTP_GET, [self]() {
        self->_requestCount++;
        uint8_t mac[6];
        WiFi.macAddress(mac);

        char* buf = api_buf();
        int pos = snprintf(buf, API_BUF_SIZE,
            "{\"tritium\":true,\"version\":\"1.0\","
            "\"device_id\":\"%02X%02X%02X%02X%02X%02X\","
            "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"ip\":\"%s\",\"port\":%u,"
            "\"uptime_s\":%lu,"
            "\"free_heap\":%lu,\"psram_free\":%lu,"
            "\"flash_size\":%lu,\"psram_size\":%lu,"
            "\"cpu_freq\":%lu,"
            "\"rssi\":%d,"
            "\"sdk\":\"%s\","
            "\"requests\":%lu,",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            WiFi.localIP().toString().c_str(),
            self->_port,
            (unsigned long)(millis() / 1000),
            (unsigned long)ESP.getFreeHeap(),
            (unsigned long)ESP.getFreePsram(),
            (unsigned long)ESP.getFlashChipSize(),
            (unsigned long)ESP.getPsramSize(),
            (unsigned long)ESP.getCpuFreqMHz(),
            WiFi.RSSI(),
            ESP.getSdkVersion(),
            (unsigned long)self->_requestCount);

        // Capabilities list based on build flags
        pos += snprintf(buf + pos, API_BUF_SIZE - pos, "\"capabilities\":[");
        bool first = true;
        auto addCap = [&](const char* name) {
            if (!first) buf[pos++] = ',';
            pos += snprintf(buf + pos, API_BUF_SIZE - pos, "\"%s\"", name);
            first = false;
        };
        addCap("webserver");
        addCap("wifi");
#if defined(ENABLE_HEARTBEAT)
        addCap("heartbeat");
#endif
#if defined(ENABLE_BLE_SCANNER)
        addCap("ble_scanner");
#endif
#if defined(HAS_CAMERA) && HAS_CAMERA
        addCap("camera");
#endif
#if defined(HAS_IMU) && HAS_IMU
        addCap("imu");
#endif
#if defined(HAS_AUDIO) && HAS_AUDIO
        addCap("audio");
#endif
#if defined(HAS_RTC) && HAS_RTC
        addCap("rtc");
#endif
#if defined(HAS_PMIC) && HAS_PMIC
        addCap("pmic");
#endif
#if defined(HAS_SDCARD) && HAS_SDCARD
        addCap("sdcard");
#endif
#if defined(ENABLE_DIAG)
        addCap("diagnostics");
#endif
#if defined(ENABLE_LORA)
        addCap("lora");
#endif
#if defined(ENABLE_ESPNOW)
        addCap("espnow_mesh");
#endif
        pos += snprintf(buf + pos, API_BUF_SIZE - pos, "]}");
        _server->send(200, "application/json", buf);
    });

    // GET /api/mesh — mesh topology and health for visualization
    _server->on("/api/mesh", HTTP_GET, [self]() {
        self->_requestCount++;
        if (self->_meshProvider) {
            char* buf = api_buf();
            int len = self->_meshProvider(buf, API_BUF_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", buf);
                return;
            }
        }
        _server->send(200, "application/json",
            "{\"enabled\":false,\"message\":\"ESP-NOW mesh not available\"}");
    });

    // GET /api/diag — full diagnostics report
    _server->on("/api/diag", HTTP_GET, [self]() {
        self->_requestCount++;
        // Build from cached snapshot + live events/anomalies (avoids stack overflow)
        char* buf = api_buf();
        int pos = 0;
#if defined(ENABLE_DIAG) && __has_include("hal_diag.h")
        pos = snprintf(buf, API_BUF_SIZE, "{\"health\":");
        pos += hal_diag::cached_health_to_json(buf + pos, API_BUF_SIZE - pos);
        pos += snprintf(buf + pos, API_BUF_SIZE - pos, ",\"events\":");
        pos += hal_diag::events_to_json(buf + pos, API_BUF_SIZE - pos, 50);
        pos += snprintf(buf + pos, API_BUF_SIZE - pos, ",\"anomalies\":");
        pos += hal_diag::anomalies_to_json(buf + pos, API_BUF_SIZE - pos);
        pos += snprintf(buf + pos, API_BUF_SIZE - pos, "}");
#endif
        if (pos > 0) {
            _server->send(200, "application/json", buf);
        } else {
            _server->send(200, "application/json",
                "{\"enabled\":false,\"message\":\"Diagnostics not available\"}");
        }
    });

    // GET /api/diag/health — current health snapshot only
    _server->on("/api/diag/health", HTTP_GET, [self]() {
        self->_requestCount++;
        // Use cached snapshot to avoid stack overflow from take_snapshot() in httpd
        char* buf = api_buf();
        int len = 0;
#if defined(ENABLE_DIAG) && __has_include("hal_diag.h")
        len = hal_diag::cached_health_to_json(buf, API_BUF_SIZE);
#endif
        if (len > 0) {
            _server->send(200, "application/json", buf);
        } else {
            _server->send(200, "application/json",
                "{\"enabled\":false}");
        }
    });

    // GET /api/diag/events — recent diagnostic events
    _server->on("/api/diag/events", HTTP_GET, [self]() {
        self->_requestCount++;
        if (self->_diagEventsProvider) {
            char* buf = api_buf();
            int len = self->_diagEventsProvider(buf, API_BUF_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", buf);
                return;
            }
        }
        _server->send(200, "application/json",
            "{\"count\":0,\"events\":[]}");
    });

    // GET /api/diag/anomalies — active anomalies
    _server->on("/api/diag/anomalies", HTTP_GET, [self]() {
        self->_requestCount++;
        if (self->_diagAnomaliesProvider) {
            char* buf = api_buf();
            int len = self->_diagAnomaliesProvider(buf, API_BUF_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", buf);
                return;
            }
        }
        _server->send(200, "application/json",
            "{\"count\":0,\"anomalies\":[]}");
    });

    // GET /api/diag/log?offset=0&count=50 — persistent diagnostic event log
#if WEB_HAS_DIAGLOG
    _server->on("/api/diag/log", HTTP_GET, [self]() {
        self->_requestCount++;
        int offset = 0;
        int count = 50;
        if (_server->hasArg("offset")) {
            offset = _server->arg("offset").toInt();
        }
        if (_server->hasArg("count")) {
            count = _server->arg("count").toInt();
            if (count < 1) count = 1;
            if (count > 200) count = 200;
        }
        char* buf = api_buf();
        int len = diaglog_get_json(buf, API_BUF_SIZE, offset, count);
        if (len > 0) {
            _server->send(200, "application/json", buf);
        } else {
            _server->send(200, "application/json",
                "{\"boot_count\":0,\"total\":0,\"returned\":0,\"events\":[]}");
        }
    });

    _server->on("/api/diag/log/clear", HTTP_POST, [self]() {
        self->_requestCount++;
        diaglog_clear();
        _server->send(200, "application/json", "{\"cleared\":true}");
    });
#endif

    // GET /api/diag/frames — frame timing info for flicker detection
    _server->on("/api/diag/frames", HTTP_GET, [self]() {
        self->_requestCount++;
        uint32_t flushes = lvgl_driver::getFlushCount();
        uint32_t last_ms = lvgl_driver::getLastFlushMs();
        uint32_t now = millis();
        uint32_t uptime = now / 1000;
        // Estimate FPS from total flushes over uptime (rough average)
        float avg_fps = uptime > 0 ? (float)flushes / (float)uptime : 0.0f;
        bool is_rgb = lvgl_driver::isRgb();
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"target_fps\":%d,\"avg_fps\":%.1f,"
            "\"total_flushes\":%lu,\"last_flush_ms\":%lu,"
            "\"uptime_s\":%lu,\"rgb_direct\":%s,"
            "\"dropped\":0,\"count\":0,\"frames\":[]}",
            is_rgb ? 60 : 30,
            avg_fps,
            (unsigned long)flushes,
            (unsigned long)(now - last_ms),
            (unsigned long)uptime,
            is_rgb ? "true" : "false");
        _server->send(200, "application/json", buf);
    });

    // GET /api/diag/tasks — FreeRTOS task details
    _server->on("/api/diag/tasks", HTTP_GET, [self]() {
        self->_requestCount++;
        UBaseType_t count = uxTaskGetNumberOfTasks();
        if (count == 0 || count > 32) {
            _server->send(200, "application/json", "{\"tasks\":[]}");
            return;
        }
        TaskStatus_t* arr = (TaskStatus_t*)malloc(count * sizeof(TaskStatus_t));
        if (!arr) {
            _server->send(500, "application/json", "{\"error\":\"alloc\"}");
            return;
        }
        UBaseType_t filled = uxTaskGetSystemState(arr, count, nullptr);
        char* buf = api_buf();
        int pos = snprintf(buf, API_BUF_SIZE, "{\"count\":%u,\"tasks\":[", (unsigned)filled);
        for (UBaseType_t i = 0; i < filled && pos < (int)API_BUF_SIZE - 128; i++) {
            const char* st = "?";
            switch (arr[i].eCurrentState) {
                case eRunning:   st = "run"; break;
                case eReady:     st = "rdy"; break;
                case eBlocked:   st = "blk"; break;
                case eSuspended: st = "sus"; break;
                case eDeleted:   st = "del"; break;
                default: break;
            }
            pos += snprintf(buf + pos, API_BUF_SIZE - pos,
                "%s{\"name\":\"%s\",\"pri\":%u,\"state\":\"%s\",\"stack_hwm\":%u}",
                i > 0 ? "," : "",
                arr[i].pcTaskName,
                (unsigned)arr[i].uxCurrentPriority,
                st,
                (unsigned)arr[i].usStackHighWaterMark);
        }
        pos += snprintf(buf + pos, API_BUF_SIZE - pos, "]}");
        free(arr);
        _server->send(200, "application/json", buf);
    });

    // GET /api/logs — recent log entries
    _server->on("/api/logs", HTTP_GET, [self]() {
        self->_requestCount++;
        char* buf = api_buf();
        int len = WebServerHAL::getLogJson(buf, API_BUF_SIZE);
        if (len > 0) {
            _server->send(200, "application/json", buf);
        } else {
            _server->send(200, "application/json", "{\"lines\":[]}");
        }
    });

    // ── Remote Control Endpoints ──────────────────────────────────────────

#if WEB_HAS_DISPLAY
    // GET /api/remote/screenshot — raw RGB565 framebuffer
    _server->on("/api/remote/screenshot", HTTP_GET, [self]() {
        self->_requestCount++;
        int w = lvgl_driver::getWidth();
        int h = lvgl_driver::getHeight();
        const uint8_t* fb = lvgl_driver::getFramebuffer();

        if (!fb || w <= 0 || h <= 0) {
            _server->send(500, "application/json",
                "{\"error\":\"no framebuffer\"}");
            return;
        }

        size_t fb_size = (size_t)w * h * 2;  // RGB565

        char wStr[8], hStr[8];
        snprintf(wStr, sizeof(wStr), "%d", w);
        snprintf(hStr, sizeof(hStr), "%d", h);
        _server->sendHeader("X-Width", wStr);
        _server->sendHeader("X-Height", hStr);
        _server->sendHeader("Access-Control-Allow-Origin", "*");
        _server->setContentLength(fb_size);
        _server->send(200, "application/octet-stream", "");

        // Stream in 4KB chunks to avoid large stack allocations
        const size_t CHUNK = 4096;
        for (size_t offset = 0; offset < fb_size; offset += CHUNK) {
            size_t len = CHUNK;
            if (offset + len > fb_size) len = fb_size - offset;
            _server->sendContent((const char*)(fb + offset), len);
        }
    });

    // GET /api/remote/info — display info for remote viewer
    _server->on("/api/remote/info", HTTP_GET, [self]() {
        self->_requestCount++;
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"width\":%d,\"height\":%d,\"format\":\"rgb565\",\"rgb\":%s}",
            lvgl_driver::getWidth(), lvgl_driver::getHeight(),
            lvgl_driver::isRgb() ? "true" : "false");
        _server->send(200, "application/json", buf);
    });
#endif

#if WEB_HAS_TOUCH
    // POST /api/remote/touch — inject touch event
    _server->on("/api/remote/touch", HTTP_POST, [self]() {
        self->_requestCount++;
        String _body = _server->arg("plain");
        const char* s = _body.c_str();
        int x = json_int(s, "x");
        int y = json_int(s, "y");
        bool pressed = (strstr(s, "\"pressed\"") && strstr(s, "true"));
        // Clamp to screen bounds
        int w = lvgl_driver::getWidth();
        int h = lvgl_driver::getHeight();
        if (x < 0) x = 0; if (x >= w) x = w - 1;
        if (y < 0) y = 0; if (y >= h) y = h - 1;

        touch_input::inject((uint16_t)x, (uint16_t)y, pressed);
        _server->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/remote/tap — inject press + release
    _server->on("/api/remote/tap", HTTP_POST, [self]() {
        self->_requestCount++;
        String _body = _server->arg("plain");
        const char* s = _body.c_str();
        int x = json_int(s, "x");
        int y = json_int(s, "y");
        int w = lvgl_driver::getWidth();
        int h = lvgl_driver::getHeight();
        if (x < 0) x = 0; if (x >= w) x = w - 1;
        if (y < 0) y = 0; if (y >= h) y = h - 1;

        touch_input::inject((uint16_t)x, (uint16_t)y, true);
        _server->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/remote/swipe — inject a swipe gesture (series of touch points)
    _server->on("/api/remote/swipe", HTTP_POST, [self]() {
        self->_requestCount++;
        String _body = _server->arg("plain");
        const char* s = _body.c_str();
        int x1 = json_int(s, "x1");
        int y1 = json_int(s, "y1");
        int x2 = json_int(s, "x2");
        int y2 = json_int(s, "y2");
        int steps = json_int(s, "steps", 10);
        int delay_ms = json_int(s, "delay_ms", 15);
        int w = lvgl_driver::getWidth();
        int h = lvgl_driver::getHeight();
        if (x1 < 0) x1 = 0; if (x1 >= w) x1 = w - 1;
        if (y1 < 0) y1 = 0; if (y1 >= h) y1 = h - 1;
        if (x2 < 0) x2 = 0; if (x2 >= w) x2 = w - 1;
        if (y2 < 0) y2 = 0; if (y2 >= h) y2 = h - 1;
        if (steps < 2) steps = 2;
        if (steps > 50) steps = 50;
        if (delay_ms < 5) delay_ms = 5;
        if (delay_ms > 100) delay_ms = 100;

        for (int i = 0; i <= steps; i++) {
            float t = (float)i / (float)steps;
            int px = x1 + (int)((x2 - x1) * t);
            int py = y1 + (int)((y2 - y1) * t);
            touch_input::inject((uint16_t)px, (uint16_t)py, true);
            delay(delay_ms);
        }
        touch_input::injectRelease();
        _server->send(200, "application/json", "{\"ok\":true}");
    });

    // GET /api/debug/touch — touch input debug info
    _server->on("/api/debug/touch", HTTP_GET, [self]() {
        self->_requestCount++;
        auto di = touch_input::getDebugInfo();
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"read_cb_calls\":%lu,\"hw_touch_count\":%lu,"
            "\"inject_count\":%lu,\"last_x\":%d,\"last_y\":%d,"
            "\"last_touch_ms\":%lu,\"hw_available\":%s,"
            "\"currently_pressed\":%s,\"last_activity_ms\":%lu}",
            (unsigned long)di.read_cb_calls,
            (unsigned long)di.hw_touch_count,
            (unsigned long)di.inject_count,
            (int)di.last_raw_x, (int)di.last_raw_y,
            (unsigned long)di.last_touch_ms,
            di.hw_available ? "true" : "false",
            di.currently_pressed ? "true" : "false",
            (unsigned long)touch_input::lastActivityMs());
        _server->send(200, "application/json", buf);
    });
#endif

    // ── Settings API ────────────────────────────────────────────────────

#if __has_include("os_settings.h")
    // GET /api/settings — export all settings as JSON
    // GET /api/settings?domain=screensaver — export one domain
    _server->on("/api/settings", HTTP_GET, [self]() {
        self->_requestCount++;
        char* buf = api_buf();
        const char* domain = nullptr;
        if (_server->hasArg("domain")) {
            domain = _server->arg("domain").c_str();
        }
        int len = TritiumSettings::instance().toJson(buf, API_BUF_SIZE, domain);
        if (len < 0) {
            _server->send(500, "application/json", "{\"error\":\"export failed\"}");
        } else {
            _server->sendHeader("Access-Control-Allow-Origin", "*");
            _server->send(200, "application/json", buf);
        }
    });

    // POST /api/settings — import settings from JSON body
    _server->on("/api/settings", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        if (body.length() == 0) {
            _server->send(400, "application/json", "{\"error\":\"empty body\"}");
            return;
        }
        bool ok = TritiumSettings::instance().fromJson(body.c_str());
        _server->send(ok ? 200 : 400, "application/json",
            ok ? "{\"ok\":true}" : "{\"error\":\"import failed\"}");
    });

    // POST /api/settings/reset — factory reset (optional domain param)
    _server->on("/api/settings/reset", HTTP_POST, [self]() {
        self->_requestCount++;
        const char* domain = nullptr;
        if (_server->hasArg("domain")) {
            domain = _server->arg("domain").c_str();
        }
        bool ok = TritiumSettings::instance().factoryReset(domain);
        _server->send(ok ? 200 : 500, "application/json",
            ok ? "{\"ok\":true}" : "{\"error\":\"reset failed\"}");
    });
#endif

    // ── SD Card Filesystem API ─────────────────────────────────────────

    // GET /api/fs/sd/list?path=/ — list files and directories on SD card
    _server->on("/api/fs/sd/list", HTTP_GET, [self]() {
        self->_requestCount++;
        struct stat sd_stat;
        if (stat("/sdcard", &sd_stat) != 0) {
            _server->send(503, "application/json",
                "{\"error\":\"SD card not mounted\"}");
            return;
        }

        String dirPath = "/sdcard";
        if (_server->hasArg("path")) {
            String p = _server->arg("path");
            if (p.length() > 0 && p != "/") {
                if (!sd_path_is_safe(p.c_str())) {
                    _server->send(400, "application/json",
                        "{\"error\":\"Invalid path\"}");
                    return;
                }
                dirPath += p;
            }
        }

        DIR* dir = opendir(dirPath.c_str());
        if (!dir) {
            _server->send(404, "application/json",
                "{\"error\":\"Directory not found\"}");
            return;
        }

        char* buf = api_buf();
        int pos = snprintf(buf, API_BUF_SIZE,
            "{\"path\":\"%s\",\"files\":[",
            dirPath.c_str() + 7);  // strip "/sdcard"

        bool first = true;
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;

            char fullPath[256];
            snprintf(fullPath, sizeof(fullPath), "%s/%s",
                     dirPath.c_str(), ent->d_name);

            struct stat st;
            bool isDir = false;
            long fsize = 0;
            if (stat(fullPath, &st) == 0) {
                isDir = S_ISDIR(st.st_mode);
                fsize = isDir ? 0 : (long)st.st_size;
            }

            if (!first) buf[pos++] = ',';
            first = false;
            pos += snprintf(buf + pos, API_BUF_SIZE - pos,
                "{\"name\":\"%s\",\"size\":%ld,\"dir\":%s}",
                ent->d_name, fsize, isDir ? "true" : "false");

            if (pos > (int)API_BUF_SIZE - 128) break;  // safety
        }
        closedir(dir);

        snprintf(buf + pos, API_BUF_SIZE - pos, "]}");
        _server->sendHeader("Access-Control-Allow-Origin", "*");
        _server->send(200, "application/json", buf);
    });

    // GET /api/fs/sd/info — SD card capacity and usage
    _server->on("/api/fs/sd/info", HTTP_GET, [self]() {
        self->_requestCount++;
        struct stat sd_stat;
        bool mounted = (stat("/sdcard", &sd_stat) == 0);

        char buf[256];
        if (mounted) {
            // Use SD_MMC for capacity info
            uint64_t totalBytes = SD_MMC.totalBytes();
            uint64_t usedBytes = SD_MMC.usedBytes();
            snprintf(buf, sizeof(buf),
                "{\"mounted\":true,\"total\":%llu,\"free\":%llu,\"used\":%llu}",
                (unsigned long long)totalBytes,
                (unsigned long long)(totalBytes - usedBytes),
                (unsigned long long)usedBytes);
        } else {
            snprintf(buf, sizeof(buf), "{\"mounted\":false}");
        }
        _server->send(200, "application/json", buf);
    });

    // GET /api/fs/sd/download?path=/file.bin — download a file from SD card
    _server->on("/api/fs/sd/download", HTTP_GET, [self]() {
        self->_requestCount++;
        if (!_server->hasArg("path")) {
            _server->send(400, "application/json",
                "{\"error\":\"Missing path parameter\"}");
            return;
        }
        String relPath = _server->arg("path");
        if (!sd_path_is_safe(relPath.c_str())) {
            _server->send(400, "application/json",
                "{\"error\":\"Invalid path\"}");
            return;
        }
        char fullPath[256];
        snprintf(fullPath, sizeof(fullPath), "/sdcard%s", relPath.c_str());

        struct stat st;
        if (stat(fullPath, &st) != 0 || S_ISDIR(st.st_mode)) {
            _server->send(404, "application/json",
                "{\"error\":\"File not found\"}");
            return;
        }

        FILE* f = fopen(fullPath, "rb");
        if (!f) {
            _server->send(500, "application/json",
                "{\"error\":\"Cannot open file\"}");
            return;
        }

        // Extract filename for Content-Disposition
        const char* fname = strrchr(relPath.c_str(), '/');
        fname = fname ? fname + 1 : relPath.c_str();

        char header[128];
        snprintf(header, sizeof(header),
            "attachment; filename=\"%s\"", fname);
        _server->sendHeader("Content-Disposition", header);
        _server->setContentLength(st.st_size);
        _server->send(200, "application/octet-stream", "");

        char chunk[1024];
        size_t total = 0;
        while (total < (size_t)st.st_size) {
            size_t n = fread(chunk, 1, sizeof(chunk), f);
            if (n == 0) break;
            _server->sendContent(chunk, n);
            total += n;
        }
        fclose(f);
    });

    // POST /api/fs/sd/delete — delete file from SD card (JSON body: {"path":"/..."})
    _server->on("/api/fs/sd/delete", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        // Extract path from JSON
        const char* s = body.c_str();
        const char* pp = strstr(s, "\"path\":\"");
        if (!pp) {
            _server->send(400, "application/json",
                "{\"ok\":false,\"error\":\"Missing path\"}");
            return;
        }
        pp += 8;
        char path[256];
        int i = 0;
        while (pp[i] && pp[i] != '"' && i < 255) { path[i] = pp[i]; i++; }
        path[i] = '\0';

        if (!sd_path_is_safe(path)) {
            _server->send(400, "application/json",
                "{\"ok\":false,\"error\":\"Invalid path\"}");
            return;
        }

        char fullPath[280];
        snprintf(fullPath, sizeof(fullPath), "/sdcard%s", path);

        if (remove(fullPath) == 0) {
            DBG_INFO("web", "SD file deleted: %s", path);
            _server->send(200, "application/json", "{\"ok\":true}");
        } else {
            _server->send(500, "application/json",
                "{\"ok\":false,\"error\":\"Delete failed\"}");
        }
    });

    // POST /api/fs/sd/mkdir — create directory on SD card
    _server->on("/api/fs/sd/mkdir", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        const char* s = body.c_str();
        const char* pp = strstr(s, "\"path\":\"");
        if (!pp) {
            _server->send(400, "application/json",
                "{\"ok\":false,\"error\":\"Missing path\"}");
            return;
        }
        pp += 8;
        char path[256];
        int i = 0;
        while (pp[i] && pp[i] != '"' && i < 255) { path[i] = pp[i]; i++; }
        path[i] = '\0';

        if (!sd_path_is_safe(path)) {
            _server->send(400, "application/json",
                "{\"ok\":false,\"error\":\"Invalid path\"}");
            return;
        }

        char fullPath[280];
        snprintf(fullPath, sizeof(fullPath), "/sdcard%s", path);

        if (mkdir(fullPath, 0755) == 0) {
            DBG_INFO("web", "SD dir created: %s", path);
            _server->send(200, "application/json", "{\"ok\":true}");
        } else {
            _server->send(500, "application/json",
                "{\"ok\":false,\"error\":\"mkdir failed\"}");
        }
    });

    // POST /api/fs/sd/upload?path=/dir/ — upload file to SD card
    _server->on("/api/fs/sd/upload", HTTP_POST,
        [self]() {
            self->_requestCount++;
            _server->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"Uploaded\"}");
        },
        [self]() {
            HTTPUpload& upload = _server->upload();
            static FILE* sdFile = nullptr;

            if (upload.status == UPLOAD_FILE_START) {
                // Build full path: /sdcard/<dirpath>/<filename>
                String dirPath = "/sdcard";
                if (_server->hasArg("path")) {
                    String p = _server->arg("path");
                    if (!sd_path_is_safe(p.c_str())) {
                        DBG_WARN("web", "SD upload: rejected unsafe path");
                        return;
                    }
                    dirPath += p;
                }
                // Ensure directory exists
                struct stat st;
                if (stat(dirPath.c_str(), &st) != 0) {
                    mkdir(dirPath.c_str(), 0755);
                }
                String fullPath = dirPath;
                if (!fullPath.endsWith("/")) fullPath += "/";
                fullPath += upload.filename;

                sdFile = fopen(fullPath.c_str(), "wb");
                DBG_INFO("web", "SD upload: %s", fullPath.c_str());
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (sdFile) fwrite(upload.buf, 1, upload.currentSize, sdFile);
            } else if (upload.status == UPLOAD_FILE_END) {
                if (sdFile) {
                    fclose(sdFile);
                    sdFile = nullptr;
                    DBG_INFO("web", "SD upload complete: %u bytes",
                             upload.totalSize);
                }
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                if (sdFile) {
                    fclose(sdFile);
                    sdFile = nullptr;
                    DBG_WARN("web", "SD upload aborted");
                }
            }
        }
    );

    // POST /api/ota/sd — flash firmware from SD card file
#if OTA_MANAGER_AVAILABLE
    _server->on("/api/ota/sd", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        char pathBuf[256];
        const char* path = "/firmware.bin";
        if (json_str(body.c_str(), "path", pathBuf, sizeof(pathBuf))) {
            if (!sd_path_is_safe(pathBuf)) {
                _server->send(400, "application/json",
                    "{\"ok\":false,\"msg\":\"Invalid path\"}");
                return;
            }
            path = pathBuf;
        }

        bool ok = ota_manager::updateFromSD(path);
        if (ok) {
            _server->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"SD flash complete, ready to reboot\"}");
        } else {
            const auto& st = ota_manager::getStatus();
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ok\":false,\"msg\":\"%s\"}",
                     st.error_msg);
            _server->send(500, "application/json", buf);
        }
    });
#endif

    DBG_INFO("web", "API endpoints added at /api/*");
}

// ── WiFi Setup page (/wifi) ──────────────────────────────────────────────

static const char WIFI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Setup</title>
%THEME%
</head><body>
%NAV%
<h1>// WiFi Configuration</h1>
<div class="card">
<h2>Current Connection</h2>
<table>
<tr><td>SSID</td><td id="cur_ssid">%CUR_SSID%</td></tr>
<tr><td>IP</td><td>%CUR_IP%</td></tr>
<tr><td>RSSI</td><td>%CUR_RSSI% dBm</td></tr>
<tr><td>Channel</td><td>%CUR_CH%</td></tr>
</table>
</div>
<div class="card">
<h2>Available Networks</h2>
<p class="label">Click scan to discover nearby networks</p>
<button onclick="scanNetworks()">Scan Networks</button>
<table id="scan_table">
<tr><th>SSID</th><th>RSSI</th><th>Ch</th><th>Security</th><th></th></tr>
</table>
</div>
<div class="card">
<h2>Connect to Network</h2>
<form method="POST" action="/wifi/connect">
  <table>
  <tr><td class="label">SSID</td><td><input type="text" name="ssid" id="ssid_input"
    style="background:#0a0a0a;color:#00ffd0;border:1px solid #00ffd044;padding:6px;width:200px;font-family:inherit"></td></tr>
  <tr><td class="label">Password</td><td><input type="password" name="pass"
    style="background:#0a0a0a;color:#00ffd0;border:1px solid #00ffd044;padding:6px;width:200px;font-family:inherit"></td></tr>
  </table>
  <br>
  <button type="submit">Connect</button>
</form>
</div>
<div class="card">
<h2>Saved Networks</h2>
<div id="saved_nets">%SAVED_NETS%</div>
</div>
<script>
function scanNetworks(){
  var t=document.getElementById('scan_table');
  t.innerHTML='<tr><th>SSID</th><th>RSSI</th><th>Ch</th><th>Security</th><th></th></tr><tr><td colspan="5" style="color:#666">Scanning...</td></tr>';
  fetch('/api/scan').then(r=>r.json()).then(d=>{
    var rows='<tr><th>SSID</th><th>Type</th><th>RSSI</th><th>Ch</th><th>Security</th><th></th></tr>';
    d.networks.forEach(n=>{
      var sec=n.encryption==0?'Open':'Encrypted';
      var tp=n.type||'';
      rows+='<tr><td>'+n.ssid+'</td><td><span class="cap-badge">'+tp+'</span></td><td>'+n.rssi+' dBm</td><td>'+n.channel+'</td><td>'+sec+'</td>';
      rows+='<td><button onclick="document.getElementById(\'ssid_input\').value=\''+n.ssid+'\'">Select</button></td></tr>';
    });
    if(d.count==0) rows+='<tr><td colspan="6" style="color:#666">No networks found</td></tr>';
    t.innerHTML=rows;
  });
}
</script>
</body></html>
)rawliteral";

void WebServerHAL::addWiFiSetup() {
    if (!_server) return;

    WebServerHAL* self = this;

    // GET /wifi — show WiFi config page
    _server->on("/wifi", HTTP_GET, [self]() {
        self->_requestCount++;

        String html(FPSTR(WIFI_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));

        html.replace("%CUR_SSID%", WiFi.isConnected() ? WiFi.SSID() : "Not connected");
        html.replace("%CUR_IP%",   WiFi.localIP().toString());
        html.replace("%CUR_RSSI%", String(WiFi.RSSI()));
        html.replace("%CUR_CH%",   String(WiFi.channel()));

        // List saved networks from NVS (simple: read /wifi_nets.txt from LittleFS)
        String saved = "<p style='color:#666'>No saved networks</p>";
        File f = LittleFS.open("/wifi_nets.txt", "r");
        if (f) {
            String content = f.readString();
            f.close();
            if (content.length() > 0) {
                saved = "<table><tr><th>SSID</th><th></th></tr>";
                int start = 0;
                while (start < (int)content.length()) {
                    int nl = content.indexOf('\n', start);
                    if (nl < 0) nl = content.length();
                    String line = content.substring(start, nl);
                    line.trim();
                    if (line.length() > 0) {
                        // Format: ssid\tpassword
                        int tab = line.indexOf('\t');
                        String ssid = (tab > 0) ? line.substring(0, tab) : line;
                        saved += "<tr><td>" + ssid + "</td><td>";
                        saved += "<form method='POST' action='/wifi/forget' style='display:inline'>";
                        saved += "<input type='hidden' name='ssid' value='" + ssid + "'>";
                        saved += "<button class='danger' type='submit'>Forget</button></form></td></tr>";
                    }
                    start = nl + 1;
                }
                saved += "</table>";
            }
        }
        html.replace("%SAVED_NETS%", saved);

        _server->send(200, "text/html", html);
    });

    // POST /wifi/connect — connect to a network and save it
    _server->on("/wifi/connect", HTTP_POST, [self]() {
        self->_requestCount++;

        if (_server->hasArg("ssid")) {
            String ssid = _server->arg("ssid");
            String pass = _server->hasArg("pass") ? _server->arg("pass") : "";

            // Save to file
            File f = LittleFS.open("/wifi_nets.txt", "a");
            if (f) {
                f.print(ssid + "\t" + pass + "\n");
                f.close();
            }

            // Attempt connection
            WiFi.begin(ssid.c_str(), pass.c_str());
            DBG_INFO("web", "WiFi connecting to: %s", ssid.c_str());

            _server->sendHeader("Location", "/wifi", true);
            _server->send(302, "text/plain", "Connecting...");
        } else {
            _server->send(400, "text/plain", "Missing SSID");
        }
    });

    // POST /wifi/forget — remove a saved network
    _server->on("/wifi/forget", HTTP_POST, [self]() {
        self->_requestCount++;

        if (_server->hasArg("ssid")) {
            String target = _server->arg("ssid");
            File f = LittleFS.open("/wifi_nets.txt", "r");
            String kept;
            if (f) {
                while (f.available()) {
                    String line = f.readStringUntil('\n');
                    line.trim();
                    int tab = line.indexOf('\t');
                    String ssid = (tab > 0) ? line.substring(0, tab) : line;
                    if (ssid != target && line.length() > 0) {
                        kept += line + "\n";
                    }
                }
                f.close();
            }
            File w = LittleFS.open("/wifi_nets.txt", "w");
            if (w) { w.print(kept); w.close(); }
            DBG_INFO("web", "WiFi network forgotten: %s", target.c_str());
        }
        _server->sendHeader("Location", "/wifi", true);
        _server->send(302, "text/plain", "Removed");
    });

    // ── WiFi REST API (JSON) ───────────────────────────────────────────
    // These endpoints power both the modern wifi.html UI and fleet management.

#if HAS_WIFI_MANAGER

    // GET /api/wifi/status — current connection info
    _server->on("/api/wifi/status", HTTP_GET, [self]() {
        self->_requestCount++;
        WifiManager* wm = WifiManager::_instance;
        char* buf = api_buf();
        if (!wm) {
            snprintf(buf, API_BUF_SIZE, "{\"available\":false}");
        } else {
            snprintf(buf, API_BUF_SIZE,
                "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\","
                "\"rssi\":%d,\"channel\":%d,\"ap_mode\":%s,"
                "\"state\":%d,\"saved_count\":%d}",
                wm->isConnected() ? "true" : "false",
                wm->isConnected() ? wm->getSSID() : "",
                wm->isConnected() ? wm->getIP() : "",
                wm->isConnected() ? (int)wm->getRSSI() : 0,
                wm->isConnected() ? (int)WiFi.channel() : 0,
                wm->isAPMode() ? "true" : "false",
                (int)wm->getState(),
                wm->getSavedCount());
        }
        _server->sendHeader("Access-Control-Allow-Origin", "*");
        _server->send(200, "application/json", buf);
    });

    // GET /api/wifi/list — saved networks
    _server->on("/api/wifi/list", HTTP_GET, [self]() {
        self->_requestCount++;
        WifiManager* wm = WifiManager::_instance;
        char* buf = api_buf();
        int pos = snprintf(buf, API_BUF_SIZE, "{\"networks\":[");
        if (wm) {
            SavedNetwork nets[WIFI_MAX_SAVED_NETWORKS];
            int count = wm->getSavedNetworks(nets, WIFI_MAX_SAVED_NETWORKS);
            for (int i = 0; i < count && pos < (int)API_BUF_SIZE - 64; i++) {
                if (i > 0) pos += snprintf(buf + pos, API_BUF_SIZE - pos, ",");
                pos += snprintf(buf + pos, API_BUF_SIZE - pos,
                    "{\"ssid\":\"%s\",\"priority\":%d}", nets[i].ssid, i);
            }
        }
        snprintf(buf + pos, API_BUF_SIZE - pos, "]}");
        _server->sendHeader("Access-Control-Allow-Origin", "*");
        _server->send(200, "application/json", buf);
    });

    // POST /api/wifi/add — add a network (JSON body: ssid, password)
    _server->on("/api/wifi/add", HTTP_POST, [self]() {
        self->_requestCount++;
        WifiManager* wm = WifiManager::_instance;
        if (!wm) {
            _server->send(500, "application/json",
                "{\"ok\":false,\"msg\":\"WiFi not available\"}");
            return;
        }
        String body = _server->arg("plain");
        char ssid[33], pass[65];
        if (!json_str(body.c_str(), "ssid", ssid, sizeof(ssid))) {
            _server->send(400, "application/json",
                "{\"ok\":false,\"msg\":\"Missing ssid\"}");
            return;
        }
        json_str(body.c_str(), "password", pass, sizeof(pass));
        bool ok = wm->addNetwork(ssid, pass);
        _server->sendHeader("Access-Control-Allow-Origin", "*");
        _server->send(200, "application/json",
            ok ? "{\"ok\":true,\"msg\":\"Network added\"}"
               : "{\"ok\":false,\"msg\":\"Failed to add (full?)\"}");
    });

    // POST /api/wifi/remove — remove a saved network (JSON body: ssid)
    _server->on("/api/wifi/remove", HTTP_POST, [self]() {
        self->_requestCount++;
        WifiManager* wm = WifiManager::_instance;
        if (!wm) {
            _server->send(500, "application/json",
                "{\"ok\":false,\"msg\":\"WiFi not available\"}");
            return;
        }
        String body = _server->arg("plain");
        char ssid[33];
        if (!json_str(body.c_str(), "ssid", ssid, sizeof(ssid))) {
            _server->send(400, "application/json",
                "{\"ok\":false,\"msg\":\"Missing ssid\"}");
            return;
        }
        bool ok = wm->removeNetwork(ssid);
        _server->sendHeader("Access-Control-Allow-Origin", "*");
        _server->send(200, "application/json",
            ok ? "{\"ok\":true,\"msg\":\"Network removed\"}"
               : "{\"ok\":false,\"msg\":\"Network not found\"}");
    });

    // POST /api/wifi/connect — connect to a specific or auto-connect
    _server->on("/api/wifi/connect", HTTP_POST, [self]() {
        self->_requestCount++;
        WifiManager* wm = WifiManager::_instance;
        if (!wm) {
            _server->send(500, "application/json",
                "{\"ok\":false,\"msg\":\"WiFi not available\"}");
            return;
        }
        String body = _server->arg("plain");
        char ssid[33];
        bool ok;
        if (json_str(body.c_str(), "ssid", ssid, sizeof(ssid))) {
            ok = wm->connect(ssid);
        } else {
            ok = wm->connect();
        }
        _server->sendHeader("Access-Control-Allow-Origin", "*");
        _server->send(200, "application/json",
            ok ? "{\"ok\":true,\"msg\":\"Connected\"}"
               : "{\"ok\":false,\"msg\":\"Connection failed\"}");
    });

    // POST /api/wifi/disconnect — disconnect
    _server->on("/api/wifi/disconnect", HTTP_POST, [self]() {
        self->_requestCount++;
        WifiManager* wm = WifiManager::_instance;
        if (wm) wm->disconnect();
        _server->sendHeader("Access-Control-Allow-Origin", "*");
        _server->send(200, "application/json", "{\"ok\":true,\"msg\":\"Disconnected\"}");
    });

#endif  // HAS_WIFI_MANAGER

    DBG_INFO("web", "WiFi setup added at /wifi");
}

// ── BLE Viewer page (/ble) ──────────────────────────────────────────────

static const char BLE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BLE Scanner</title>
%THEME%
<script>
var refreshTimer;
function startRefresh(){
  loadDevices();
  refreshTimer=setInterval(loadDevices,3000);
}
function loadDevices(){
  fetch('/api/ble').then(r=>r.json()).then(d=>{
    var t=document.getElementById('ble_table');
    var rows='<tr><th>MAC</th><th>RSSI</th><th>Name</th><th>Seen</th><th>Known</th></tr>';
    d.devices.forEach(dev=>{
      var cls=dev.known?'color:#00ffd0':'';
      rows+='<tr style="'+cls+'"><td>'+dev.mac+'</td><td>'+dev.rssi+' dBm</td>';
      rows+='<td>'+(dev.name||'—')+'</td><td>'+dev.seen+'</td>';
      rows+='<td>'+(dev.known?'YES':'')+'</td></tr>';
    });
    if(d.devices.length==0) rows+='<tr><td colspan="5" style="color:#666">No devices detected</td></tr>';
    t.innerHTML=rows;
    document.getElementById('ble_count').textContent=d.total+' devices ('+d.known+' known)';
    document.getElementById('ble_active').textContent=d.active?'Active':'Inactive';
  }).catch(()=>{
    document.getElementById('ble_active').textContent='Not available';
  });
}
</script>
</head><body onload="startRefresh()">
%NAV%
<h1>// BLE Scanner</h1>
<div class="card">
<table>
<tr><td>Scanner Status</td><td id="ble_active">Loading...</td></tr>
<tr><td>Visible Devices</td><td id="ble_count">—</td></tr>
</table>
</div>
<div class="card">
<h2>Detected Devices</h2>
<p class="label">Auto-refreshes every 3 seconds</p>
<table id="ble_table">
<tr><td colspan="5" style="color:#666">Loading...</td></tr>
</table>
</div>
</body></html>
)rawliteral";

void WebServerHAL::addBleViewer() {
    if (!_server) return;

    WebServerHAL* self = this;

    // GET /ble — BLE scanner page
    _server->on("/ble", HTTP_GET, [self]() {
        self->_requestCount++;

        String html(FPSTR(BLE_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));
        _server->send(200, "text/html", html);
    });

    // GET /api/ble — JSON BLE device list (uses provider callback)
    _server->on("/api/ble", HTTP_GET, [self]() {
        self->_requestCount++;

        if (self->_bleProvider) {
            char* buf = api_buf();
            int len = self->_bleProvider(buf, API_BUF_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", buf);
                return;
            }
        }
        _server->send(200, "application/json",
            "{\"active\":false,\"total\":0,\"known\":0,\"devices\":[]}");
    });

    DBG_INFO("web", "BLE viewer added at /ble");
}

// ── Commission page (/commission) ────────────────────────────────────────

static const char COMMISSION_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Commission Node</title>
%THEME%
<style>
#qr-canvas{margin:16px auto;display:block;image-rendering:pixelated}
.qr-wrap{text-align:center;padding:16px;background:#fff;border-radius:8px;
  display:inline-block;margin:12px 0}
.center{text-align:center}
.conn-str{background:#0a0a0a;border:1px solid #00ffd044;padding:10px;
  border-radius:4px;font-family:'Courier New',monospace;color:#00ffd0;
  word-break:break-all;margin:8px 0}
input[type=text],input[type=url]{background:#0a0a0a;color:#00ffd0;
  border:1px solid #00ffd044;padding:6px;width:100%;max-width:400px;
  font-family:inherit;border-radius:4px}
</style>
</head><body>
%NAV%
<h1>// Node Commissioning</h1>

<div class="card">
<h2>Node Identity</h2>
<table>
<tr><td class="label">Device ID</td><td style="font-family:monospace">%DEVICE_ID%</td></tr>
<tr><td class="label">MAC</td><td style="font-family:monospace">%MAC%</td></tr>
<tr><td class="label">Mode</td><td>%MODE%</td></tr>
</table>
</div>

<div class="card">
<h2>Connect to This Node</h2>
<p class="label">Scan this QR code with your phone:</p>
<div class="center">
  <div class="qr-wrap"><canvas id="qr-canvas"></canvas></div>
</div>
<p class="label">Or manually:</p>
<div class="conn-str" id="conn-str">%CONN_STR%</div>
</div>

<div class="card">
<h2>Fleet Server</h2>
<p class="label">URL of the Tritium fleet server this node reports to:</p>
<form method="POST" action="/commission">
  <input type="url" name="fleet_url" value="%FLEET_URL%" placeholder="https://fleet.example.com">
  <br><br>
  <button type="submit">Save</button>
</form>
</div>

<script>
// Minimal QR Code generator (QR Code Model 2, alphanumeric/byte mode)
// Based on Project Nayuki QR Code generator library (MIT license)
// Minified inline to avoid external dependency
var QR=(function(){
'use strict';
function QrCode(dc,ecl){
var size,modules,isFunc;
function init(){
var ver=1,dataLen=dc.length,ecBits;
for(;ver<=40;ver++){
  var cap=getNumDataCodewords(ver,ecl)*8;
  if(dataLen*8+(dataLen>0?4:0)<=cap)break;
}
if(ver>40)throw'Data too long';
size=ver*4+17;
modules=[];isFunc=[];
for(var i=0;i<size;i++){modules.push(new Array(size).fill(false));isFunc.push(new Array(size).fill(false));}
drawFinderPattern(3,3);drawFinderPattern(size-4,3);drawFinderPattern(3,size-4);
var aligns=getAlignmentPatternPositions(ver);
for(var i=0;i<aligns.length;i++)for(var j=0;j<aligns.length;j++){
  if((i==0&&j==0)||(i==0&&j==aligns.length-1)||(i==aligns.length-1&&j==0))continue;
  drawAlignmentPattern(aligns[i],aligns[j]);
}
drawTimingPatterns();
drawFormatBits(0);
if(ver>=7)drawVersionBits();
var rawData=encodeData(dc,ver,ecl);
drawCodewords(rawData);
var bestMask=-1,bestPen=Infinity;
for(var m=0;m<8;m++){
  applyMask(m);drawFormatBits(m);
  var pen=getPenaltyScore();
  if(pen<bestPen){bestPen=pen;bestMask=m;}
  applyMask(m);
}
applyMask(bestMask);drawFormatBits(bestMask);
isFunc=null;
}
function drawFinderPattern(cx,cy){
for(var dy=-4;dy<=4;dy++)for(var dx=-4;dx<=4;dx++){
  var xx=cx+dx,yy=cy+dy;
  if(xx<0||xx>=size||yy<0||yy>=size)continue;
  var d=Math.max(Math.abs(dx),Math.abs(dy));
  setModule(xx,yy,d!=2&&d!=4,true);
}}
function drawAlignmentPattern(cx,cy){
for(var dy=-2;dy<=2;dy++)for(var dx=-2;dx<=2;dx++)
  setModule(cx+dx,cy+dy,Math.max(Math.abs(dx),Math.abs(dy))!=1,true);
}
function drawTimingPatterns(){
for(var i=0;i<size;i++){setModule(6,i,i%2==0,true);setModule(i,6,i%2==0,true);}
}
function drawFormatBits(mask){
var data=ecl.fmtBits<<3|mask,rem=data;
for(var i=0;i<10;i++)rem=(rem<<1)^((rem>>>9)*0x537);
var bits=(data<<10|rem)^0x5412;
for(var i=0;i<=5;i++)setModule(8,i,getBit(bits,i),true);
setModule(8,7,getBit(bits,6),true);setModule(8,8,getBit(bits,7),true);
setModule(7,8,getBit(bits,8),true);
for(var i=9;i<15;i++)setModule(14-i,8,getBit(bits,i),true);
for(var i=0;i<8;i++)setModule(size-1-i,8,getBit(bits,i),true);
setModule(8,size-8,true,true);
for(var i=8;i<15;i++)setModule(8,size-15+i,getBit(bits,i),true);
}
function drawVersionBits(){
var ver=Math.floor((size-17)/4),rem=ver;
for(var i=0;i<12;i++)rem=(rem<<1)^((rem>>>11)*0x1F25);
var bits=ver<<12|rem;
for(var i=0;i<18;i++){
  var bt=getBit(bits,i);
  var a=Math.floor(i/3),b=i%3+size-11;
  setModule(a,b,bt,true);setModule(b,a,bt,true);
}}
function drawCodewords(data){
var i=0,right=size-1;
while(right>=1){
  if(right==6)right=5;
  for(var vert=0;vert<size;vert++){
    for(var j=0;j<2;j++){
      var x=right-j,upward=((right+1)&2)==0;
      var y=upward?size-1-vert:vert;
      if(!isFunc[y][x]&&i<data.length*8){
        modules[y][x]=getBit(data[i>>>3],7-(i&7));i++;
      }
    }
  }
  right-=2;
}}
function applyMask(mask){
for(var y=0;y<size;y++)for(var x=0;x<size;x++){
  if(isFunc[y][x])continue;
  var inv=false;
  switch(mask){
    case 0:inv=(x+y)%2==0;break;case 1:inv=y%2==0;break;
    case 2:inv=x%3==0;break;case 3:inv=(x+y)%3==0;break;
    case 4:inv=(Math.floor(x/3)+Math.floor(y/2))%2==0;break;
    case 5:inv=x*y%2+x*y%3==0;break;
    case 6:inv=(x*y%2+x*y%3)%2==0;break;
    case 7:inv=((x+y)%2+x*y%3)%2==0;break;
  }
  if(inv)modules[y][x]=!modules[y][x];
}}
function getPenaltyScore(){
var pen=0;
for(var y=0;y<size;y++){
  var run=0,last=false;
  for(var x=0;x<size;x++){
    if(modules[y][x]==last){run++;if(run==5)pen+=3;else if(run>5)pen++;}
    else{last=modules[y][x];run=1;}
  }
}
for(var x=0;x<size;x++){
  var run=0,last=false;
  for(var y=0;y<size;y++){
    if(modules[y][x]==last){run++;if(run==5)pen+=3;else if(run>5)pen++;}
    else{last=modules[y][x];run=1;}
  }
}
for(var y=0;y<size-1;y++)for(var x=0;x<size-1;x++){
  var c=modules[y][x];
  if(c==modules[y][x+1]&&c==modules[y+1][x]&&c==modules[y+1][x+1])pen+=3;
}
var dark=0;
for(var y=0;y<size;y++)for(var x=0;x<size;x++)if(modules[y][x])dark++;
var total=size*size;
pen+=Math.abs(Math.ceil(dark*20/total-10))*10;
return pen;
}
function setModule(x,y,dark,func){modules[y][x]=dark;if(func)isFunc[y][x]=true;}
function getBit(x,i){return((x>>>i)&1)!=0;}
init();
this.size=size;
this.getModule=function(x,y){return(x>=0&&x<size&&y>=0&&y<size)?modules[y][x]:false;};
}
function encodeData(data,ver,ecl){
var bb=[];
pushBits(bb,4,4);
pushBits(bb,data.length,ver<=9?8:16);
for(var i=0;i<data.length;i++)pushBits(bb,data.charCodeAt(i),8);
var cap=getNumDataCodewords(ver,ecl)*8;
pushBits(bb,0,Math.min(4,cap-bb.length));
pushBits(bb,0,(8-bb.length%8)%8);
for(var pad=0xEC;bb.length<cap;pad^=0xEC^0x11)pushBits(bb,pad,8);
var bytes=[];for(var i=0;i<bb.length;i+=8){
  var b=0;for(var j=0;j<8;j++)b=(b<<1)|(bb[i+j]||0);bytes.push(b);
}
return addEccAndInterleave(bytes,ver,ecl);
}
function pushBits(bb,val,len){for(var i=len-1;i>=0;i--)bb.push((val>>>i)&1);}
function addEccAndInterleave(data,ver,ecl){
var nb=NUM_EC_BLOCKS[ecl.ord][ver],
    tb=getNumRawDataModules(ver)/8,
    eccLen=(tb-getNumDataCodewords(ver,ecl))/nb,
    shortLen=Math.floor(getNumDataCodewords(ver,ecl)/nb),
    numShort=nb-getNumDataCodewords(ver,ecl)%nb,
    blocks=[],eccBlocks=[];
var gen=reedSolomonGeneratePolynomial(eccLen);
for(var i=0,off=0;i<nb;i++){
  var len=shortLen+(i>=numShort?1:0);
  var blk=data.slice(off,off+len);off+=len;
  blocks.push(blk);
  eccBlocks.push(reedSolomonComputeRemainder(blk,gen));
}
var result=[];
for(var i=0;i<=shortLen;i++)for(var j=0;j<nb;j++){
  if(i==shortLen&&j<numShort)continue;
  result.push(blocks[j][i<blocks[j].length?i:blocks[j].length-1]);
}
for(var i=0;i<eccLen;i++)for(var j=0;j<nb;j++)result.push(eccBlocks[j][i]);
return result;
}
function reedSolomonComputeRemainder(data,gen){
var r=new Array(gen.length).fill(0);
data.forEach(function(b){
  var f=b^r.shift();r.push(0);
  gen.forEach(function(g,i){r[i]^=reedSolomonMultiply(g,f);});
});
return r;
}
function reedSolomonMultiply(x,y){
var z=0;for(var i=7;i>=0;i--){z=(z<<1)^((z>>>7)*0x11D);z^=((y>>>i)&1)*x;}return z;
}
function reedSolomonGeneratePolynomial(degree){
var r=[1];
for(var i=0;i<degree;i++){
  var nr=new Array(r.length+1).fill(0);
  var f=reedSolomonPow(2,i);
  for(var j=0;j<r.length;j++){nr[j]^=reedSolomonMultiply(r[j],f);nr[j+1]^=r[j];}
  r=nr;
}
return r.slice(1);
}
function reedSolomonPow(b,e){var r=1;for(var i=0;i<e;i++)r=reedSolomonMultiply(r,b);return r;}
function getAlignmentPatternPositions(ver){
if(ver==1)return[];
var num=Math.floor(ver/7)+2,step=ver==32?26:Math.ceil((ver*4+4)/(num*2-2))*2;
var pos=[6];for(var i=0,p=ver*4+10;i<num-1;i++,p-=step)pos.splice(1,0,p);
return pos;
}
function getNumRawDataModules(ver){
var s=ver*4+17,r=s*s-8*8*3-2*(s-16)-1-((ver>=2?(Math.floor(ver/7)+2)*(Math.floor(ver/7)+2)-3:0)*25);
if(ver>=7)r-=36;return r;
}
function getNumDataCodewords(ver,ecl){return Math.floor(getNumRawDataModules(ver)/8)-ECC_CODEWORDS_PER_BLOCK[ecl.ord][ver]*NUM_EC_BLOCKS[ecl.ord][ver];}
var ECC_CODEWORDS_PER_BLOCK=[
[null,7,10,15,20,26,18,20,24,30,18,20,24,26,30,22,24,28,30,28,28,28,28,30,30,26,28,30,30,30,30,30,30,30,30,30,30,30,30,30,30],
[null,10,16,26,18,24,16,18,22,22,26,30,22,22,24,24,28,28,26,26,26,26,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28],
[null,13,22,18,26,18,24,18,22,20,24,28,26,24,20,30,24,28,28,26,30,28,30,30,30,30,28,30,30,30,30,30,30,30,30,30,30,30,30,30,30],
[null,17,28,22,16,22,28,26,26,24,28,24,28,22,24,24,30,28,28,26,28,30,24,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30,30]];
var NUM_EC_BLOCKS=[
[null,1,1,1,1,1,2,2,2,2,4,4,4,4,4,6,6,6,6,7,8,8,9,9,10,12,12,12,13,14,15,16,17,18,19,19,20,21,22,24,25],
[null,1,1,1,2,2,4,4,4,4,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8],
[null,1,1,2,2,4,4,6,6,8,8,8,10,12,16,12,17,16,18,21,20,23,23,25,27,29,34,34,35,38,40,43,45,48,51,53,56,59,62,65,68],
[null,1,1,2,4,4,4,5,6,8,8,11,11,16,16,18,16,19,21,25,25,25,34,30,32,35,37,40,42,45,48,51,54,57,60,63,66,70,74,77,81]];
var Ecl={LOW:{ord:0,fmtBits:1},MEDIUM:{ord:1,fmtBits:0},QUARTILE:{ord:2,fmtBits:3},HIGH:{ord:3,fmtBits:2}};
return{QrCode:QrCode,Ecl:Ecl};
})();

// Render QR code to canvas
function renderQR(text){
  var qr=new QR.QrCode(text,QR.Ecl.LOW);
  var scale=Math.max(4,Math.floor(240/qr.size));
  var canvas=document.getElementById('qr-canvas');
  canvas.width=canvas.height=qr.size*scale;
  var ctx=canvas.getContext('2d');
  ctx.fillStyle='#ffffff';
  ctx.fillRect(0,0,canvas.width,canvas.height);
  ctx.fillStyle='#000000';
  for(var y=0;y<qr.size;y++)for(var x=0;x<qr.size;x++){
    if(qr.getModule(x,y))ctx.fillRect(x*scale,y*scale,scale,scale);
  }
}
var connStr=document.getElementById('conn-str').textContent;
renderQR(connStr);
</script>
</body></html>
)rawliteral";

void WebServerHAL::addCommissionPage() {
    if (!_server) return;

    WebServerHAL* self = this;

    // GET /commission — show commissioning page with QR code
    _server->on("/commission", HTTP_GET, [self]() {
        self->_requestCount++;

        String html(FPSTR(COMMISSION_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));

        uint8_t mac[6];
        WiFi.macAddress(mac);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        char deviceId[13];
        snprintf(deviceId, sizeof(deviceId), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        html.replace("%MAC%", macStr);
        html.replace("%DEVICE_ID%", deviceId);

        // Determine mode and connection string for QR code
        wifi_mode_t mode = WiFi.getMode();
        bool isAP = (mode == WIFI_AP || mode == WIFI_AP_STA);

        if (isAP) {
            // AP mode — QR encodes WiFi credentials for phone to join
            String ssid = WiFi.softAPSSID();
            String connStr = "WIFI:T:nopass;S:" + ssid + ";;";
            html.replace("%MODE%", "Access Point: " + ssid);
            html.replace("%CONN_STR%", connStr);
        } else {
            // STA mode — QR encodes the node's web URL
            String ip = WiFi.localIP().toString();
            String connStr = "http://" + ip + "/";
            html.replace("%MODE%", "WiFi Client (" + WiFi.SSID() + ")");
            html.replace("%CONN_STR%", connStr);
        }

        // Fleet URL from config
        String fleetUrl = "";
        File f = LittleFS.open("/config.json", "r");
        if (f) {
            String cfg = f.readString();
            f.close();
            // Simple parse: find "fleet_url":"..."
            int idx = cfg.indexOf("\"fleet_url\"");
            if (idx >= 0) {
                int q1 = cfg.indexOf('\"', idx + 11);
                int q2 = cfg.indexOf('\"', q1 + 1);
                if (q1 >= 0 && q2 > q1) {
                    fleetUrl = cfg.substring(q1 + 1, q2);
                }
            }
        }
        html.replace("%FLEET_URL%", fleetUrl);

        _server->send(200, "text/html", html);
    });

    // POST /commission — save fleet server URL to config
    _server->on("/commission", HTTP_POST, [self]() {
        self->_requestCount++;

        if (_server->hasArg("fleet_url")) {
            String fleetUrl = _server->arg("fleet_url");

            // Read existing config or start fresh
            String cfg = "{}";
            File f = LittleFS.open("/config.json", "r");
            if (f) {
                cfg = f.readString();
                f.close();
            }

            // Simple update: replace or insert fleet_url
            int idx = cfg.indexOf("\"fleet_url\"");
            if (idx >= 0) {
                // Replace existing value
                int q1 = cfg.indexOf('\"', idx + 11);
                int q2 = cfg.indexOf('\"', q1 + 1);
                if (q1 >= 0 && q2 > q1) {
                    cfg = cfg.substring(0, q1 + 1) + fleetUrl + cfg.substring(q2);
                }
            } else {
                // Insert before closing brace
                int brace = cfg.lastIndexOf('}');
                if (brace >= 0) {
                    String prefix = cfg.substring(0, brace);
                    prefix.trim();
                    // Add comma if there's existing content
                    if (prefix.length() > 1 && prefix[prefix.length() - 1] != '{') {
                        prefix += ",";
                    }
                    cfg = prefix + "\"fleet_url\":\"" + fleetUrl + "\"}";
                }
            }

            File w = LittleFS.open("/config.json", "w");
            if (w) {
                w.print(cfg);
                w.close();
                DBG_INFO("web", "Fleet URL saved: %s", fleetUrl.c_str());
            }
        }
        _server->sendHeader("Location", "/commission", true);
        _server->send(302, "text/plain", "Saved");
    });

    DBG_INFO("web", "Commission page added at /commission");
}

// ── Log ring buffer ──────────────────────────────────────────────────────

static const int LOG_RING_SIZE = 128;       // Number of lines to keep
static const int LOG_LINE_MAX  = 128;       // Max chars per line
static char (*_logRing)[LOG_LINE_MAX] = nullptr;  // PSRAM-allocated on first use
static int   _logHead = 0;                  // Next write position
static int   _logCount = 0;                 // Total lines stored (up to LOG_RING_SIZE)
static SemaphoreHandle_t _logMutex = nullptr;

void WebServerHAL::captureLog(const char* line) {
    if (!line || !line[0]) return;
    if (!_logMutex) _logMutex = xSemaphoreCreateMutex();
    if (!_logRing) {
        // Allocate under mutex to prevent double-init race
        if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (!_logRing) {  // Double-check after acquiring lock
                _logRing = (char(*)[LOG_LINE_MAX])heap_caps_calloc(
                    LOG_RING_SIZE, LOG_LINE_MAX, MALLOC_CAP_SPIRAM);
                if (!_logRing) _logRing = (char(*)[LOG_LINE_MAX])calloc(LOG_RING_SIZE, LOG_LINE_MAX);
            }
            xSemaphoreGive(_logMutex);
        }
    }
    if (!_logRing) return;
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        strncpy(_logRing[_logHead], line, LOG_LINE_MAX - 1);
        _logRing[_logHead][LOG_LINE_MAX - 1] = '\0';
        // Strip trailing newline
        int len = strlen(_logRing[_logHead]);
        if (len > 0 && _logRing[_logHead][len - 1] == '\n')
            _logRing[_logHead][len - 1] = '\0';
        _logHead = (_logHead + 1) % LOG_RING_SIZE;
        if (_logCount < LOG_RING_SIZE) _logCount++;
        xSemaphoreGive(_logMutex);
    }
}

int WebServerHAL::getLogJson(char* buf, size_t size) {
    if (!_logRing) return snprintf(buf, size, "{\"count\":0,\"lines\":[]}");
    if (!_logMutex) _logMutex = xSemaphoreCreateMutex();
    if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    int pos = snprintf(buf, size, "{\"count\":%d,\"lines\":[", _logCount);
    int start = (_logCount < LOG_RING_SIZE) ? 0 : _logHead;
    int count = _logCount;

    int limit = (int)size - 8;  // Reserve space for trailing "]}\0"
    for (int i = 0; i < count && pos < limit; i++) {
        int idx = (start + i) % LOG_RING_SIZE;
        if (i > 0 && pos < limit) buf[pos++] = ',';
        if (pos >= limit) break;
        buf[pos++] = '"';
        // JSON-escape the line
        for (int c = 0; _logRing[idx][c] && pos < limit - 2; c++) {
            char ch = _logRing[idx][c];
            if (ch == '"') { buf[pos++] = '\\'; buf[pos++] = '"'; }
            else if (ch == '\\') { buf[pos++] = '\\'; buf[pos++] = '\\'; }
            else if (ch == '\n') { buf[pos++] = '\\'; buf[pos++] = 'n'; }
            else if (ch == '\r') { /* skip */ }
            else if ((unsigned char)ch < 0x20) { buf[pos++] = '.'; }
            else { buf[pos++] = ch; }
        }
        if (pos < limit) buf[pos++] = '"';
    }
    if (pos < (int)size - 3) {
        pos += snprintf(buf + pos, size - pos, "]}");
    } else {
        // Truncated — close JSON safely
        pos = (int)size - 3;
        buf[pos++] = ']'; buf[pos++] = '}'; buf[pos] = '\0';
    }
    xSemaphoreGive(_logMutex);
    return pos;
}

// ── System Info page (/system) ───────────────────────────────────────────

static const char SYSTEM_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>System Info</title>
%THEME%
</head><body>
%NAV%
<h1>// System Info</h1>
<div class="card">
<h2>Chip</h2>
<table>
<tr><td class="label">Model</td><td>%CHIP_MODEL%</td></tr>
<tr><td class="label">Revision</td><td>%CHIP_REV%</td></tr>
<tr><td class="label">Cores</td><td>%CORES%</td></tr>
<tr><td class="label">CPU Frequency</td><td>%CPU_MHZ% MHz</td></tr>
<tr><td class="label">SDK Version</td><td>%SDK%</td></tr>
</table>
</div>
<div class="card">
<h2>Memory</h2>
<table>
<tr><td class="label">Flash Size</td><td>%FLASH_SIZE%</td></tr>
<tr><td class="label">Flash Speed</td><td>%FLASH_SPEED% MHz</td></tr>
<tr><td class="label">Heap Total</td><td>%HEAP_TOTAL%</td></tr>
<tr><td class="label">Heap Free</td><td id="heap_free">%HEAP_FREE%</td></tr>
<tr><td class="label">Heap Min Free</td><td>%HEAP_MIN%</td></tr>
<tr><td class="label">PSRAM Total</td><td>%PSRAM_TOTAL%</td></tr>
<tr><td class="label">PSRAM Free</td><td id="psram_free">%PSRAM_FREE%</td></tr>
</table>
</div>
<div class="card">
<h2>Network</h2>
<table>
<tr><td class="label">MAC Address</td><td style="font-family:monospace">%MAC%</td></tr>
<tr><td class="label">WiFi SSID</td><td>%SSID%</td></tr>
<tr><td class="label">IP Address</td><td>%IP%</td></tr>
<tr><td class="label">Gateway</td><td>%GATEWAY%</td></tr>
<tr><td class="label">DNS</td><td>%DNS%</td></tr>
<tr><td class="label">RSSI</td><td>%RSSI% dBm</td></tr>
<tr><td class="label">Channel</td><td>%CHANNEL%</td></tr>
</table>
</div>
<div class="card">
<h2>Runtime</h2>
<table>
<tr><td class="label">Uptime</td><td>%UPTIME%</td></tr>
<tr><td class="label">Web Requests</td><td>%REQCOUNT%</td></tr>
<tr><td class="label">Reset Reason</td><td>%RESET_REASON%</td></tr>
</table>
</div>
<div class="card">
<h2>Partitions</h2>
<table>
<tr><th>Label</th><th>Type</th><th>Offset</th><th>Size</th></tr>
%PARTITIONS%
</table>
</div>
<div class="card">
<h2>FreeRTOS Tasks <span id="task_count" style="color:#666;font-size:12px"></span></h2>
<table id="task_table">
<tr><th>Name</th><th>Pri</th><th>State</th><th>Stack HWM</th></tr>
</table>
</div>
<script>
function fmt(n){return n>1048576?(n/1048576).toFixed(1)+' MB':n>1024?(n/1024).toFixed(0)+' KB':n+' B'}
function stColor(s){return s==='run'?'#05ffa1':s==='rdy'?'#00f0ff':s==='blk'?'#8888aa':'#ff2a6d'}
function stkColor(v){return v<200?'#ff2a6d':v<500?'#fcee0a':'#05ffa1'}
function refreshTasks(){
  fetch('/api/diag/tasks').then(r=>r.json()).then(d=>{
    document.getElementById('task_count').textContent='('+d.count+')';
    var t=document.getElementById('task_table');
    while(t.rows.length>1)t.deleteRow(1);
    d.tasks.sort((a,b)=>b.pri-a.pri);
    d.tasks.forEach(function(tk){
      var r=t.insertRow();
      r.insertCell().textContent=tk.name;
      r.insertCell().textContent=tk.pri;
      var sc=r.insertCell();sc.textContent=tk.state.toUpperCase();sc.style.color=stColor(tk.state);
      var hc=r.insertCell();hc.textContent=tk.stack_hwm;hc.style.color=stkColor(tk.stack_hwm);
    });
  }).catch(function(){});
}
setInterval(function(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('heap_free').textContent=fmt(d.free_heap);
    document.getElementById('psram_free').textContent=fmt(d.psram_free);
  });
  refreshTasks();
},5000);
refreshTasks();
</script>
</body></html>
)rawliteral";

void WebServerHAL::addSystemPage() {
    if (!_server) return;

    WebServerHAL* self = this;

    _server->on("/system", HTTP_GET, [self]() {
        self->_requestCount++;

        String html(FPSTR(SYSTEM_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));

        // Chip info
        html.replace("%CHIP_MODEL%", ESP.getChipModel());
        html.replace("%CHIP_REV%", String(ESP.getChipRevision()));
        html.replace("%CORES%", String(ESP.getChipCores()));
        html.replace("%CPU_MHZ%", String(ESP.getCpuFreqMHz()));
        html.replace("%SDK%", ESP.getSdkVersion());

        // Memory
        char sizeBuf[32];
        snprintf(sizeBuf, sizeof(sizeBuf), "%.1f MB", ESP.getFlashChipSize() / 1048576.0f);
        html.replace("%FLASH_SIZE%", sizeBuf);
        html.replace("%FLASH_SPEED%", String(ESP.getFlashChipSpeed() / 1000000));
        snprintf(sizeBuf, sizeof(sizeBuf), "%.1f KB", ESP.getHeapSize() / 1024.0f);
        html.replace("%HEAP_TOTAL%", sizeBuf);
        snprintf(sizeBuf, sizeof(sizeBuf), "%.1f KB", ESP.getFreeHeap() / 1024.0f);
        html.replace("%HEAP_FREE%", sizeBuf);
        snprintf(sizeBuf, sizeof(sizeBuf), "%.1f KB", ESP.getMinFreeHeap() / 1024.0f);
        html.replace("%HEAP_MIN%", sizeBuf);
        snprintf(sizeBuf, sizeof(sizeBuf), "%.1f MB", ESP.getPsramSize() / 1048576.0f);
        html.replace("%PSRAM_TOTAL%", sizeBuf);
        snprintf(sizeBuf, sizeof(sizeBuf), "%.1f MB", ESP.getFreePsram() / 1048576.0f);
        html.replace("%PSRAM_FREE%", sizeBuf);

        // Network
        uint8_t mac[6];
        WiFi.macAddress(mac);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        html.replace("%MAC%", macStr);
        html.replace("%SSID%", WiFi.isConnected() ? WiFi.SSID() : "Not connected");
        html.replace("%IP%", WiFi.localIP().toString());
        html.replace("%GATEWAY%", WiFi.gatewayIP().toString());
        html.replace("%DNS%", WiFi.dnsIP().toString());
        html.replace("%RSSI%", String(WiFi.RSSI()));
        html.replace("%CHANNEL%", String(WiFi.channel()));

        // Runtime
        html.replace("%UPTIME%", uptimeString());
        html.replace("%REQCOUNT%", String(self->_requestCount));

        // Reset reason
        esp_reset_reason_t reason = esp_reset_reason();
        const char* reasonStr = "Unknown";
        switch (reason) {
            case ESP_RST_POWERON:  reasonStr = "Power-on"; break;
            case ESP_RST_SW:       reasonStr = "Software"; break;
            case ESP_RST_PANIC:    reasonStr = "Panic"; break;
            case ESP_RST_INT_WDT:  reasonStr = "Interrupt WDT"; break;
            case ESP_RST_TASK_WDT: reasonStr = "Task WDT"; break;
            case ESP_RST_WDT:      reasonStr = "Other WDT"; break;
            case ESP_RST_DEEPSLEEP:reasonStr = "Deep Sleep"; break;
            case ESP_RST_BROWNOUT: reasonStr = "Brownout"; break;
            default: break;
        }
        html.replace("%RESET_REASON%", reasonStr);

        // Partition table
        String partHtml;
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
            ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it) {
            const esp_partition_t* part = esp_partition_get(it);
            if (part) {
                char row[256];
                const char* typeStr = (part->type == ESP_PARTITION_TYPE_APP) ? "app" :
                                      (part->type == ESP_PARTITION_TYPE_DATA) ? "data" : "?";
                snprintf(row, sizeof(row),
                    "<tr><td>%s</td><td>%s</td><td>0x%06lX</td><td>%.0f KB</td></tr>",
                    part->label, typeStr,
                    (unsigned long)part->address, part->size / 1024.0f);
                partHtml += row;
            }
            it = esp_partition_next(it);
        }
        esp_partition_iterator_release(it);
        if (partHtml.length() == 0)
            partHtml = "<tr><td colspan='4' style='color:#666'>No partition info</td></tr>";
        html.replace("%PARTITIONS%", partHtml);

        _server->send(200, "text/html", html);
    });

    DBG_INFO("web", "System page added at /system");
}

// ── Logs page (/logs) ────────────────────────────────────────────────────

static const char LOGS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>System Logs</title>
%THEME%
<style>
#log-container{background:#050505;border:1px solid #00ffd022;border-radius:4px;
  padding:10px;height:60vh;overflow-y:auto;font-size:12px;line-height:1.6;
  font-family:'Courier New',monospace;color:#8a8a8a}
#log-container .line{white-space:pre-wrap;word-wrap:break-word}
.log-controls{display:flex;gap:10px;align-items:center;margin:10px 0}
.log-controls label{color:#666;font-size:12px}
.log-count{color:#666;font-size:12px}
</style>
</head><body>
%NAV%
<h1>// System Logs</h1>
<div class="card">
<div class="log-controls">
  <button onclick="setTab('live')" id="tab-live" style="background:#00ffd0;color:#0a0a0a">Live</button>
  <button onclick="setTab('sd')" id="tab-sd">SD Card</button>
  <span style="margin-left:8px">|</span>
  <button onclick="clearLog()">Clear</button>
  <button onclick="togglePause()"><span id="pause-btn">Pause</span></button>
  <label><input type="checkbox" id="autoscroll" checked> Auto-scroll</label>
  <span class="log-count" id="log-count">0 lines</span>
</div>
<div id="sd-controls" style="display:none;margin-bottom:8px">
  <select id="sd-file" style="background:#111;color:#00ffd0;border:1px solid #00ffd044;padding:4px;font-family:inherit;font-size:12px"></select>
  <button onclick="loadSdLog()">Load</button>
  <a id="sd-dl" href="#" download style="margin-left:8px;color:#00ffd0;font-size:12px">Download</a>
</div>
<div id="log-container"></div>
</div>
<script>
var paused=false,lastCount=0,activeTab='live';
function setTab(t){activeTab=t;
document.getElementById('tab-live').style.background=t==='live'?'#00ffd0':'transparent';
document.getElementById('tab-live').style.color=t==='live'?'#0a0a0a':'#00ffd0';
document.getElementById('tab-sd').style.background=t==='sd'?'#00ffd0':'transparent';
document.getElementById('tab-sd').style.color=t==='sd'?'#0a0a0a':'#00ffd0';
document.getElementById('sd-controls').style.display=t==='sd'?'block':'none';
if(t==='sd'){loadSdFileList();}else{clearLog();fetchLogs();}}
function togglePause(){
  paused=!paused;
  document.getElementById('pause-btn').textContent=paused?'Resume':'Pause';
}
function clearLog(){
  document.getElementById('log-container').innerHTML='';
  lastCount=0;
}
function fetchLogs(){
  if(paused||activeTab!=='live')return;
  fetch('/api/logs').then(r=>r.json()).then(d=>{
    var c=document.getElementById('log-container');
    if(d.count!==lastCount){
      var html='';
      d.lines.forEach(function(l){
        var cls='';
        if(l.indexOf('[E]')>=0||l.indexOf('ERROR')>=0) cls='color:#ff3366';
        else if(l.indexOf('[W]')>=0||l.indexOf('WARN')>=0) cls='color:#ffaa00';
        else if(l.indexOf('[I]')>=0||l.indexOf('INFO')>=0) cls='color:#00ffd0';
        html+='<div class="line" style="'+cls+'">'+escHtml(l)+'</div>';
      });
      c.innerHTML=html;
      lastCount=d.count;
      document.getElementById('log-count').textContent=d.lines.length+' lines';
      if(document.getElementById('autoscroll').checked){
        c.scrollTop=c.scrollHeight;
      }
    }
  }).catch(()=>{});
}
function loadSdFileList(){
fetch('/api/logs/sd/list').then(r=>r.json()).then(files=>{
var sel=document.getElementById('sd-file');sel.innerHTML='';
files.forEach(function(f){var o=document.createElement('option');
o.value=f.name;o.textContent=f.name+' ('+(f.size/1024).toFixed(1)+' KB)';sel.appendChild(o);});
if(files.length>0)loadSdLog();
}).catch(()=>{document.getElementById('log-container').innerHTML='<div class="line" style="color:#666">No SD logs available</div>';});}
function loadSdLog(){
var fname=document.getElementById('sd-file').value;if(!fname)return;
document.getElementById('sd-dl').href='/api/logs/sd?file='+encodeURIComponent(fname);
document.getElementById('log-container').innerHTML='<div class="line" style="color:#666">Loading...</div>';
fetch('/api/logs/sd').then(r=>r.text()).then(text=>{
var c=document.getElementById('log-container');var lines=text.split('\n');
var html='';lines.forEach(function(l){if(!l)return;
var cls='';if(l.indexOf('ERROR')>=0)cls='color:#ff3366';
else if(l.indexOf('WARN')>=0)cls='color:#ffaa00';
else if(l.indexOf('INFO')>=0)cls='color:#00ffd0';
else if(l.indexOf('DEBUG')>=0)cls='color:#66cccc';
html+='<div class="line" style="'+cls+'">'+escHtml(l)+'</div>';});
c.innerHTML=html;document.getElementById('log-count').textContent=lines.length+' lines';
if(document.getElementById('autoscroll').checked)c.scrollTop=c.scrollHeight;
}).catch(()=>{document.getElementById('log-container').innerHTML='<div class="line" style="color:#ff3366">Failed to load SD log</div>';});}
function escHtml(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
setInterval(fetchLogs,1500);
fetchLogs();
</script>
</body></html>
)rawliteral";

void WebServerHAL::addLogsPage() {
    if (!_server) return;

    WebServerHAL* self = this;

    _server->on("/logs", HTTP_GET, [self]() {
        self->_requestCount++;
        String html(FPSTR(LOGS_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));
        _server->send(200, "text/html", html);
    });

    // API endpoint for log data
    _server->on("/api/logs", HTTP_GET, [self]() {
        self->_requestCount++;
        // Allocate from PSRAM if available for large log buffer
        static char* logBuf = nullptr;
        static const size_t LOG_BUF_SIZE = 48 * 1024;
        if (!logBuf) {
            logBuf = (char*)ps_malloc(LOG_BUF_SIZE);
            if (!logBuf) logBuf = (char*)malloc(LOG_BUF_SIZE);
        }
        if (logBuf) {
            int len = WebServerHAL::getLogJson(logBuf, LOG_BUF_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", logBuf);
                return;
            }
        }
        _server->send(200, "application/json", "{\"count\":0,\"lines\":[]}");
    });

    // GET /api/logs/sd — download SD card system log
    _server->on("/api/logs/sd", HTTP_GET, [self]() {
        self->_requestCount++;
        struct stat st;
        if (stat("/sdcard/logs/system.log", &st) != 0) {
            _server->send(404, "application/json",
                "{\"error\":\"No SD log file\"}");
            return;
        }
        FILE* f = fopen("/sdcard/logs/system.log", "r");
        if (!f) {
            _server->send(500, "application/json",
                "{\"error\":\"Cannot open log\"}");
            return;
        }

        char sizeStr[16];
        snprintf(sizeStr, sizeof(sizeStr), "%ld", (long)st.st_size);
        _server->sendHeader("Content-Disposition",
            "attachment; filename=\"system.log\"");
        _server->setContentLength(st.st_size);
        _server->send(200, "text/plain", "");

        char chunk[1024];
        size_t total = 0;
        while (total < (size_t)st.st_size) {
            size_t n = fread(chunk, 1, sizeof(chunk), f);
            if (n == 0) break;
            _server->sendContent(chunk, n);
            total += n;
        }
        fclose(f);
    });

    // GET /api/logs/sd/list — list available log files
    _server->on("/api/logs/sd/list", HTTP_GET, [self]() {
        self->_requestCount++;
        char* buf = api_buf();
        int pos = snprintf(buf, API_BUF_SIZE, "[");

        const char* names[] = {
            "/sdcard/logs/system.log",
            "/sdcard/logs/system.log.1",
            "/sdcard/logs/system.log.2",
            "/sdcard/logs/system.log.3",
            "/sdcard/logs/system.log.4",
            "/sdcard/logs/system.log.5",
        };
        bool first = true;
        for (int i = 0; i < 6; i++) {
            struct stat st;
            if (stat(names[i], &st) == 0) {
                if (!first) buf[pos++] = ',';
                first = false;
                pos += snprintf(buf + pos, API_BUF_SIZE - pos,
                    "{\"name\":\"%s\",\"size\":%ld}",
                    names[i] + 14,  // strip "/sdcard/logs/"
                    (long)st.st_size);
            }
        }
        snprintf(buf + pos, API_BUF_SIZE - pos, "]");
        _server->send(200, "application/json", buf);
    });

    DBG_INFO("web", "Logs page added at /logs");
}

// ── Terminal page (/terminal) ─────────────────────────────────────────────

static const char TERMINAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Terminal — Tritium Edge</title>
%THEME%
<style>
#term{background:#050505;border:1px solid #00ffd022;border-radius:4px;
  padding:10px;height:55vh;overflow-y:auto;font-size:13px;line-height:1.5;
  font-family:'JetBrains Mono','Fira Code','Cascadia Code','Courier New',monospace;color:#05ffa1}
#term .line{white-space:pre-wrap;word-wrap:break-word}
.cmd-row{display:flex;gap:8px;margin-top:8px;align-items:center}
#cmd-input{flex:1;background:#111;color:#00ffd0;border:1px solid #00ffd044;
  padding:8px 12px;font-family:inherit;font-size:14px;border-radius:4px;outline:none}
#cmd-input:focus{border-color:#00ffd0;box-shadow:0 0 8px #00ffd022}
.quick-bar{display:flex;gap:6px;margin:8px 0;flex-wrap:wrap}
.quick-bar button{background:#111;color:#00ffd0;border:1px solid #00ffd044;
  padding:4px 10px;border-radius:3px;font-size:11px;cursor:pointer;
  font-family:inherit;transition:all .15s}
.quick-bar button:hover{background:#00ffd0;color:#0a0a0a}
</style>
</head><body>
%NAV%
<h1>// Terminal</h1>
<div class="card">
<div class="quick-bar">
  <button onclick="send('IDENTIFY')">IDENTIFY</button>
  <button onclick="send('SERVICES')">SERVICES</button>
  <button onclick="send('HELP')">HELP</button>
  <button onclick="clearTerm()" style="border-color:#ff336644;color:#ff3366">Clear</button>
  <label style="margin-left:auto;color:#666;font-size:11px">
    <input type="checkbox" id="autoscroll" checked> Auto-scroll</label>
</div>
<div id="term"></div>
<div class="cmd-row">
  <span style="color:#00ffd0;font-size:16px;font-weight:bold">&gt;</span>
  <input id="cmd-input" type="text" placeholder="Type command..." autocomplete="off"
    onkeydown="if(event.key==='Enter'){sendInput();event.preventDefault();}">
  <button onclick="sendInput()" style="background:#00ffd0;color:#0a0a0a;border:none;
    padding:8px 16px;border-radius:4px;cursor:pointer;font-weight:bold">Send</button>
</div>
</div>
<script>
var lastCount=0;
function send(cmd){
  fetch('/api/terminal/cmd',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({cmd:cmd})}).then(function(){fetchLogs();});
}
function sendInput(){
  var inp=document.getElementById('cmd-input');
  var cmd=inp.value.trim();
  if(!cmd)return;
  send(cmd);
  inp.value='';
  inp.focus();
}
function clearTerm(){document.getElementById('term').innerHTML='';lastCount=0;}
function fetchLogs(){
  fetch('/api/terminal/output').then(function(r){return r.json()}).then(function(d){
    if(d.count!==lastCount){
      var t=document.getElementById('term');var html='';
      d.lines.forEach(function(l){
        var cls='color:#05ffa1';
        if(l.indexOf('[E]')>=0||l.indexOf('ERROR')>=0) cls='color:#ff3366';
        else if(l.indexOf('[W]')>=0||l.indexOf('WARN')>=0) cls='color:#ffaa00';
        else if(l.indexOf('[I]')>=0||l.indexOf('INFO')>=0) cls='color:#00ffd0';
        else if(l.indexOf('> ')===0) cls='color:#fcee0a';
        html+='<div class="line" style="'+cls+'">'+escHtml(l)+'</div>';
      });
      t.innerHTML=html;lastCount=d.count;
      if(document.getElementById('autoscroll').checked)t.scrollTop=t.scrollHeight;
    }
  }).catch(function(){});
}
function escHtml(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
setInterval(fetchLogs,1000);
fetchLogs();
document.getElementById('cmd-input').focus();
</script>
</body></html>
)rawliteral";

void WebServerHAL::addTerminalPage() {
    if (!_server) return;

    WebServerHAL* self = this;

    _server->on("/terminal", HTTP_GET, [self]() {
        self->_requestCount++;
        String html(FPSTR(TERMINAL_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));
        _server->send(200, "text/html", html);
    });

    // GET /api/terminal/output — get log lines for terminal display
    _server->on("/api/terminal/output", HTTP_GET, [self]() {
        self->_requestCount++;
        char* buf = api_buf();
        // Use the same log ring as /api/logs for terminal output
        int len = WebServerHAL::getLogJson(buf, API_BUF_SIZE);
        if (len > 0) {
            _server->send(200, "application/json", buf);
        } else {
            _server->send(200, "application/json", "{\"count\":0,\"lines\":[]}");
        }
    });

    // POST /api/terminal/cmd — execute a command and return output
    _server->on("/api/terminal/cmd", HTTP_POST, [self]() {
        self->_requestCount++;
        if (!_server->hasArg("plain")) {
            _server->send(400, "application/json", "{\"error\":\"Missing body\"}");
            return;
        }
        String body = _server->arg("plain");
        // Parse JSON: {"cmd":"IDENTIFY"}
        int start = body.indexOf("\"cmd\"");
        if (start < 0) {
            _server->send(400, "application/json", "{\"error\":\"Missing cmd field\"}");
            return;
        }
        start = body.indexOf(':', start);
        if (start < 0) { _server->send(400, "application/json", "{\"error\":\"Bad JSON\"}"); return; }
        start = body.indexOf('"', start + 1);
        if (start < 0) { _server->send(400, "application/json", "{\"error\":\"Bad JSON\"}"); return; }
        int end = body.indexOf('"', start + 1);
        if (end <= start) { _server->send(400, "application/json", "{\"error\":\"Bad JSON\"}"); return; }

        String cmd = body.substring(start + 1, end);
        char cmd_buf[256];
        strncpy(cmd_buf, cmd.c_str(), sizeof(cmd_buf) - 1);
        cmd_buf[sizeof(cmd_buf) - 1] = '\0';

        // Log the command echo
        char echo[300];
        snprintf(echo, sizeof(echo), "> %s", cmd_buf);
        WebServerHAL::captureLog(echo);

        // Split into verb + args
        char* space = strchr(cmd_buf, ' ');
        const char* verb = cmd_buf;
        const char* args = nullptr;
        if (space) { *space = '\0'; args = space + 1; }

        char resp[512];
        bool handled = false;

        if (strcmp(verb, "IDENTIFY") == 0) {
#if __has_include("display.h") && __has_include("service_registry.h")
            snprintf(resp, sizeof(resp),
                     "{\"board\":\"esp32-s3\",\"display\":\"%dx%d\",\"services\":%d}",
                     display_get_width(), display_get_height(),
                     ServiceRegistry::count());
            WebServerHAL::captureLog(resp);
#endif
            handled = true;
        } else if (strcmp(verb, "SERVICES") == 0) {
#if __has_include("service_registry.h")
            char line[128];
            snprintf(line, sizeof(line), "[svc] %d services:", ServiceRegistry::count());
            WebServerHAL::captureLog(line);
            for (int i = 0; i < ServiceRegistry::count(); i++) {
                auto* s = ServiceRegistry::at(i);
                if (s) {
                    snprintf(line, sizeof(line), "  %-20s pri=%3d cap=%02X",
                             s->name(), s->initPriority(), s->capabilities());
                    WebServerHAL::captureLog(line);
                }
            }
#endif
            handled = true;
        } else if (strcmp(verb, "HELP") == 0) {
            WebServerHAL::captureLog("Commands: IDENTIFY, SERVICES, HELP");
            WebServerHAL::captureLog("Service commands are dispatched to registered services.");
            handled = true;
        }

        if (!handled) {
#if __has_include("service_registry.h")
            if (!ServiceRegistry::dispatchCommand(verb, args)) {
                snprintf(resp, sizeof(resp), "[cmd] Unknown: %s", verb);
                WebServerHAL::captureLog(resp);
            }
#else
            snprintf(resp, sizeof(resp), "[cmd] Unknown: %s", verb);
            WebServerHAL::captureLog(resp);
#endif
        }

        _server->send(200, "application/json", "{\"ok\":true}");
    });

    DBG_INFO("web", "Terminal page added at /terminal");
}

// ── Error pages (404, 500) ───────────────────────────────────────────────

static const char ERROR_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>%ERROR_CODE% - %ERROR_TITLE%</title>
%THEME%
<style>
.error-code{font-size:72px;color:#ff3366;font-weight:bold;text-align:center;
  margin:40px 0 10px;text-shadow:0 0 20px #ff336644}
.error-msg{text-align:center;color:#666;font-size:14px;margin-bottom:30px}
.error-uri{color:#ff3366;font-family:monospace}
</style>
</head><body>
%NAV%
<div class="error-code">%ERROR_CODE%</div>
<div class="error-msg">%ERROR_MSG%</div>
<div style="text-align:center"><a href="/">Back to Dashboard</a></div>
</body></html>
)rawliteral";

// ── Map page (/map) — Leaflet.js slippy map with local tile serving ──────

static const char MAP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Map — Tritium Edge</title>
%THEME%
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"
  integrity="sha256-p4NxAoJBhIIN+hmNHrzRCf9tD/miZyoHS5obTRR9BMY="
  crossorigin=""/>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"
  integrity="sha256-20nQCchB9co0qIjJZRGuk2/Z9VM+kNiyxNV1lvTlZBo="
  crossorigin=""></script>
<style>
#map{width:100%;height:calc(100vh - 120px);border:1px solid #00ffd044;
  border-radius:6px;margin-top:8px;background:#0a0a0a}
.leaflet-control-layers{background:#111!important;color:#c0c0c0!important;
  border:1px solid #00ffd044!important;border-radius:4px!important}
.leaflet-control-layers label{color:#c0c0c0!important}
.leaflet-control-layers-selector{accent-color:#00ffd0}
.leaflet-control-zoom a{background:#111!important;color:#00ffd0!important;
  border-color:#00ffd044!important}
.leaflet-control-zoom a:hover{background:#1a1a1a!important;color:#66ffe8!important}
.leaflet-bar{border:1px solid #00ffd044!important}
.leaflet-tile-pane{filter:brightness(0.85) contrast(1.1)}
#no-data{display:none;text-align:center;padding:60px 20px;color:#666}
#no-data h2{color:#ff3366;margin-bottom:12px}
.leaflet-control-attribution{background:rgba(10,10,10,0.8)!important;
  color:#666!important}
.leaflet-control-attribution a{color:#00ffd0!important}
</style>
</head><body>
%NAV%
<h1>Offline Map</h1>
<div id="no-data">
  <h2>No Tile Data Available</h2>
  <p>Insert an SD card with GIS tiles in OSM slippy-map format.</p>
  <p>Expected path: <code>/sdcard/gis/{layer}/{z}/{x}/{y}.png</code></p>
</div>
<div id="map"></div>
<script>
(function(){
  var map = L.map('map',{
    center:[39.8283,-98.5795], zoom:4,
    zoomControl:true, attributionControl:true
  });
  var mapDiv = document.getElementById('map');
  var noData = document.getElementById('no-data');

  fetch('/api/gis/layers').then(function(r){return r.json()}).then(function(layers){
    if(!layers||layers.length===0){
      mapDiv.style.display='none';
      noData.style.display='block';
      return;
    }
    var baseLayers={};
    var first=true;
    layers.forEach(function(ly){
      var tileUrl='/api/gis/tiles/'+ly.name+'/{z}/{x}/{y}.png';
      var layer=L.tileLayer(tileUrl,{
        minZoom:ly.zoom_min||0, maxZoom:ly.zoom_max||18,
        attribution:'Tritium GIS &mdash; '+ly.name,
        errorTileUrl:''
      });
      baseLayers[ly.name]=layer;
      if(first){
        layer.addTo(map);
        if(ly.bounds){
          var b=ly.bounds;
          map.fitBounds([[b.lat_min,b.lon_min],[b.lat_max,b.lon_max]]);
        }
        first=false;
      }
    });
    if(layers.length>1){
      L.control.layers(baseLayers,null,{position:'topright',collapsed:false}).addTo(map);
    }
  }).catch(function(){
    mapDiv.style.display='none';
    noData.style.display='block';
  });
})();
</script>
</body></html>
)rawliteral";

void WebServerHAL::addMapPage() {
    if (!_server) return;

    WebServerHAL* self = this;

    // GET /map — Leaflet.js slippy map page
    _server->on("/map", HTTP_GET, [self]() {
        self->_requestCount++;

        String html(FPSTR(MAP_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));
        _server->send(200, "text/html", html);
    });

    // GET /api/gis/layers — JSON array of available layers
    _server->on("/api/gis/layers", HTTP_GET, [self]() {
        self->_requestCount++;

        if (self->_gisLayerProvider) {
            char* buf = api_buf();
            int len = self->_gisLayerProvider(buf, API_BUF_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", buf);
                return;
            }
        }
        _server->send(200, "application/json", "[]");
    });

    // Note: Tile requests at /api/gis/tiles/{layer}/{z}/{x}/{y}.png are
    // handled in addErrorPages() onNotFound handler, since WebServer.h
    // does not support path parameter matching.

    DBG_INFO("web", "Map page added at /map");
}

// ── Error pages (/404, /500) ──────────────────────────────────────────────

void WebServerHAL::addErrorPages() {
    if (!_server) return;

    WebServerHAL* self = this;

    _server->onNotFound([self]() {
        self->_requestCount++;

        // Handle CORS preflight requests for cross-origin fleet panel access
        if (_server->method() == HTTP_OPTIONS) {
            _server->sendHeader("Access-Control-Allow-Origin", "*");
            _server->sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            _server->sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
            _server->sendHeader("Access-Control-Max-Age", "86400");
            _server->send(204);
            return;
        }

        // Serve GIS tile requests: /api/gis/tiles/{layer}/{z}/{x}/{y}.png
        String uri = _server->uri();
        if (uri.startsWith("/api/gis/tiles/") && self->_gisTileProvider) {
            const char* path = uri.c_str() + 15; // skip "/api/gis/tiles/"

            // Parse layer name (up to next '/')
            const char* slash1 = strchr(path, '/');
            if (slash1) {
                char layer[32] = {0};
                size_t layerLen = slash1 - path;
                if (layerLen >= sizeof(layer)) layerLen = sizeof(layer) - 1;
                memcpy(layer, path, layerLen);

                // Parse z/x/y.png
                int z = 0, x = 0, y = 0;
                if (sscanf(slash1, "/%d/%d/%d.png", &z, &x, &y) == 3) {
                    size_t tileLen = 0;
                    uint8_t* tileData = self->_gisTileProvider(
                        layer, (uint8_t)z, (uint32_t)x, (uint32_t)y, tileLen);

                    if (tileData && tileLen > 0) {
                        _server->sendHeader("Cache-Control",
                                            "public, max-age=86400");
                        _server->send_P(200, "image/png",
                                        (const char*)tileData, tileLen);
                        free(tileData);
                        return;
                    }
                    _server->send(404, "text/plain", "Tile not found");
                    return;
                }
            }
            _server->send(400, "text/plain", "Invalid tile path");
            return;
        }

        // If captive portal is active, redirect instead of 404
        if (_dnsServer) {
            _server->sendHeader("Location", "http://192.168.4.1/wifi", true);
            _server->send(302, "text/plain", "Redirecting to setup...");
            return;
        }

        String html(FPSTR(ERROR_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));
        html.replace("%ERROR_CODE%", "404");
        html.replace("%ERROR_TITLE%", "Not Found");
        String msg = "The path <span class=\"error-uri\">";
        msg += _server->uri();
        msg += "</span> does not exist on this node.";
        html.replace("%ERROR_MSG%", msg);
        _server->send(404, "text/html", html);
    });

    DBG_INFO("web", "Error pages registered (404/500)");
}

// ── Remote Viewer page (/remote) ──────────────────────────────────────────

static const char REMOTE_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tritium-OS // Remote Control</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--cyan:#00f0ff;--mag:#ff2a6d;--green:#05ffa1;--void:#0a0a0f;--s2:#12121a;--s3:#1a1a2e;--ghost:#8888aa;--text:#c8d0dc}
body{background:var(--void);color:var(--text);font-family:'Courier New',monospace;
font-size:13px;display:flex;flex-direction:column;align-items:center;padding:12px;gap:8px}
h1{color:var(--cyan);font-size:14px;letter-spacing:0.12em;text-transform:uppercase}
.row{display:flex;gap:6px;align-items:center;flex-wrap:wrap;justify-content:center}
.btn{padding:4px 10px;border:1px solid rgba(0,240,255,0.3);background:transparent;
color:var(--cyan);font-family:inherit;font-size:11px;cursor:pointer;border-radius:3px;
transition:background 0.15s}
.btn:hover{background:rgba(0,240,255,0.1)}
.btn.active{background:rgba(0,240,255,0.15)}
.btn.app{border-color:rgba(0,240,255,0.15);color:var(--text);font-size:10px}
.btn.app:hover{border-color:var(--cyan);color:var(--cyan)}
.btn.home{border-color:var(--green);color:var(--green)}
.info{font-size:10px;color:var(--ghost)}
#viewer{border:1px solid rgba(0,240,255,0.15);border-radius:4px;cursor:crosshair;
image-rendering:pixelated;max-width:100%;touch-action:none}
.stats{display:flex;gap:12px;font-size:10px;color:var(--ghost)}
.stats .val{color:var(--cyan)}
.status .on{color:var(--green)}.status .off{color:var(--mag)}
</style></head><body>
<h1>// Remote Control</h1>
<div class="row">
<button class="btn active" id="btn-live" onclick="toggleLive()">Live</button>
<button class="btn" onclick="capture()">Snap</button>
<label class="info"><input type="checkbox" id="touch-enable" checked> Touch</label>
<button class="btn home" onclick="goHome()">Home</button>
<span id="app-btns"></span>
<span class="info" id="fps-info">--</span>
</div>
<canvas id="viewer" width="800" height="480"></canvas>
<div class="stats">
<span id="status" class="status">Connecting...</span>
<span id="res-info">--</span>
<span>Heap: <span class="val" id="v-heap">--</span></span>
<span>RSSI: <span class="val" id="v-rssi">--</span></span>
<span>Up: <span class="val" id="v-uptime">--</span></span>
</div>
<script>
(function(){
var canvas=document.getElementById('viewer');
var ctx=canvas.getContext('2d');
var W=0,H=0,live=true,busy=false,frames=0,lastFps=Date.now();
var isRgb=false;
var dragStart=null;

function fetchInfo(){
fetch('/api/remote/info').then(function(r){return r.json()}).then(function(d){
W=d.width;H=d.height;isRgb=d.rgb||false;
canvas.width=W;canvas.height=H;
document.getElementById('res-info').textContent=W+'x'+H;
document.getElementById('status').innerHTML='<span class="on">Connected</span>';
capture();loadApps();updateStatus();
}).catch(function(){document.getElementById('status').innerHTML='<span class="off">Offline</span>';});}

function loadApps(){
fetch('/api/shell/apps').then(function(r){return r.json()}).then(function(d){
var html='';
d.apps.forEach(function(a){
html+='<button class="btn app" onclick="launchApp('+a.index+')">'+a.name+'</button>';
});
document.getElementById('app-btns').innerHTML=html;
}).catch(function(){});}

window.launchApp=function(i){
fetch('/api/shell/launch',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({index:i})});};

window.goHome=function(){
fetch('/api/shell/home',{method:'POST'});};

function updateStatus(){
fetch('/api/status').then(function(r){return r.json()}).then(function(d){
document.getElementById('v-heap').textContent=Math.round(d.free_heap/1024)+'KB';
document.getElementById('v-rssi').textContent=d.rssi+'dBm';
var s=d.uptime_s;var h=Math.floor(s/3600);var m=Math.floor((s%3600)/60);
document.getElementById('v-uptime').textContent=h+'h'+m+'m';
}).catch(function(){});}

function capture(){
if(busy)return;busy=true;
fetch('/api/remote/screenshot').then(function(r){
return r.arrayBuffer();
}).then(function(ab){
var buf=new Uint16Array(ab);
if(buf.length!==W*H){busy=false;return;}
var img=ctx.createImageData(W,H);
var d=img.data;
for(var i=0;i<buf.length;i++){
var px=buf[i];
var r5,g6,b5;
if(isRgb){r5=(px>>11)&0x1F;g6=(px>>5)&0x3F;b5=px&0x1F;}
else{b5=(px>>11)&0x1F;g6=(px>>5)&0x3F;r5=px&0x1F;}
d[i*4]=(r5*255/31)|0;d[i*4+1]=(g6*255/63)|0;d[i*4+2]=(b5*255/31)|0;d[i*4+3]=255;
}
ctx.putImageData(img,0,0);
frames++;busy=false;
var now=Date.now();if(now-lastFps>=2000){
document.getElementById('fps-info').textContent=(frames*1000/(now-lastFps)).toFixed(1)+' fps';
frames=0;lastFps=now;}
}).catch(function(){busy=false;});}

window.toggleLive=function(){live=!live;
document.getElementById('btn-live').className=live?'btn active':'btn';};

setInterval(function(){if(live)capture();},200);
setInterval(updateStatus,5000);

function toDevice(e){
var r=canvas.getBoundingClientRect();
var sx=W/r.width,sy=H/r.height;
var cx=e.clientX!==undefined?e.clientX:e.touches[0].clientX;
var cy=e.clientY!==undefined?e.clientY:e.touches[0].clientY;
return{x:Math.round((cx-r.left)*sx),y:Math.round((cy-r.top)*sy)};}

canvas.addEventListener('pointerdown',function(e){
if(!document.getElementById('touch-enable').checked)return;
e.preventDefault();canvas.setPointerCapture(e.pointerId);
dragStart=toDevice(e);
fetch('/api/remote/touch',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({x:dragStart.x,y:dragStart.y,pressed:true})});
});

canvas.addEventListener('pointerup',function(e){
if(!document.getElementById('touch-enable').checked||!dragStart)return;
e.preventDefault();
var end=toDevice(e);
var dx=end.x-dragStart.x,dy=end.y-dragStart.y;
var dist=Math.sqrt(dx*dx+dy*dy);
if(dist>20){
fetch('/api/remote/swipe',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({x1:dragStart.x,y1:dragStart.y,x2:end.x,y2:end.y,steps:Math.min(Math.round(dist/10),30)})});
}else{
fetch('/api/remote/tap',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({x:dragStart.x,y:dragStart.y})});}
dragStart=null;
});

canvas.addEventListener('pointercancel',function(){dragStart=null;});

fetchInfo();
})();
</script></body></html>)rawliteral";

// ── addAllPages() ────────────────────────────────────────────────────────

void WebServerHAL::addAllPages() {
    addDashboard();
    addOtaPage();
    addConfigEditor();
    addFileManager();
    addApiEndpoints();
    addWiFiSetup();
    addBleViewer();
    addCommissionPage();
    addSystemPage();
    addLogsPage();
    addTerminalPage();
    addMapPage();

    // SD Card Storage page
    if (_server) {
        WebServerHAL* self = this;
        _server->on("/storage", HTTP_GET, [self]() {
            self->_requestCount++;
            _server->send(200, "text/html", FPSTR(STORAGE_HTML));
        });
        DBG_INFO("web", "SD storage page added at /storage");
    }

    // Remote Viewer page
    if (_server) {
        WebServerHAL* self = this;
        _server->on("/remote", HTTP_GET, [self]() {
            self->_requestCount++;
            _server->send(200, "text/html", FPSTR(REMOTE_HTML));
        });
        DBG_INFO("web", "Remote viewer added at /remote");
    }

    addErrorPages();       // Must be last — registers onNotFound handler
    DBG_INFO("web", "All pages registered");
}

// ── runTest() ───────────────────────────────────────────────────────────────

WebServerHAL::TestResult WebServerHAL::runTest() {
    TestResult result = {};
    uint32_t start = millis();

    // Test init
    bool wasRunning = _running;
    uint16_t testPort = 8181;
    if (!wasRunning) {
        result.init_ok = init(testPort);
    } else {
        result.init_ok = true;
        testPort = _port;
    }
    result.port = testPort;
    result.ip = _ip;

    // Test mDNS
    result.mdns_ok = startMDNS("esp32-test");

    // Test dashboard route registration
    if (_server && _running) {
        addDashboard();
        result.dashboard_ok = true;  // No crash = routes registered OK
    }

    // Test API route registration
    if (_server && _running) {
        addApiEndpoints();
        result.api_ok = true;  // No crash = routes registered OK
    }

    // Clean up if we started the server
    if (!wasRunning) {
        stop();
    }

    result.test_duration_ms = millis() - start;
    DBG_INFO("web", "Test complete: init=%d mdns=%d dash=%d api=%d  (%lu ms)",
             result.init_ok, result.mdns_ok, result.dashboard_ok, result.api_ok,
             (unsigned long)result.test_duration_ms);
    return result;
}

#endif // SIMULATOR
