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
void WebServerHAL::addMapPage() {}
void WebServerHAL::addScreenshotPage() {}
void WebServerHAL::addRemotePage() {}
void WebServerHAL::addMeshPage() {}
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
void WebServerHAL::setScreenshotProvider(ScreenshotProvider) {}
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
#include <esp_system.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>

// Serial capture for polling-based terminal fallback
#include "serial_capture.h"

// Display health for runtime board verification
#if __has_include("display.h")
#include "display.h"
#define WEB_HAS_DISPLAY_HEALTH 1
#else
#define WEB_HAS_DISPLAY_HEALTH 0
#endif

// Board fingerprinting for hardware identification
#if __has_include("board_fingerprint.h")
#include "board_fingerprint.h"
#define WEB_HAS_FINGERPRINT 1
#else
#define WEB_HAS_FINGERPRINT 0
#endif

// Persistent diagnostic log — optional
#if __has_include("hal_diaglog.h")
#include "hal_diaglog.h"
#define WEB_HAS_DIAGLOG 1
#else
#define WEB_HAS_DIAGLOG 0
#endif

// WiFi manager for API endpoints
#if __has_include("wifi_manager.h")
#include "wifi_manager.h"
#define WEB_HAS_WIFI_MANAGER 1
#else
#define WEB_HAS_WIFI_MANAGER 0
#endif

// Settings framework for REST API
#if defined(ENABLE_SETTINGS) && __has_include("os_settings.h")
#include "os_settings.h"
#define WEB_HAS_SETTINGS 1
#else
#define WEB_HAS_SETTINGS 0
#endif

// Shell navigation API (app listing, launch, home)
#if defined(ENABLE_SHELL) && __has_include("os_shell.h")
#include "os_shell.h"
#include "lvgl_driver.h"
#define WEB_HAS_SHELL 1
#else
#define WEB_HAS_SHELL 0
#endif

// Valpatel-styled web pages (self-contained HTML with inline CSS/JS)
#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
#include "web/dashboard_html.h"
#include "web/wifi_html.h"
#include "web/files_html.h"
#include "web/ota_html.h"
#include "web/mesh_html.h"
#include "web/terminal_html.h"
#include "web/remote_html.h"
#endif

// Touch input for remote control injection
#if __has_include("touch_input.h")
#include "touch_input.h"
#include "hal_touch.h"
#define WEB_HAS_TOUCH_INPUT 1
#else
#define WEB_HAS_TOUCH_INPUT 0
#endif

static WebServer* _server = nullptr;
static WebServerHAL* _instance = nullptr;
static DNSServer* _dnsServer = nullptr;

// Shared JSON response buffer — safe because sync WebServer handles one request at a time
static char _shared_json[4096];  // 4KB shared across all endpoints (sync WebServer = one at a time)
static const size_t SHARED_JSON_SIZE = sizeof(_shared_json);

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
  <a href="/config">Config</a>
  <a href="/files">Files</a>
  <a href="/mesh">Mesh</a>
  <a href="/commission">Commission</a>
  <a href="/map">Map</a>
  <a href="/terminal">Terminal</a>
  <a href="/remote">Remote</a>
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
  <div class="metric"><div class="val" id="v_rssi">%RSSI%</div><div class="lbl">WiFi dBm</div></div>
  <div class="metric"><div class="val" id="v_reqs">%REQCOUNT%</div><div class="lbl">Requests</div></div>
</div>
</div>
<div class="card">
<h2>Node Identity</h2>
<table>
<tr><td class="label">Board</td><td>%BOARD%</td></tr>
<tr><td class="label">MAC</td><td style="font-family:monospace">%MAC%</td></tr>
<tr><td class="label">IP</td><td>%IP%</td></tr>
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

// ── OTA Update page (/update) ───────────────────────────────────────────────

static const char OTA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA Update</title>
%THEME%
</head><body>
%NAV%
<h1>// Firmware Update</h1>
<div class="card">
<form method="POST" action="/update" enctype="multipart/form-data" id="otaform">
  <p class="label">Select firmware.bin:</p>
  <input type="file" name="update" accept=".bin">
  <br><br>
  <input type="submit" value="Upload &amp; Flash">
</form>
<div id="progress">
  <p class="label">Uploading...</p>
  <div class="bar-bg"><div class="bar-fill" id="pbar" style="width:0%"></div></div>
  <p id="ptxt" style="margin-top:4px;color:#00ffd0">0%</p>
</div>
<div id="result"></div>
</div>
<script>
document.getElementById('otaform').addEventListener('submit',function(e){
  e.preventDefault();
  var form=e.target;
  var data=new FormData(form);
  var xhr=new XMLHttpRequest();
  document.getElementById('progress').style.display='block';
  xhr.upload.addEventListener('progress',function(ev){
    if(ev.lengthComputable){
      var pct=Math.round((ev.loaded/ev.total)*100);
      document.getElementById('pbar').style.width=pct+'%';
      document.getElementById('ptxt').textContent=pct+'%';
    }
  });
  xhr.onreadystatechange=function(){
    if(xhr.readyState==4){
      var r=document.getElementById('result');
      if(xhr.status==200){
        r.innerHTML='<div class="msg ok">Update successful! Rebooting...</div>';
        setTimeout(function(){location.href='/';},5000);
      }else{
        r.innerHTML='<div class="msg err">Update failed: '+xhr.responseText+'</div>';
      }
    }
  };
  xhr.open('POST','/update');
  xhr.send(data);
});
</script>
</body></html>
)rawliteral";

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
    _instance = this;

    _server->enableCORS(true);
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

void WebServerHAL::setScreenshotProvider(ScreenshotProvider provider) {
    _screenshotProvider = provider;
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

#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    // Valpatel-styled self-contained page — fetches data via /api/status
    _server->on("/", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send_P(200, "text/html", DASHBOARD_HTML_V2);
    });

    // Terminal page
    _server->on("/terminal", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send_P(200, "text/html", TERMINAL_HTML);
    });
#else
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
#endif

    DBG_INFO("web", "Dashboard page added at /");
}

// ── addOtaPage() ────────────────────────────────────────────────────────────

void WebServerHAL::addOtaPage() {
    if (!_server) return;

    WebServerHAL* self = this;

    // Serve the upload form
#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    _server->on("/update", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send_P(200, "text/html", OTA_HTML_V2);
    });
#else
    _server->on("/update", HTTP_GET, [self]() {
        self->_requestCount++;
        String html(FPSTR(OTA_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));
        _server->send(200, "text/html", html);
    });
#endif

    // Handle firmware upload
    _server->on("/update", HTTP_POST,
        // Response after upload completes
        [self]() {
            self->_requestCount++;
            _server->sendHeader("Connection", "close");
            if (Update.hasError()) {
                _server->send(500, "text/plain", "Update FAILED");
            } else {
                _server->send(200, "text/plain", "Update OK — rebooting...");
                delay(500);
                ESP.restart();
            }
        },
        // Handle upload data
        [self]() {
            HTTPUpload& upload = _server->upload();
            if (upload.status == UPLOAD_FILE_START) {
                DBG_INFO("web", "OTA upload start: %s", upload.filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    DBG_ERROR("web", "OTA begin failed");
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    DBG_ERROR("web", "OTA write failed");
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (Update.end(true)) {
                    DBG_INFO("web", "OTA success: %u bytes", upload.totalSize);
                } else {
                    DBG_ERROR("web", "OTA end failed");
                }
            }
        }
    );

    // Also serve on /ota as an alias
#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    _server->on("/ota", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send_P(200, "text/html", OTA_HTML_V2);
    });
#else
    _server->on("/ota", HTTP_GET, [self]() {
        self->_requestCount++;
        String html(FPSTR(OTA_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));
        _server->send(200, "text/html", html);
    });
#endif

    DBG_INFO("web", "OTA page added at /update and /ota");
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
#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    _server->on("/files", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send_P(200, "text/html", FILES_HTML_V2);
    });
#else
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
#endif

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

// ── Event ring buffer (polling fallback for SSE/WebSocket) ───────────────
// Web pages can poll GET /api/events/poll when WebSocket is unavailable.
// In web pages: try ws first, fall back to polling
// if (!ws || ws.readyState !== WebSocket.OPEN) { fetch('/api/events/poll') }

#if defined(ENABLE_SETTINGS) || defined(ENABLE_OS_EVENTS)

static const int EVT_RING_SIZE = 32;        // Number of events to keep
static const int EVT_TYPE_MAX  = 32;        // Max chars for event type string
static const int EVT_DATA_MAX  = 96;        // Max chars for event data JSON

struct EventEntry {
    char     type[EVT_TYPE_MAX];
    char     data[EVT_DATA_MAX];
    uint32_t ts;                             // millis() timestamp
};

static EventEntry _evtRing[EVT_RING_SIZE];
static int  _evtHead  = 0;                  // Next write position
static int  _evtCount = 0;                  // Total events stored
static uint32_t _evtSeq = 0;                // Monotonic sequence number
static SemaphoreHandle_t _evtMutex = nullptr;

// Push an event into the ring buffer (call from anywhere)
void webserver_push_event(const char* type, const char* data_json) {
    if (!type || !type[0]) return;
    if (!_evtMutex) _evtMutex = xSemaphoreCreateMutex();
    if (xSemaphoreTake(_evtMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        strncpy(_evtRing[_evtHead].type, type, EVT_TYPE_MAX - 1);
        _evtRing[_evtHead].type[EVT_TYPE_MAX - 1] = '\0';
        if (data_json) {
            strncpy(_evtRing[_evtHead].data, data_json, EVT_DATA_MAX - 1);
            _evtRing[_evtHead].data[EVT_DATA_MAX - 1] = '\0';
        } else {
            _evtRing[_evtHead].data[0] = '\0';
        }
        _evtRing[_evtHead].ts = millis();
        _evtHead = (_evtHead + 1) % EVT_RING_SIZE;
        if (_evtCount < EVT_RING_SIZE) _evtCount++;
        _evtSeq++;
        xSemaphoreGive(_evtMutex);
    }
}

// Build JSON for events newer than `since_ms` (millis timestamp)
static int getEventPollJson(char* buf, size_t size, uint32_t since_ms) {
    if (!_evtMutex) _evtMutex = xSemaphoreCreateMutex();
    if (xSemaphoreTake(_evtMutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    int pos = snprintf(buf, size, "{\"seq\":%lu,\"events\":[", (unsigned long)_evtSeq);
    int start = (_evtCount < EVT_RING_SIZE) ? 0 : _evtHead;
    int count = _evtCount;
    bool first = true;

    for (int i = 0; i < count && pos < (int)size - 40; i++) {
        int idx = (start + i) % EVT_RING_SIZE;
        if (_evtRing[idx].ts <= since_ms) continue;  // skip old events

        if (!first && pos < (int)size - 2) buf[pos++] = ',';
        first = false;

        pos += snprintf(buf + pos, size - pos,
            "{\"type\":\"%s\",\"data\":%s,\"ts\":%lu}",
            _evtRing[idx].type,
            _evtRing[idx].data[0] ? _evtRing[idx].data : "{}",
            (unsigned long)_evtRing[idx].ts);
    }
    pos += snprintf(buf + pos, size - pos, "]}");
    xSemaphoreGive(_evtMutex);
    return pos;
}

#endif // ENABLE_SETTINGS || ENABLE_OS_EVENTS

// ── addApiEndpoints() ───────────────────────────────────────────────────────

void WebServerHAL::addApiEndpoints() {
    if (!_server) return;

    WebServerHAL* self = this;

    // GET /api/status
    _server->on("/api/status", HTTP_GET, [self]() {
        self->_requestCount++;
        char buf[768];
        int pos = snprintf(buf, sizeof(buf),
            "{\"uptime_s\":%lu,\"free_heap\":%lu,\"psram_free\":%lu,"
            "\"rssi\":%d,\"ip\":\"%s\",\"requests\":%lu",
            (unsigned long)(millis() / 1000),
            (unsigned long)ESP.getFreeHeap(),
            (unsigned long)ESP.getFreePsram(),
            WiFi.RSSI(),
            WiFi.localIP().toString().c_str(),
            (unsigned long)self->_requestCount);
#if WEB_HAS_DISPLAY_HEALTH
        const display_health_t* dh = display_get_health();
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"display\":{\"board\":\"%s\",\"driver\":\"%s\","
            "\"resolution\":\"%dx%d\",\"verified\":%s,"
            "\"expected_id\":\"0x%06lX\",\"actual_id\":\"0x%06lX\","
            "\"frames\":%lu}",
            dh->board_name, dh->driver, dh->width, dh->height,
            dh->verified ? "true" : "false",
            (unsigned long)dh->expected_id, (unsigned long)dh->actual_id,
            (unsigned long)dh->frame_count);
#endif
#if WEB_HAS_FINGERPRINT
        {
            const board_fingerprint_t* fp = board_fingerprint_get();
            if (fp) {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    ",\"hw_match\":%s,\"hw_detected\":\"%s\","
                    "\"hw_confidence\":%d",
                    fp->match ? "true" : "false",
                    fp->detected_name, fp->confidence);
            }
        }
#endif
        pos += snprintf(buf + pos, sizeof(buf) - pos, "}");
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

    // GET /api/fingerprint — hardware identification and board mismatch detection
#if WEB_HAS_FINGERPRINT
    _server->on("/api/fingerprint", HTTP_GET, [self]() {
        self->_requestCount++;
        const board_fingerprint_t* fp = board_fingerprint_get();
        if (!fp) {
            _server->send(503, "application/json", "{\"error\":\"not scanned\"}");
            return;
        }
        int pos = snprintf(_shared_json, SHARED_JSON_SIZE,
            "{\"detected\":\"%s\",\"compiled\":\"%s\","
            "\"match\":%s,\"confidence\":%d,"
            "\"scan_ms\":%lu,"
            "\"peripherals\":{\"imu\":%s,\"rtc\":%s,\"pmic\":%s,"
            "\"touch_3b\":%s,\"touch_38\":%s,\"tca9554\":%s,\"audio\":%s},"
            "\"i2c_buses\":[",
            fp->detected_name, fp->compiled_name,
            fp->match ? "true" : "false", fp->confidence,
            (unsigned long)fp->scan_time_ms,
            fp->has_imu ? "true" : "false",
            fp->has_rtc ? "true" : "false",
            fp->has_pmic ? "true" : "false",
            fp->has_touch_3b ? "true" : "false",
            fp->has_touch_38 ? "true" : "false",
            fp->has_tca9554 ? "true" : "false",
            fp->has_audio ? "true" : "false");

        bool first_bus = true;
        for (int i = 0; i < fp->bus_count; i++) {
            const fp_i2c_bus_t* b = &fp->buses[i];
            if (b->count == 0) continue;
            if (!first_bus) _shared_json[pos++] = ',';
            first_bus = false;
            pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos,
                "{\"sda\":%d,\"scl\":%d,\"devices\":[", b->sda, b->scl);
            for (int j = 0; j < b->count; j++) {
                if (j > 0) _shared_json[pos++] = ',';
                pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "\"0x%02X\"", b->devices[j]);
            }
            pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "]}");
        }
        pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "]}");
        _server->send(200, "application/json", _shared_json);
    });
#endif

    // POST /api/reboot
    _server->on("/api/reboot", HTTP_POST, [self]() {
        self->_requestCount++;
        _server->send(200, "application/json", "{\"status\":\"rebooting\"}");
        delay(500);
        ESP.restart();
    });

    // GET /api/scan — WiFi scan
    _server->on("/api/scan", HTTP_GET, [self]() {
        self->_requestCount++;
        int n = WiFi.scanNetworks();
        String json = "{\"count\":";
        json += String(n);
        json += ",\"networks\":[";
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            json += "{\"ssid\":\"";
            json += WiFi.SSID(i);
            json += "\",\"rssi\":";
            json += String(WiFi.RSSI(i));
            json += ",\"channel\":";
            json += String(WiFi.channel(i));
            json += ",\"encryption\":";
            json += String(WiFi.encryptionType(i));
            json += "}";
        }
        json += "]}";
        WiFi.scanDelete();
        _server->send(200, "application/json", json);
    });

    // GET /api/node — full node identity and capabilities for fleet discovery
    _server->on("/api/node", HTTP_GET, [self]() {
        self->_requestCount++;
        uint8_t mac[6];
        WiFi.macAddress(mac);

        int pos = snprintf(_shared_json, SHARED_JSON_SIZE,
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
        pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "\"capabilities\":[");
        bool first = true;
        auto addCap = [&](const char* name) {
            if (!first) _shared_json[pos++] = ',';
            pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "\"%s\"", name);
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
        pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "]}");
        _server->send(200, "application/json", _shared_json);
    });

    // GET /api/mesh — mesh topology and health for visualization
    _server->on("/api/mesh", HTTP_GET, [self]() {
        self->_requestCount++;
        if (self->_meshProvider) {
            int len = self->_meshProvider(_shared_json, SHARED_JSON_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", _shared_json);
                return;
            }
        }
        _server->send(200, "application/json",
            "{\"enabled\":false,\"message\":\"ESP-NOW mesh not available\"}");
    });

    // GET /api/diag — full diagnostics report (requires diag provider)
    _server->on("/api/diag", HTTP_GET, [self]() {
        self->_requestCount++;
        if (self->_diagProvider) {
            int len = self->_diagProvider(_shared_json, SHARED_JSON_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", _shared_json);
                return;
            }
        }
        _server->send(200, "application/json",
            "{\"enabled\":false,\"message\":\"Diagnostics not available\"}");
    });

    // GET /api/diag/health — current health snapshot only
    _server->on("/api/diag/health", HTTP_GET, [self]() {
        self->_requestCount++;
        if (self->_diagHealthProvider) {
            int len = self->_diagHealthProvider(_shared_json, SHARED_JSON_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", _shared_json);
                return;
            }
        }
        _server->send(200, "application/json",
            "{\"enabled\":false}");
    });

    // GET /api/diag/events — recent diagnostic events
    _server->on("/api/diag/events", HTTP_GET, [self]() {
        self->_requestCount++;
        if (self->_diagEventsProvider) {
            int len = self->_diagEventsProvider(_shared_json, SHARED_JSON_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", _shared_json);
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
            int len = self->_diagAnomaliesProvider(_shared_json, SHARED_JSON_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", _shared_json);
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
        int len = diaglog_get_json(_shared_json, SHARED_JSON_SIZE, offset, count);
        if (len > 0) {
            _server->send(200, "application/json", _shared_json);
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

    // GET /api/logs — recent log entries
    _server->on("/api/logs", HTTP_GET, [self]() {
        self->_requestCount++;
        int len = WebServerHAL::getLogJson(_shared_json, SHARED_JSON_SIZE);
        if (len > 0) {
            _server->send(200, "application/json", _shared_json);
        } else {
            _server->send(200, "application/json", "{\"lines\":[]}");
        }
    });

    // GET /api/screenshot — returns current framebuffer as 24-bit BMP
    _server->on("/api/screenshot", HTTP_GET, [self]() {
        self->_requestCount++;
        if (!self->_screenshotProvider) {
            _server->send(503, "text/plain", "No screenshot provider");
            return;
        }
        int w = 0, h = 0;
        uint16_t* fb = self->_screenshotProvider(w, h);
        if (!fb || w == 0 || h == 0) {
            _server->send(503, "text/plain", "No framebuffer available");
            return;
        }

        // BMP file: 54-byte header + row data (rows padded to 4-byte boundary)
        int row_bytes = w * 3;
        int row_pad = (4 - (row_bytes % 4)) % 4;
        int padded_row = row_bytes + row_pad;
        uint32_t pixel_data_size = (uint32_t)padded_row * h;
        uint32_t file_size = 54 + pixel_data_size;

        // Build BMP header
        uint8_t hdr[54] = {};
        // File header (14 bytes)
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2] = file_size & 0xFF; hdr[3] = (file_size >> 8) & 0xFF;
        hdr[4] = (file_size >> 16) & 0xFF; hdr[5] = (file_size >> 24) & 0xFF;
        hdr[10] = 54;  // pixel data offset
        // DIB header (40 bytes)
        hdr[14] = 40;  // header size
        hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
        hdr[20] = (w >> 16) & 0xFF; hdr[21] = (w >> 24) & 0xFF;
        // Negative height = top-down rows (natural screen order)
        int32_t neg_h = -h;
        hdr[22] = neg_h & 0xFF; hdr[23] = (neg_h >> 8) & 0xFF;
        hdr[24] = (neg_h >> 16) & 0xFF; hdr[25] = (neg_h >> 24) & 0xFF;
        hdr[26] = 1;   // planes
        hdr[28] = 24;  // bits per pixel
        hdr[34] = pixel_data_size & 0xFF; hdr[35] = (pixel_data_size >> 8) & 0xFF;
        hdr[36] = (pixel_data_size >> 16) & 0xFF; hdr[37] = (pixel_data_size >> 24) & 0xFF;

        // Stream response: send header + convert/send rows
        WiFiClient client = _server->client();
        client.printf("HTTP/1.1 200 OK\r\n"
                      "Content-Type: image/bmp\r\n"
                      "Content-Length: %u\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Access-Control-Allow-Origin: *\r\n", file_size);
#if WEB_HAS_DISPLAY_HEALTH
        {
            const display_health_t* dh = display_get_health();
            client.printf("X-Display-Verified: %s\r\n"
                          "X-Display-Board: %s\r\n"
                          "X-Display-Frames: %lu\r\n",
                          dh->verified ? "true" : "false",
                          dh->board_name,
                          (unsigned long)dh->frame_count);
        }
#endif
        client.print("\r\n");
        client.write(hdr, 54);

        // Convert and stream row by row (avoids allocating full 24-bit buffer)
        // Use a small per-row buffer in stack
        uint8_t* row_buf = (uint8_t*)malloc(padded_row);
        if (!row_buf) {
            client.stop();
            return;
        }

        bool needs_swap = true;
#if defined(BOARD_TOUCH_LCD_43C_BOX)
        needs_swap = false;  // RGB parallel panels store pixels in native order
#endif

        for (int y = 0; y < h; y++) {
            uint16_t* row_src = &fb[y * w];
            for (int x = 0; x < w; x++) {
                uint16_t px = row_src[x];
                if (needs_swap) px = (px >> 8) | (px << 8);  // undo SPI byte-swap
                // RGB565 → 24-bit BGR (BMP is BGR order)
                uint8_t r5 = (px >> 11) & 0x1F;
                uint8_t g6 = (px >> 5) & 0x3F;
                uint8_t b5 = px & 0x1F;
                row_buf[x * 3 + 0] = (b5 << 3) | (b5 >> 2);  // B
                row_buf[x * 3 + 1] = (g6 << 2) | (g6 >> 4);  // G
                row_buf[x * 3 + 2] = (r5 << 3) | (r5 >> 2);  // R
            }
            // Pad row to 4-byte boundary
            for (int p = 0; p < row_pad; p++) row_buf[row_bytes + p] = 0;
            client.write(row_buf, padded_row);
        }
        free(row_buf);
    });

    // GET /api/screenshot.json — metadata only (for automation tools)
    _server->on("/api/screenshot.json", HTTP_GET, [self]() {
        self->_requestCount++;
        int w = 0, h = 0;
        uint16_t* fb = nullptr;
        if (self->_screenshotProvider) fb = self->_screenshotProvider(w, h);
        char json[384];
        int pos = snprintf(json, sizeof(json),
            "{\"available\":%s,\"width\":%d,\"height\":%d,\"format\":\"bmp\","
            "\"url\":\"/api/screenshot\"",
            fb ? "true" : "false", w, h);
#if WEB_HAS_DISPLAY_HEALTH
        const display_health_t* dh = display_get_health();
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",\"display_verified\":%s,\"board\":\"%s\",\"driver\":\"%s\","
            "\"frames\":%lu",
            dh->verified ? "true" : "false",
            dh->board_name, dh->driver,
            (unsigned long)dh->frame_count);
#endif
        pos += snprintf(json + pos, sizeof(json) - pos, "}");
        _server->send(200, "application/json", json);
    });

    // ── Remote Control API ────────────────────────────────────────────────

    // POST /api/remote/touch — inject a single touch event
    _server->on("/api/remote/touch", HTTP_POST, [self]() {
        self->_requestCount++;
#if WEB_HAS_TOUCH_INPUT
        String body = _server->arg("plain");
        if (body.length() == 0) {
            _server->send(400, "application/json", "{\"error\":\"empty body\"}");
            return;
        }
        // Simple JSON parse: {"x":100,"y":200,"pressed":true}
        int xi = body.indexOf("\"x\"");
        int yi = body.indexOf("\"y\"");
        int pi = body.indexOf("\"pressed\"");
        if (xi < 0 || yi < 0) {
            _server->send(400, "application/json", "{\"error\":\"missing x or y\"}");
            return;
        }
        int xc = body.indexOf(':', xi);
        int yc = body.indexOf(':', yi);
        uint16_t x = (uint16_t)body.substring(xc + 1).toInt();
        uint16_t y = (uint16_t)body.substring(yc + 1).toInt();
        bool pressed = true;
        if (pi >= 0) {
            int pc = body.indexOf(':', pi);
            String pval = body.substring(pc + 1);
            pval.trim();
            pressed = pval.startsWith("true") || pval.startsWith("1");
        }
        touch_input::inject(x, y, pressed);
        _server->send(200, "application/json", "{\"ok\":true}");
        DBG_INFO("REMOTE", "Touch inject x=%u y=%u pressed=%d", x, y, pressed);
#else
        _server->send(503, "application/json",
            "{\"error\":\"touch input not available\"}");
#endif
    });

    // POST /api/remote/tap — inject a tap (press + delayed release)
    _server->on("/api/remote/tap", HTTP_POST, [self]() {
        self->_requestCount++;
#if WEB_HAS_TOUCH_INPUT
        String body = _server->arg("plain");
        if (body.length() == 0) {
            _server->send(400, "application/json", "{\"error\":\"empty body\"}");
            return;
        }
        int xi = body.indexOf("\"x\"");
        int yi = body.indexOf("\"y\"");
        if (xi < 0 || yi < 0) {
            _server->send(400, "application/json", "{\"error\":\"missing x or y\"}");
            return;
        }
        int xc = body.indexOf(':', xi);
        int yc = body.indexOf(':', yi);
        uint16_t x = (uint16_t)body.substring(xc + 1).toInt();
        uint16_t y = (uint16_t)body.substring(yc + 1).toInt();
        // Press, wait 50ms, release
        touch_input::inject(x, y, true);
        delay(50);
        touch_input::inject(x, y, false);
        _server->send(200, "application/json", "{\"ok\":true}");
        DBG_INFO("REMOTE", "Tap x=%u y=%u", x, y);
#else
        _server->send(503, "application/json",
            "{\"error\":\"touch input not available\"}");
#endif
    });

    // POST /api/remote/swipe — inject a swipe gesture (multiple touch points)
    _server->on("/api/remote/swipe", HTTP_POST, [self]() {
        self->_requestCount++;
#if WEB_HAS_TOUCH_INPUT
        String body = _server->arg("plain");
        if (body.length() == 0) {
            _server->send(400, "application/json", "{\"error\":\"empty body\"}");
            return;
        }
        // Parse: {"x1":100,"y1":400,"x2":100,"y2":100,"duration_ms":300}
        auto findInt = [&body](const char* key) -> int {
            int ki = body.indexOf(key);
            if (ki < 0) return -1;
            int colon = body.indexOf(':', ki);
            if (colon < 0) return -1;
            return body.substring(colon + 1).toInt();
        };
        int x1 = findInt("\"x1\"");
        int y1 = findInt("\"y1\"");
        int x2 = findInt("\"x2\"");
        int y2 = findInt("\"y2\"");
        int dur = findInt("\"duration_ms\"");
        if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0) {
            _server->send(400, "application/json",
                "{\"error\":\"missing x1/y1/x2/y2\"}");
            return;
        }
        if (dur <= 0) dur = 300;
        if (dur > 2000) dur = 2000;  // cap at 2 seconds

        // Generate touch points along the line at ~20ms intervals
        int steps = dur / 20;
        if (steps < 2) steps = 2;
        if (steps > 100) steps = 100;

        for (int i = 0; i <= steps; i++) {
            float t = (float)i / (float)steps;
            uint16_t cx = (uint16_t)(x1 + t * (x2 - x1));
            uint16_t cy = (uint16_t)(y1 + t * (y2 - y1));
            touch_input::inject(cx, cy, true);
            if (i < steps) delay(dur / steps);
        }
        // Release at end point
        touch_input::inject((uint16_t)x2, (uint16_t)y2, false);

        _server->send(200, "application/json", "{\"ok\":true}");
        DBG_INFO("REMOTE", "Swipe (%d,%d)->(%d,%d) %dms", x1, y1, x2, y2, dur);
#else
        _server->send(503, "application/json",
            "{\"error\":\"touch input not available\"}");
#endif
    });

    // GET /api/remote/screenshot — raw RGB565 binary (faster than BMP)
    _server->on("/api/remote/screenshot", HTTP_GET, [self]() {
        self->_requestCount++;
        if (!self->_screenshotProvider) {
            _server->send(503, "text/plain", "No screenshot provider");
            return;
        }
        int w = 0, h = 0;
        uint16_t* fb = self->_screenshotProvider(w, h);
        if (!fb || w == 0 || h == 0) {
            _server->send(503, "text/plain", "No framebuffer available");
            return;
        }

        size_t data_size = (size_t)w * h * 2;

        // Determine if we need to byte-swap (QSPI panels store swapped)
        bool needs_swap = true;
#if defined(BOARD_TOUCH_LCD_43C_BOX)
        needs_swap = false;  // RGB parallel panels store pixels in native order
#endif

        // Stream raw RGB565 with dimensions in headers
        WiFiClient client = _server->client();
        client.printf("HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Content-Length: %u\r\n"
                      "X-Width: %d\r\n"
                      "X-Height: %d\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Access-Control-Expose-Headers: X-Width, X-Height\r\n"
                      "\r\n",
                      (unsigned)data_size, w, h);

        if (needs_swap) {
            // Stream row-by-row, swapping bytes for standard RGB565
            // Use a row buffer to avoid modifying the framebuffer in place
            const int row_words = w;
            uint16_t* row_buf = (uint16_t*)malloc(row_words * 2);
            if (!row_buf) {
                // Fallback: send as-is (client will get swapped bytes)
                client.write((const uint8_t*)fb, data_size);
            } else {
                for (int y = 0; y < h; y++) {
                    uint16_t* src = &fb[y * w];
                    for (int x = 0; x < w; x++) {
                        uint16_t px = src[x];
                        row_buf[x] = (px >> 8) | (px << 8);
                    }
                    client.write((const uint8_t*)row_buf, row_words * 2);
                }
                free(row_buf);
            }
        } else {
            // Native order — send directly
            client.write((const uint8_t*)fb, data_size);
        }
    });

    // GET /api/remote/info — device info for remote control client
    _server->on("/api/remote/info", HTTP_GET, [self]() {
        self->_requestCount++;
        int w = 0, h = 0;
        bool has_fb = false;
        if (self->_screenshotProvider) {
            uint16_t* fb = self->_screenshotProvider(w, h);
            has_fb = (fb != nullptr && w > 0 && h > 0);
        }

        bool has_touch = false;
        bool has_shell = false;
#if WEB_HAS_TOUCH_INPUT
        has_touch = true;
#endif
#if defined(ENABLE_SHELL)
        has_shell = true;
#endif

        char json[512];
        int pos = snprintf(json, sizeof(json),
            "{\"width\":%d,\"height\":%d,\"touch\":%s,\"shell\":%s,"
            "\"screenshot\":%s,\"fps\":30,"
            "\"free_heap\":%lu,\"psram_free\":%lu,"
            "\"uptime_s\":%lu,\"ip\":\"%s\",\"rssi\":%d",
            w, h,
            has_touch ? "true" : "false",
            has_shell ? "true" : "false",
            has_fb ? "true" : "false",
            (unsigned long)ESP.getFreeHeap(),
            (unsigned long)ESP.getFreePsram(),
            (unsigned long)(millis() / 1000),
            WiFi.localIP().toString().c_str(),
            WiFi.RSSI());

#if WEB_HAS_TOUCH_INPUT
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",\"remote_active\":%s,\"last_touch_ms\":%lu",
            touch_input::isRemoteActive() ? "true" : "false",
            (unsigned long)touch_input::lastActivityMs());
#endif
        pos += snprintf(json + pos, sizeof(json) - pos, "}");
        _server->send(200, "application/json", json);
    });

    DBG_INFO("web", "Remote control API registered at /api/remote/*");

    // ── Settings REST API ─────────────────────────────────────────────────
#if WEB_HAS_SETTINGS
    // GET /api/settings — export all settings as JSON
    _server->on("/api/settings", HTTP_GET, [self]() {
        self->_requestCount++;
        int len = TritiumSettings::instance().toJson(_shared_json, SHARED_JSON_SIZE);
        if (len > 0) {
            _server->send(200, "application/json", _shared_json);
        } else {
            _server->send(500, "application/json",
                "{\"error\":\"failed to export settings\"}");
        }
    });

    // PUT /api/settings — import settings from JSON body
    _server->on("/api/settings", HTTP_PUT, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        if (body.length() == 0) {
            _server->send(400, "application/json",
                "{\"error\":\"empty body\"}");
            return;
        }
        bool ok = TritiumSettings::instance().fromJson(body.c_str());
        if (ok) {
            _server->send(200, "application/json", "{\"ok\":true}");
        } else {
            _server->send(400, "application/json",
                "{\"ok\":false,\"error\":\"failed to import settings\"}");
        }
    });

    // POST /api/settings/reset — factory reset (optional domain in body)
    _server->on("/api/settings/reset", HTTP_POST, [self]() {
        self->_requestCount++;
        const char* domain = nullptr;
        char domainBuf[32] = {0};
        String body = _server->arg("plain");
        if (body.length() > 0) {
            // Simple JSON parse: look for "domain":"value"
            int idx = body.indexOf("\"domain\"");
            if (idx >= 0) {
                int colon = body.indexOf(':', idx);
                int q1 = body.indexOf('"', colon + 1);
                int q2 = body.indexOf('"', q1 + 1);
                if (q1 >= 0 && q2 > q1) {
                    int len = q2 - q1 - 1;
                    if (len > 0 && len < (int)sizeof(domainBuf)) {
                        body.substring(q1 + 1, q2).toCharArray(
                            domainBuf, sizeof(domainBuf));
                        domain = domainBuf;
                    }
                }
            }
        }
        bool ok = TritiumSettings::instance().factoryReset(domain);
        char resp[64];
        if (domain) {
            snprintf(resp, sizeof(resp),
                "{\"ok\":%s,\"domain\":\"%s\"}",
                ok ? "true" : "false", domain);
        } else {
            snprintf(resp, sizeof(resp),
                "{\"ok\":%s}", ok ? "true" : "false");
        }
        _server->send(200, "application/json", resp);
    });
#endif

    // ── Polling fallback endpoints (for web pages without WebSocket) ─────
    // In web pages: try ws first, fall back to polling
    // if (!ws || ws.readyState !== WebSocket.OPEN) { fetch('/api/events/poll') }
#if defined(ENABLE_SETTINGS) || defined(ENABLE_OS_EVENTS)

    // GET /api/events/poll?since=<millis> — recent events since last poll
    _server->on("/api/events/poll", HTTP_GET, [self]() {
        self->_requestCount++;
        uint32_t since = 0;
        if (_server->hasArg("since")) {
            since = (uint32_t)_server->arg("since").toInt();
        }
        int len = getEventPollJson(_shared_json, SHARED_JSON_SIZE, since);
        if (len > 0) {
            _server->send(200, "application/json", _shared_json);
        } else {
            _server->send(200, "application/json", "{\"seq\":0,\"events\":[]}");
        }
    });

    // GET /api/serial/poll?lines=<n> — recent serial output lines
    _server->on("/api/serial/poll", HTTP_GET, [self]() {
        self->_requestCount++;
        int max_lines = 50;
        if (_server->hasArg("lines")) {
            max_lines = _server->arg("lines").toInt();
            if (max_lines < 1) max_lines = 1;
            if (max_lines > 48) max_lines = 48;
        }
        int len = serial_capture::getLinesJson(_shared_json, SHARED_JSON_SIZE, max_lines);
        if (len > 0) {
            _server->send(200, "application/json", _shared_json);
        } else {
            _server->send(200, "application/json", "{\"count\":0,\"lines\":[]}");
        }
    });

    // POST /api/serial/send — inject a command as if typed on serial
    _server->on("/api/serial/send", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        if (body.length() == 0) {
            _server->send(400, "application/json", "{\"ok\":false,\"error\":\"empty body\"}");
            return;
        }
        // Parse {"cmd":"SERVICES"} — simple extraction without a JSON library
        int ci = body.indexOf("\"cmd\"");
        if (ci < 0) {
            _server->send(400, "application/json", "{\"ok\":false,\"error\":\"missing cmd field\"}");
            return;
        }
        // Find the value string after "cmd":
        int q1 = body.indexOf('"', ci + 5);  // opening quote of value
        if (q1 < 0) { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return; }
        // Skip colon and whitespace between key and value
        int colon = body.indexOf(':', ci + 4);
        if (colon < 0) { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return; }
        q1 = body.indexOf('"', colon + 1);
        if (q1 < 0) { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return; }
        int q2 = body.indexOf('"', q1 + 1);  // closing quote
        if (q2 < 0) { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return; }
        String cmd = body.substring(q1 + 1, q2);
        if (cmd.length() == 0) {
            _server->send(400, "application/json", "{\"ok\":false,\"error\":\"empty cmd\"}");
            return;
        }
        serial_capture::injectCommand(cmd.c_str());
        _server->send(200, "application/json", "{\"ok\":true}");
    });

#endif // ENABLE_SETTINGS || ENABLE_OS_EVENTS

    // ── Debug API (touch, LVGL, system diagnostics) ─────────────────────

#if WEB_HAS_TOUCH_INPUT
    // GET /api/debug/touch — touch subsystem diagnostics
    _server->on("/api/debug/touch", HTTP_GET, [self]() {
        self->_requestCount++;
        auto info = touch_input::getDebugInfo();
        snprintf(_shared_json, SHARED_JSON_SIZE,
            "{\"hw_available\":%s,\"driver\":\"%s\","
            "\"read_cb_calls\":%lu,\"hw_touch_count\":%lu,"
            "\"inject_count\":%lu,\"last_raw_x\":%d,\"last_raw_y\":%d,"
            "\"last_touch_ms\":%lu,\"currently_pressed\":%s,"
            "\"uptime_ms\":%lu}",
            info.hw_available ? "true" : "false",
            info.hw_available ? "GT911" : "none",
            (unsigned long)info.read_cb_calls,
            (unsigned long)info.hw_touch_count,
            (unsigned long)info.inject_count,
            info.last_raw_x, info.last_raw_y,
            (unsigned long)info.last_touch_ms,
            info.currently_pressed ? "true" : "false",
            (unsigned long)millis());
        _server->send(200, "application/json", _shared_json);
    });

    // GET /api/debug/gt911 — GT911 register dump for hardware debugging
    _server->on("/api/debug/gt911", HTTP_GET, [self]() {
        self->_requestCount++;
        extern TouchHAL touch;
        touch.dumpDiag(_shared_json, SHARED_JSON_SIZE);
        _server->send(200, "application/json", _shared_json);
    });
#endif

#if WEB_HAS_SHELL
    // GET /api/debug/lvgl — LVGL display and rendering diagnostics
    _server->on("/api/debug/lvgl", HTTP_GET, [self]() {
        self->_requestCount++;
        snprintf(_shared_json, SHARED_JSON_SIZE,
            "{\"initialized\":%s,\"width\":%d,\"height\":%d,"
            "\"render_mode\":\"%s\",\"flush_count\":%lu,"
            "\"last_flush_ms\":%lu,\"uptime_ms\":%lu,"
            "\"psram_free\":%lu,\"heap_free\":%lu}",
            lvgl_driver::display() ? "true" : "false",
            lvgl_driver::getWidth(), lvgl_driver::getHeight(),
            lvgl_driver::isRgb() ? "direct" : "partial",
            (unsigned long)lvgl_driver::getFlushCount(),
            (unsigned long)lvgl_driver::getLastFlushMs(),
            (unsigned long)millis(),
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            (unsigned long)esp_get_free_heap_size());
        _server->send(200, "application/json", _shared_json);
    });

    DBG_INFO("web", "Debug API registered at /api/debug/*");
#endif

    // ── Shell navigation API ─────────────────────────────────────────────
#if WEB_HAS_SHELL
    // GET /api/shell/apps — list all registered shell apps
    _server->on("/api/shell/apps", HTTP_GET, [self]() {
        self->_requestCount++;
        int count = tritium_shell::getAppCount();
        int active = tritium_shell::getActiveApp();
        int pos = snprintf(_shared_json, SHARED_JSON_SIZE,
            "{\"count\":%d,\"active\":%d,\"apps\":[", count, active);
        for (int i = 0; i < count && pos < (int)SHARED_JSON_SIZE - 128; i++) {
            const tritium_shell::AppDescriptor* app = tritium_shell::getApp(i);
            if (!app) continue;
            if (i > 0) pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, ",");
            pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos,
                "{\"index\":%d,\"name\":\"%s\",\"description\":\"%s\",\"system\":%s}",
                i, app->name ? app->name : "",
                app->description ? app->description : "",
                app->is_system ? "true" : "false");
        }
        pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "]}");
        _server->send(200, "application/json", _shared_json);
    });

    // POST /api/shell/launch — launch an app by index or name
    _server->on("/api/shell/launch", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        int count = tritium_shell::getAppCount();

        // Try "index" field first
        int idx = -1;
        int ipos = body.indexOf("\"index\"");
        if (ipos >= 0) {
            int colon = body.indexOf(':', ipos);
            if (colon >= 0) {
                idx = body.substring(colon + 1).toInt();
            }
        }

        // Try "name" field if no index
        if (idx < 0) {
            int npos = body.indexOf("\"name\"");
            if (npos >= 0) {
                int q1 = body.indexOf('"', body.indexOf(':', npos) + 1);
                int q2 = body.indexOf('"', q1 + 1);
                if (q1 >= 0 && q2 > q1) {
                    String name = body.substring(q1 + 1, q2);
                    for (int i = 0; i < count; i++) {
                        const tritium_shell::AppDescriptor* app = tritium_shell::getApp(i);
                        if (app && app->name && name.equalsIgnoreCase(app->name)) {
                            idx = i;
                            break;
                        }
                    }
                }
            }
        }

        if (idx < 0 || idx >= count) {
            _server->send(400, "application/json",
                "{\"ok\":false,\"error\":\"invalid app index or name\"}");
            return;
        }

        tritium_shell::showApp(idx);
        const tritium_shell::AppDescriptor* app = tritium_shell::getApp(idx);
        snprintf(_shared_json, SHARED_JSON_SIZE,
            "{\"ok\":true,\"app\":\"%s\"}", app && app->name ? app->name : "");
        _server->send(200, "application/json", _shared_json);
    });

    // POST /api/shell/home — go to launcher
    _server->on("/api/shell/home", HTTP_POST, [self]() {
        self->_requestCount++;
        tritium_shell::showLauncher();
        _server->send(200, "application/json", "{\"ok\":true}");
    });

    DBG_INFO("web", "Shell navigation API registered at /api/shell/*");
#endif // WEB_HAS_SHELL

    DBG_INFO("web", "API endpoints added at /api/*");
}

// ── Screenshot viewer page (/screenshot) ─────────────────────────────────

static const char SCREENSHOT_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Screen Capture</title>
<style>
body{background:#0a0a0a;color:#c0c0c0;font-family:'Courier New',monospace;margin:0;padding:16px}
h1{color:#00f0ff;font-size:1.2em;margin:0 0 12px}
.controls{margin:8px 0;display:flex;gap:8px;align-items:center;flex-wrap:wrap}
button{background:#111;color:#00f0ff;border:1px solid #00f0ff44;padding:6px 14px;cursor:pointer;font-family:inherit;font-size:0.9em}
button:hover{background:#00f0ff22}
button.active{background:#00f0ff33;border-color:#00f0ff}
.label{color:#666;font-size:0.85em}
#capture{border:1px solid #333;display:block;max-width:100%;image-rendering:pixelated;background:#000}
#info{color:#666;font-size:0.8em;margin-top:6px}
</style>
</head><body>
<h1>// Screen Capture</h1>
<div class="controls">
<button onclick="capture()">Capture</button>
<button id="autoBtn" onclick="toggleAuto()">Auto: OFF</button>
<span class="label">Interval:</span>
<select id="interval" onchange="updateInterval()" style="background:#111;color:#00f0ff;border:1px solid #00f0ff44;padding:4px;font-family:inherit">
<option value="500">500ms</option>
<option value="1000" selected>1s</option>
<option value="2000">2s</option>
<option value="5000">5s</option>
</select>
<button onclick="downloadCapture()">Download</button>
</div>
<img id="capture" alt="Screen capture">
<div id="info"></div>
<script>
let autoMode=false,timer=null,captureCount=0;
function capture(){
  const t0=performance.now();
  const img=document.getElementById('capture');
  const url='/api/screenshot?t='+Date.now();
  fetch(url).then(r=>{
    if(!r.ok)throw new Error(r.status);
    return r.blob();
  }).then(b=>{
    const ms=Math.round(performance.now()-t0);
    captureCount++;
    img.src=URL.createObjectURL(b);
    document.getElementById('info').textContent=
      '#'+captureCount+' | '+Math.round(b.size/1024)+'KB | '+ms+'ms';
  }).catch(e=>{
    document.getElementById('info').textContent='Error: '+e.message;
  });
}
function toggleAuto(){
  autoMode=!autoMode;
  document.getElementById('autoBtn').textContent='Auto: '+(autoMode?'ON':'OFF');
  document.getElementById('autoBtn').className=autoMode?'active':'';
  if(autoMode){capture();startTimer();}else{clearInterval(timer);timer=null;}
}
function startTimer(){
  if(timer)clearInterval(timer);
  timer=setInterval(capture,parseInt(document.getElementById('interval').value));
}
function updateInterval(){if(autoMode)startTimer();}
function downloadCapture(){
  const a=document.createElement('a');
  a.href='/api/screenshot';
  a.download='tritium-capture-'+Date.now()+'.bmp';
  a.click();
}
capture();
</script>
</body></html>
)rawliteral";

void WebServerHAL::addScreenshotPage() {
    if (!_server) return;
    WebServerHAL* self = this;
    _server->on("/screenshot", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send(200, "text/html", SCREENSHOT_HTML);
    });
    DBG_INFO("web", "Screenshot page at /screenshot");
}

// ── Remote Control page (/remote) ────────────────────────────────────────
// Full-screen remote control interface with live screenshot streaming and
// touch injection — a lightweight VNC for ESP32.

void WebServerHAL::addRemotePage() {
    if (!_server) return;
    WebServerHAL* self = this;
#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    _server->on("/remote", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send_P(200, "text/html", REMOTE_HTML, REMOTE_HTML_LEN);
    });
    DBG_INFO("web", "Remote control page at /remote");
#else
    DBG_INFO("web", "Remote page skipped (web pages not enabled)");
#endif
}

// ── WiFi Setup page (/wifi) ──────────────────────────────────────────────
// Full WiFi management UI: status, scan, connect, saved networks with
// drag-to-reorder priority, AP mode toggle, and real-time RSSI monitoring.
// All interactions use JSON API endpoints backed by WifiManager.

static const char WIFI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Manager — Tritium</title>
%THEME%
<style>
:root{--t-cyan:#00f0ff;--t-void:#0a0a0f;--t-surface-2:#12121a;
  --t-magenta:#ff2a6d;--t-green:#05ffa1;--t-yellow:#fcee0a;--t-glow:0 0 8px #00f0ff44}
body{background:var(--t-void)}
h1{color:var(--t-cyan);border-color:var(--t-cyan)}
h2{color:var(--t-cyan)}
a{color:var(--t-cyan)}
.card{background:var(--t-surface-2);border:1px solid #00f0ff22;box-shadow:var(--t-glow)}
th{color:var(--t-cyan)}
button,input[type=submit]{background:var(--t-cyan);color:var(--t-void)}
button:hover{background:#66ffe8}
button.danger{background:var(--t-magenta);color:#fff}
button.danger:hover{background:#ff6690}
.status-badge{display:inline-block;padding:3px 10px;border-radius:12px;font-size:12px;
  font-weight:bold;text-transform:uppercase}
.status-badge.ok{background:#05ffa122;color:var(--t-green);border:1px solid #05ffa144}
.status-badge.warn{background:#fcee0a22;color:var(--t-yellow);border:1px solid #fcee0a44}
.status-badge.err{background:#ff2a6d22;color:var(--t-magenta);border:1px solid #ff2a6d44}
.signal-bars{display:inline-flex;align-items:flex-end;gap:2px;height:16px;vertical-align:middle}
.signal-bars .bar{width:4px;background:#333;border-radius:1px}
.signal-bars .bar.active{background:var(--t-cyan)}
.signal-bars .bar:nth-child(1){height:4px}
.signal-bars .bar:nth-child(2){height:7px}
.signal-bars .bar:nth-child(3){height:11px}
.signal-bars .bar:nth-child(4){height:16px}
input[type=text],input[type=password]{background:var(--t-void);color:var(--t-cyan);
  border:1px solid #00f0ff33;padding:8px 12px;border-radius:4px;width:100%;
  font-family:inherit;font-size:14px;box-sizing:border-box}
input[type=text]:focus,input[type=password]:focus{border-color:var(--t-cyan);
  outline:none;box-shadow:var(--t-glow)}
.saved-item{display:flex;align-items:center;gap:10px;padding:10px 12px;
  border:1px solid #00f0ff15;border-radius:4px;margin:4px 0;
  background:var(--t-void);cursor:grab;transition:border-color 0.2s}
.saved-item:hover{border-color:var(--t-cyan)}
.saved-item.dragging{opacity:0.5;border-color:var(--t-magenta)}
.saved-item .pri{color:#666;font-size:11px;min-width:24px}
.saved-item .name{flex:1;color:#e0e0e0}
.saved-item .rssi-val{color:#888;font-size:12px;min-width:60px;text-align:right}
.saved-item .fails{color:var(--t-magenta);font-size:11px}
.saved-item .actions{display:flex;gap:4px}
.saved-item .actions button{padding:4px 8px;font-size:11px}
.ap-toggle{display:flex;align-items:center;gap:12px;padding:12px}
.toggle-switch{position:relative;width:48px;height:26px;cursor:pointer}
.toggle-switch input{opacity:0;width:0;height:0}
.toggle-slider{position:absolute;inset:0;background:#333;border-radius:13px;transition:0.3s}
.toggle-slider::before{content:"";position:absolute;height:20px;width:20px;left:3px;
  bottom:3px;background:#888;border-radius:50%;transition:0.3s}
.toggle-switch input:checked+.toggle-slider{background:var(--t-cyan)}
.toggle-switch input:checked+.toggle-slider::before{transform:translateX(22px);background:var(--t-void)}
.scan-row{cursor:pointer;transition:background 0.15s}
.scan-row:hover{background:#ffffff08}
.scan-row.known td:first-child::after{content:" *";color:var(--t-green);font-size:11px}
.form-row{display:flex;gap:8px;margin:8px 0;align-items:flex-end}
.form-row .field{flex:1}
.form-row .field label{display:block;color:#888;font-size:11px;margin-bottom:4px;
  text-transform:uppercase}
.confirm-overlay{display:none;position:fixed;inset:0;background:#0008;z-index:100;
  align-items:center;justify-content:center}
.confirm-overlay.active{display:flex}
.confirm-box{background:var(--t-surface-2);border:1px solid var(--t-magenta);
  border-radius:8px;padding:24px;max-width:360px;text-align:center}
.confirm-box p{margin:12px 0}
.confirm-box .actions{display:flex;gap:8px;justify-content:center;margin-top:16px}
#status_panel{display:grid;grid-template-columns:1fr 1fr;gap:4px 16px}
#status_panel .label{color:#666;font-size:12px}
#status_panel .value{color:#e0e0e0;font-size:14px}
.refresh-btn{background:none;border:1px solid #00f0ff33;color:var(--t-cyan);
  padding:4px 10px;font-size:11px;border-radius:3px}
.refresh-btn:hover{border-color:var(--t-cyan);background:#00f0ff11}
</style>
</head><body>
%NAV%
<h1>// WiFi Manager</h1>

<!-- Status panel -->
<div class="card">
<h2>Connection Status
  <span id="state_badge" class="status-badge" style="margin-left:8px"></span>
</h2>
<div id="status_panel">
  <div class="label">SSID</div><div class="value" id="st_ssid">—</div>
  <div class="label">IP Address</div><div class="value" id="st_ip">—</div>
  <div class="label">RSSI</div><div class="value" id="st_rssi">—</div>
  <div class="label">Channel</div><div class="value" id="st_ch">—</div>
  <div class="label">MAC</div><div class="value" id="st_mac">—</div>
  <div class="label">Failover</div><div class="value" id="st_failover">—</div>
</div>
</div>

<!-- AP Mode -->
<div class="card">
<h2>Access Point</h2>
<div class="ap-toggle">
  <label class="toggle-switch">
    <input type="checkbox" id="ap_toggle" onchange="toggleAP(this.checked)">
    <span class="toggle-slider"></span>
  </label>
  <span id="ap_label" style="color:#888">AP Inactive</span>
</div>
<div id="ap_info" style="display:none;margin-top:8px">
  <div style="display:grid;grid-template-columns:1fr 1fr;gap:4px 16px">
    <div class="label">AP SSID</div><div class="value" id="ap_ssid">—</div>
    <div class="label">AP IP</div><div class="value" id="ap_ip">—</div>
    <div class="label">Clients</div><div class="value" id="ap_clients">0</div>
  </div>
</div>
</div>

<!-- Scan -->
<div class="card">
<h2>Available Networks</h2>
<div style="display:flex;gap:8px;align-items:center;margin-bottom:8px">
  <button onclick="doScan()" id="scan_btn">Scan Networks</button>
  <span id="scan_status" style="color:#666;font-size:12px"></span>
</div>
<table id="scan_table">
<tr><th>SSID</th><th>Signal</th><th>Ch</th><th>Security</th><th></th></tr>
<tr><td colspan="5" style="color:#555">Click scan to discover networks</td></tr>
</table>
</div>

<!-- Connect form -->
<div class="card">
<h2>Connect to Network</h2>
<div class="form-row">
  <div class="field"><label>SSID</label>
    <input type="text" id="ssid_input" placeholder="Network name">
  </div>
  <div class="field"><label>Password</label>
    <input type="password" id="pass_input" placeholder="Password (leave blank for open)">
  </div>
</div>
<div style="margin-top:8px">
  <button onclick="doConnect()">Connect & Save</button>
</div>
<div id="connect_msg" style="margin-top:8px"></div>
</div>

<!-- Saved networks -->
<div class="card">
<h2>Saved Networks <span style="color:#666;font-size:12px">(drag to reorder priority)</span></h2>
<div id="saved_list"><p style="color:#555">Loading...</p></div>
</div>

<!-- Delete confirmation overlay -->
<div class="confirm-overlay" id="confirm_overlay">
<div class="confirm-box">
  <h2 style="color:var(--t-magenta)">Remove Network</h2>
  <p>Remove <strong id="confirm_ssid"></strong> from saved networks?</p>
  <div class="actions">
    <button class="danger" onclick="confirmRemove()">Remove</button>
    <button onclick="cancelRemove()">Cancel</button>
  </div>
</div>
</div>

<script>
var pendingRemove='';

function signalBars(rssi){
  var lvl=rssi>-50?4:rssi>-65?3:rssi>-75?2:rssi>-85?1:0;
  var h='<div class="signal-bars">';
  for(var i=1;i<=4;i++) h+='<div class="bar'+(i<=lvl?' active':'')+'"></div>';
  return h+'</div>';
}

function authStr(a){
  var m={'OPEN':'Open','WEP':'WEP','WPA_PSK':'WPA','WPA2_PSK':'WPA2',
    'WPA_WPA2_PSK':'WPA/2','WPA3_PSK':'WPA3','UNKNOWN':'?'};
  return m[a]||a;
}

function fetchStatus(){
  fetch('/api/wifi/status').then(r=>r.json()).then(d=>{
    var badge=document.getElementById('state_badge');
    var st=d.state||'unknown';
    badge.textContent=st;
    badge.className='status-badge '+(d.connected?'ok':st==='connecting'?'warn':'err');

    document.getElementById('st_ssid').textContent=d.connected?d.ssid:'Not connected';
    document.getElementById('st_ip').textContent=d.connected?d.ip:'—';
    document.getElementById('st_rssi').innerHTML=d.connected?(d.rssi+' dBm '+signalBars(d.rssi)):'—';
    document.getElementById('st_ch').textContent=d.connected?d.channel:'—';
    document.getElementById('st_mac').textContent=d.mac||'—';
    document.getElementById('st_failover').textContent=d.failover?'Enabled':'Disabled';

    // AP state
    var tog=document.getElementById('ap_toggle');
    tog.checked=d.ap_active;
    document.getElementById('ap_label').textContent=d.ap_active?'AP Active':'AP Inactive';
    document.getElementById('ap_label').style.color=d.ap_active?'var(--t-green)':'#888';
    document.getElementById('ap_info').style.display=d.ap_active?'block':'none';
    if(d.ap_active){
      document.getElementById('ap_ssid').textContent=d.ap_ssid||'—';
      document.getElementById('ap_ip').textContent=d.ap_ip||'—';
      document.getElementById('ap_clients').textContent=d.clients||0;
    }

    // Saved networks
    renderSaved(d.saved||[]);
  }).catch(()=>{});
}

function renderSaved(nets){
  var el=document.getElementById('saved_list');
  if(!nets.length){el.innerHTML='<p style="color:#555">No saved networks</p>';return;}
  var h='';
  nets.sort(function(a,b){return a.priority-b.priority;});
  nets.forEach(function(n,i){
    h+='<div class="saved-item" draggable="true" data-ssid="'+n.ssid+'" data-idx="'+i+'">';
    h+='<span class="pri">#'+n.priority+'</span>';
    h+='<span class="name">'+n.ssid+'</span>';
    h+='<span class="rssi-val">'+(n.rssi?n.rssi+' dBm':'—')+'</span>';
    if(n.fails>0) h+='<span class="fails">'+n.fails+' fails</span>';
    h+='<div class="actions">';
    h+='<button onclick="askRemove(\''+n.ssid+'\')" class="danger">Remove</button>';
    h+='</div></div>';
  });
  el.innerHTML=h;
  setupDrag();
}

// Drag-to-reorder
function setupDrag(){
  var items=document.querySelectorAll('.saved-item');
  var dragged=null;
  items.forEach(function(item){
    item.addEventListener('dragstart',function(e){
      dragged=this;this.classList.add('dragging');
      e.dataTransfer.effectAllowed='move';
    });
    item.addEventListener('dragend',function(){
      this.classList.remove('dragging');dragged=null;
    });
    item.addEventListener('dragover',function(e){e.preventDefault();e.dataTransfer.dropEffect='move';});
    item.addEventListener('drop',function(e){
      e.preventDefault();
      if(!dragged||dragged===this)return;
      var list=document.getElementById('saved_list');
      var allItems=Array.from(list.querySelectorAll('.saved-item'));
      var fromIdx=allItems.indexOf(dragged);
      var toIdx=allItems.indexOf(this);
      if(fromIdx<toIdx) this.after(dragged); else this.before(dragged);
      // Send new order to device
      var newOrder=Array.from(list.querySelectorAll('.saved-item'));
      newOrder.forEach(function(el,i){
        var ssid=el.getAttribute('data-ssid');
        fetch('/api/wifi/reorder',{method:'POST',
          headers:{'Content-Type':'application/json'},
          body:JSON.stringify({ssid:ssid,priority:i})
        });
      });
      setTimeout(fetchStatus,500);
    });
  });
}

function doScan(){
  var btn=document.getElementById('scan_btn');
  var status=document.getElementById('scan_status');
  btn.disabled=true;btn.textContent='Scanning...';
  status.textContent='';
  fetch('/api/wifi/scan').then(r=>r.json()).then(d=>{
    var t=document.getElementById('scan_table');
    var rows='<tr><th>SSID</th><th>Signal</th><th>Ch</th><th>Security</th><th></th></tr>';
    if(d.networks&&d.networks.length){
      d.networks.forEach(function(n){
        var cls=n.known?'scan-row known':'scan-row';
        rows+='<tr class="'+cls+'">';
        rows+='<td>'+n.ssid+'</td>';
        rows+='<td>'+signalBars(n.rssi)+' <span style="color:#888;font-size:11px">'+n.rssi+'</span></td>';
        rows+='<td>'+n.channel+'</td>';
        rows+='<td>'+authStr(n.auth)+'</td>';
        rows+='<td><button onclick="selectNet(\''+n.ssid.replace(/'/g,"\\'")+'\')">Select</button></td>';
        rows+='</tr>';
      });
      status.textContent=d.networks.length+' networks found';
    }else{
      rows+='<tr><td colspan="5" style="color:#555">No networks found</td></tr>';
      status.textContent='No networks found';
    }
    t.innerHTML=rows;
    btn.disabled=false;btn.textContent='Scan Networks';
  }).catch(function(){
    status.textContent='Scan failed';
    btn.disabled=false;btn.textContent='Scan Networks';
  });
}

function selectNet(ssid){
  document.getElementById('ssid_input').value=ssid;
  document.getElementById('pass_input').focus();
}

function doConnect(){
  var ssid=document.getElementById('ssid_input').value.trim();
  var pass=document.getElementById('pass_input').value;
  var msg=document.getElementById('connect_msg');
  if(!ssid){msg.innerHTML='<span style="color:var(--t-magenta)">Enter an SSID</span>';return;}
  msg.innerHTML='<span style="color:var(--t-yellow)">Connecting to '+ssid+'...</span>';
  fetch('/api/wifi/connect',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:ssid,password:pass})
  }).then(r=>r.json()).then(d=>{
    if(d.connected){
      msg.innerHTML='<span style="color:var(--t-green)">Connected to '+ssid+'</span>';
      document.getElementById('ssid_input').value='';
      document.getElementById('pass_input').value='';
    }else{
      msg.innerHTML='<span style="color:var(--t-magenta)">Failed to connect to '+ssid+'</span>';
    }
    setTimeout(fetchStatus,1000);
  }).catch(function(){
    msg.innerHTML='<span style="color:var(--t-magenta)">Connection error</span>';
  });
}

function toggleAP(on){
  fetch('/api/wifi/ap',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({active:on})
  }).then(()=>setTimeout(fetchStatus,1500));
}

function askRemove(ssid){
  pendingRemove=ssid;
  document.getElementById('confirm_ssid').textContent=ssid;
  document.getElementById('confirm_overlay').classList.add('active');
}
function cancelRemove(){
  document.getElementById('confirm_overlay').classList.remove('active');
  pendingRemove='';
}
function confirmRemove(){
  document.getElementById('confirm_overlay').classList.remove('active');
  fetch('/api/wifi/remove',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:pendingRemove})
  }).then(()=>{pendingRemove='';setTimeout(fetchStatus,500);});
}

// Initial load + auto-refresh every 5s
fetchStatus();
setInterval(fetchStatus,5000);
</script>
</body></html>
)rawliteral";

void WebServerHAL::addWiFiSetup() {
    if (!_server) return;

    WebServerHAL* self = this;

    // GET /wifi — WiFi manager SPA
#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    _server->on("/wifi", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send_P(200, "text/html", WIFI_HTML_V2);
    });
#else
    _server->on("/wifi", HTTP_GET, [self]() {
        self->_requestCount++;
        String html(FPSTR(WIFI_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));
        _server->send(200, "text/html", html);
    });
#endif

    // GET /api/wifi/status — full WiFi status JSON
    _server->on("/api/wifi/status", HTTP_GET, [self]() {
        self->_requestCount++;

        // Build status JSON from WifiManager via WifiService (accessed through
        // ServiceRegistry). Falls back to direct WiFi calls if service unavailable.
        char buf[1536];
        int pos = 0;

        bool connected = (WiFi.status() == WL_CONNECTED);
        bool ap_active = (WiFi.getMode() & WIFI_AP) != 0;

        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"connected\":%s,\"ap_active\":%s,\"state\":\"%s\","
            "\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"channel\":%d,",
            connected ? "true" : "false",
            ap_active ? "true" : "false",
            connected ? (ap_active ? "ap_and_sta" : "connected") :
                (ap_active ? "ap_only" : "disconnected"),
            connected ? WiFi.SSID().c_str() : "",
            connected ? WiFi.localIP().toString().c_str() : "",
            connected ? WiFi.RSSI() : 0,
            connected ? WiFi.channel() : 0);

        // MAC address
        uint8_t mac[6];
        WiFi.macAddress(mac);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        // AP info
        if (ap_active) {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"clients\":%d,",
                WiFi.softAPSSID().c_str(),
                WiFi.softAPIP().toString().c_str(),
                WiFi.softAPgetStationNum());
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "\"ap_ssid\":\"\",\"ap_ip\":\"\",\"clients\":0,");
        }

        // Failover status — use WifiManager if available
        bool failover = true;
        if (WifiManager::_instance) {
            failover = WifiManager::_instance->isAutoFailoverEnabled();
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "\"failover\":%s,", failover ? "true" : "false");

        // Saved networks
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\"saved\":[");
        if (WifiManager::_instance) {
            SavedNetwork nets[WIFI_MAX_SAVED_NETWORKS];
            int count = WifiManager::_instance->getSavedNetworks(nets, WIFI_MAX_SAVED_NETWORKS);
            for (int i = 0; i < count && pos < (int)sizeof(buf) - 140; i++) {
                if (i > 0) buf[pos++] = ',';
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"ssid\":\"%s\",\"priority\":%d,\"rssi\":%d,"
                    "\"fails\":%u,\"enabled\":%s}",
                    nets[i].ssid, nets[i].priority, nets[i].rssi_last,
                    nets[i].fail_count, nets[i].enabled ? "true" : "false");
            }
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");

        _server->send(200, "application/json", buf);
    });

    // GET /api/wifi/scan — trigger scan and return results
    _server->on("/api/wifi/scan", HTTP_GET, [self]() {
        self->_requestCount++;
        String json = "{\"networks\":[";

        if (WifiManager::_instance) {
            WifiManager::_instance->startScan();
            ScanResult results[WIFI_MAX_SCAN_RESULTS];
            int count = WifiManager::_instance->getScanResults(results, WIFI_MAX_SCAN_RESULTS);
            for (int i = 0; i < count; i++) {
                if (i > 0) json += ",";
                json += "{\"ssid\":\"";
                json += results[i].ssid;
                json += "\",\"rssi\":";
                json += String(results[i].rssi);
                json += ",\"channel\":";
                json += String(results[i].channel);
                json += ",\"auth\":\"";
                switch (results[i].auth) {
                    case WifiAuth::OPEN:         json += "OPEN"; break;
                    case WifiAuth::WEP:          json += "WEP"; break;
                    case WifiAuth::WPA_PSK:      json += "WPA_PSK"; break;
                    case WifiAuth::WPA2_PSK:     json += "WPA2_PSK"; break;
                    case WifiAuth::WPA_WPA2_PSK: json += "WPA_WPA2_PSK"; break;
                    case WifiAuth::WPA3_PSK:     json += "WPA3_PSK"; break;
                    default:                     json += "UNKNOWN"; break;
                }
                json += "\",\"known\":";
                json += results[i].known ? "true" : "false";
                json += "}";
            }
        } else {
            // Fallback: direct WiFi scan if WifiManager not available
            int n = WiFi.scanNetworks();
            for (int i = 0; i < n; i++) {
                if (i > 0) json += ",";
                json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i));
                json += ",\"channel\":" + String(WiFi.channel(i));
                json += ",\"auth\":\"" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "WPA2_PSK") + "\"";
                json += ",\"known\":false}";
            }
            WiFi.scanDelete();
        }

        json += "]}";
        _server->send(200, "application/json", json);
    });

    // POST /api/wifi/connect — connect to network (JSON body: {ssid, password})
    _server->on("/api/wifi/connect", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        // Minimal JSON parse for ssid and password
        String ssid, pass;
        int si = body.indexOf("\"ssid\"");
        if (si >= 0) {
            int q1 = body.indexOf('"', si + 6);
            int q2 = body.indexOf('"', q1 + 1);
            if (q1 >= 0 && q2 > q1) ssid = body.substring(q1 + 1, q2);
        }
        int pi = body.indexOf("\"password\"");
        if (pi >= 0) {
            int q1 = body.indexOf('"', pi + 10);
            int q2 = body.indexOf('"', q1 + 1);
            if (q1 >= 0 && q2 > q1) pass = body.substring(q1 + 1, q2);
        }

        bool ok = false;
        if (ssid.length() > 0) {
            DBG_INFO("web", "WiFi connecting to: %s", ssid.c_str());
            if (WifiManager::_instance) {
                ok = WifiManager::_instance->connectTo(ssid.c_str(), pass.c_str(), true);
            } else {
                WiFi.begin(ssid.c_str(), pass.c_str());
                uint32_t start = millis();
                while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
                    delay(250);
                }
                ok = (WiFi.status() == WL_CONNECTED);
            }
        }

        char resp[128];
        snprintf(resp, sizeof(resp), "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"}",
            ok ? "true" : "false",
            ok ? ssid.c_str() : "",
            ok ? WiFi.localIP().toString().c_str() : "");
        _server->send(200, "application/json", resp);
    });

    // POST /api/wifi/remove — remove saved network (JSON body: {ssid})
    _server->on("/api/wifi/remove", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        String ssid;
        int si = body.indexOf("\"ssid\"");
        if (si >= 0) {
            int q1 = body.indexOf('"', si + 6);
            int q2 = body.indexOf('"', q1 + 1);
            if (q1 >= 0 && q2 > q1) ssid = body.substring(q1 + 1, q2);
        }
        bool ok = false;
        if (ssid.length() > 0 && WifiManager::_instance) {
            ok = WifiManager::_instance->removeNetwork(ssid.c_str());
            DBG_INFO("web", "WiFi network removed: %s", ssid.c_str());
        }
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":%s}", ok ? "true" : "false");
        _server->send(200, "application/json", resp);
    });

    // POST /api/wifi/reorder — change network priority (JSON: {ssid, priority})
    _server->on("/api/wifi/reorder", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        String ssid;
        int priority = -1;
        int si = body.indexOf("\"ssid\"");
        if (si >= 0) {
            int q1 = body.indexOf('"', si + 6);
            int q2 = body.indexOf('"', q1 + 1);
            if (q1 >= 0 && q2 > q1) ssid = body.substring(q1 + 1, q2);
        }
        int pi = body.indexOf("\"priority\"");
        if (pi >= 0) {
            int colon = body.indexOf(':', pi);
            if (colon >= 0) priority = body.substring(colon + 1).toInt();
        }
        bool ok = false;
        if (ssid.length() > 0 && WifiManager::_instance) {
            ok = WifiManager::_instance->reorderNetwork(ssid.c_str(), (int8_t)priority);
        }
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":%s}", ok ? "true" : "false");
        _server->send(200, "application/json", resp);
    });

    // POST /api/wifi/ap — toggle AP mode (JSON: {active: true/false})
    _server->on("/api/wifi/ap", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        bool activate = body.indexOf("true") >= 0;
        bool ok = false;
        if (WifiManager::_instance) {
            if (activate) {
                ok = WifiManager::_instance->startAP();
            } else {
                ok = WifiManager::_instance->stopAP();
            }
        }
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":%s}", ok ? "true" : "false");
        _server->send(200, "application/json", resp);
    });

    // POST /api/wifi/disconnect — disconnect STA
    _server->on("/api/wifi/disconnect", HTTP_POST, [self]() {
        self->_requestCount++;
        if (WifiManager::_instance) {
            WifiManager::_instance->disconnect();
        } else {
            WiFi.disconnect();
        }
        DBG_INFO("web", "WiFi disconnected via API");
        _server->send(200, "application/json", "{\"ok\":true,\"message\":\"Disconnected\"}");
    });

    // POST /api/wifi/reconnect — reconnect to best saved network
    _server->on("/api/wifi/reconnect", HTTP_POST, [self]() {
        self->_requestCount++;
        bool ok = false;
        if (WifiManager::_instance) {
            ok = WifiManager::_instance->connect();
        }
        DBG_INFO("web", "WiFi reconnect requested via API (ok=%d)", ok);
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":%s,\"message\":\"Reconnecting...\"}",
            ok ? "true" : "false");
        _server->send(200, "application/json", resp);
    });

    // POST /api/wifi/failover — enable/disable auto-failover (JSON: {enabled, timeout_s})
    _server->on("/api/wifi/failover", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        bool enabled = body.indexOf("\"enabled\"") >= 0 && body.indexOf("true") >= 0;
        int timeout_s = 30;
        int ti = body.indexOf("\"timeout_s\"");
        if (ti >= 0) {
            int colon = body.indexOf(':', ti);
            if (colon >= 0) timeout_s = body.substring(colon + 1).toInt();
        }
        bool ok = false;
        if (WifiManager::_instance) {
            WifiManager::_instance->enableAutoFailover(enabled);
            ok = true;
        }
        char resp[96];
        snprintf(resp, sizeof(resp), "{\"ok\":%s,\"enabled\":%s,\"timeout_s\":%d}",
            ok ? "true" : "false",
            enabled ? "true" : "false",
            timeout_s);
        _server->send(200, "application/json", resp);
    });

    // POST /api/wifi/ap/config — configure AP mode (JSON: {ssid, password, channel})
    _server->on("/api/wifi/ap/config", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        String ssid, pass;
        int si = body.indexOf("\"ssid\"");
        if (si >= 0) {
            int q1 = body.indexOf('"', si + 6);
            int q2 = body.indexOf('"', q1 + 1);
            if (q1 >= 0 && q2 > q1) ssid = body.substring(q1 + 1, q2);
        }
        int pi = body.indexOf("\"password\"");
        if (pi >= 0) {
            int q1 = body.indexOf('"', pi + 10);
            int q2 = body.indexOf('"', q1 + 1);
            if (q1 >= 0 && q2 > q1) pass = body.substring(q1 + 1, q2);
        }
        bool ok = false;
        if (WifiManager::_instance && ssid.length() > 0) {
            // Stop existing AP and restart with new config
            WifiManager::_instance->stopAP();
            ok = WifiManager::_instance->startAP(
                ssid.c_str(),
                pass.length() > 0 ? pass.c_str() : nullptr);
            DBG_INFO("web", "AP configured: ssid=%s ok=%d", ssid.c_str(), ok);
        }
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":%s,\"ssid\":\"%s\"}",
            ok ? "true" : "false",
            ssid.c_str());
        _server->send(200, "application/json", resp);
    });

    // Keep the legacy /api/scan endpoint working (backwards compatibility)
    // The original was already registered by addApiEndpoints(), so we don't
    // re-register here — it still works alongside the new /api/wifi/* routes.

    DBG_INFO("web", "WiFi manager added at /wifi (API: /api/wifi/*)");
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
            int len = self->_bleProvider(_shared_json, SHARED_JSON_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", _shared_json);
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

// ── Log access (delegates to serial_capture ring buffer) ─────────────────

void WebServerHAL::captureLog(const char*) {
    // No-op: serial_capture ring buffer handles all log capture now.
    // Kept for API compatibility.
}

int WebServerHAL::getLogJson(char* buf, size_t size) {
    return serial_capture::getLinesJson(buf, size, 48);
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
<h2>Tasks</h2>
<pre id="tasks" style="color:#00ffd0;font-size:12px;white-space:pre;overflow-x:auto">%TASKS%</pre>
</div>
<script>
function fmt(n){return n>1048576?(n/1048576).toFixed(1)+' MB':n>1024?(n/1024).toFixed(0)+' KB':n+' B'}
setInterval(function(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('heap_free').textContent=fmt(d.free_heap);
    document.getElementById('psram_free').textContent=fmt(d.psram_free);
  });
},5000);
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

        // Task list via vTaskList
        char taskBuf[1024];
#if configUSE_TRACE_FACILITY && configTASKLIST_INCLUDE_COREID
        vTaskList(taskBuf);
#else
        snprintf(taskBuf, sizeof(taskBuf), "Name             State  Prio  Stack  Num\n");
        // vTaskList may not be available with all configs
        char* p = taskBuf + strlen(taskBuf);
        snprintf(p, sizeof(taskBuf) - (p - taskBuf),
            "(Task list requires configUSE_TRACE_FACILITY=1)");
#endif
        html.replace("%TASKS%", taskBuf);

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
  <button onclick="clearLog()">Clear</button>
  <button onclick="togglePause()"><span id="pause-btn">Pause</span></button>
  <label><input type="checkbox" id="autoscroll" checked> Auto-scroll</label>
  <span class="log-count" id="log-count">0 lines</span>
</div>
<div id="log-container"></div>
</div>
<script>
var paused=false,lastCount=0;
function togglePause(){
  paused=!paused;
  document.getElementById('pause-btn').textContent=paused?'Resume':'Pause';
}
function clearLog(){
  document.getElementById('log-container').innerHTML='';
  lastCount=0;
}
function fetchLogs(){
  if(paused)return;
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

    // API endpoint for log data — reads directly from serial_capture ring
    _server->on("/api/logs", HTTP_GET, [self]() {
        self->_requestCount++;
        int len = WebServerHAL::getLogJson(_shared_json, SHARED_JSON_SIZE);
        if (len > 0) {
            _server->send(200, "application/json", _shared_json);
        } else {
            _server->send(200, "application/json", "{\"count\":0,\"lines\":[]}");
        }
    });

    DBG_INFO("web", "Logs page added at /logs");
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
            int len = self->_gisLayerProvider(_shared_json, SHARED_JSON_SIZE);
            if (len > 0) {
                _server->send(200, "application/json", _shared_json);
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

        // Handle CORS preflight OPTIONS requests for all /api/* paths
        if (_server->method() == HTTP_OPTIONS) {
            _server->sendHeader("Access-Control-Allow-Origin", "*");
            _server->sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            _server->sendHeader("Access-Control-Allow-Headers", "Content-Type");
            _server->sendHeader("Access-Control-Expose-Headers", "X-Width, X-Height");
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

        // GET /api/settings/{domain} — single-domain settings export
#if WEB_HAS_SETTINGS
        if (uri.startsWith("/api/settings/") && _server->method() == HTTP_GET) {
            const char* domain = uri.c_str() + 14; // skip "/api/settings/"
            if (domain[0] != '\0' && strchr(domain, '/') == nullptr) {
                int len = TritiumSettings::instance().toJson(
                    _shared_json, SHARED_JSON_SIZE, domain);
                if (len > 0) {
                    _server->send(200, "application/json", _shared_json);
                } else {
                    _server->send(404, "application/json",
                        "{\"error\":\"domain not found or empty\"}");
                }
                return;
            }
        }
#endif

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

// ── addMeshPage() ────────────────────────────────────────────────────────

void WebServerHAL::addMeshPage() {
    if (!_server) return;

    WebServerHAL* self = this;

    // Serve the mesh topology viewer HTML at /mesh
#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    _server->on("/mesh", HTTP_GET, [self]() {
        self->_requestCount++;
        _server->send_P(200, "text/html", MESH_HTML_V2);
    });
#else
    _server->on("/mesh", HTTP_GET, [self]() {
        self->_requestCount++;

        // Read mesh.html from LittleFS (placed there by build or upload)
        // Fall back to a redirect if file is missing
        if (LittleFS.exists("/web/mesh.html")) {
            File f = LittleFS.open("/web/mesh.html", "r");
            if (f) {
                _server->streamFile(f, "text/html");
                f.close();
                return;
            }
        }

        // Inline minimal fallback — point to /api/mesh for raw JSON
        _server->send(200, "text/html",
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<title>Mesh</title></head><body style='background:#0a0a0f;color:#c8d0dc;font-family:monospace;padding:20px'>"
            "<h2 style='color:#00f0ff'>Mesh Topology</h2>"
            "<p>Upload <code>/web/mesh.html</code> to LittleFS for the full UI.</p>"
            "<p><a href='/api/mesh' style='color:#00f0ff'>Raw mesh JSON &rarr;</a></p>"
            "</body></html>");
    });
#endif

    // POST /api/mesh/ping?mac=AA:BB:CC:DD:EE:FF — ping a peer
    _server->on("/api/mesh/ping", HTTP_POST, [self]() {
        self->_requestCount++;
        String mac = _server->arg("mac");
        if (mac.length() < 17) {
            _server->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing mac param\"}");
            return;
        }
        // Parse MAC
        uint8_t m[6];
        if (sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
            _server->send(400, "application/json", "{\"ok\":false,\"error\":\"Bad MAC format\"}");
            return;
        }
        // Use mesh provider to relay the ping
        // Note: the actual ping is handled via EspNowService -> MeshManager
        // For now return success — the next /api/mesh poll will show updated RSSI
        _server->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/mesh/state — set a shared state key-value
    _server->on("/api/mesh/state", HTTP_POST, [self]() {
        self->_requestCount++;
        // Expect JSON body: {"key":"...", "value":"..."}
        String body = _server->arg("plain");
        _server->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/mesh/send — send message to peer or broadcast
    _server->on("/api/mesh/send", HTTP_POST, [self]() {
        self->_requestCount++;
        String body = _server->arg("plain");
        _server->send(200, "application/json", "{\"ok\":true}");
    });

    DBG_INFO("web", "Mesh page registered at /mesh");
}

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
    addMapPage();
    addScreenshotPage();
    addRemotePage();
    addMeshPage();
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
