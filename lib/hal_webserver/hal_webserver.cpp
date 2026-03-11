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

#include "tritium_compat.h"
#include <esp_http_server.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_chip_info.h>
#include <mdns.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <dirent.h>
#include <esp_heap_caps.h>

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
#include "core/lv_obj_class_private.h"  // Full lv_obj_class_t definition (has name field)
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
#include "web/settings_html.h"
#endif

// Touch input for remote control injection
#if __has_include("touch_input.h")
#include "touch_input.h"
#include "hal_touch.h"
#define WEB_HAS_TOUCH_INPUT 1
#else
#define WEB_HAS_TOUCH_INPUT 0
#endif

static httpd_handle_t _server = nullptr;
static WebServerHAL* _instance = nullptr;
static bool _captive_portal_active = false;

// Captive portal DNS task — lightweight UDP DNS responder
static TaskHandle_t _dns_task = nullptr;
static void _captive_dns_task(void* param);

// Shared JSON response buffer — allocated in PSRAM during init() (saves 4KB BSS).
static const size_t SHARED_JSON_SIZE = 4096;
static char* _shared_json = nullptr;

// Deferred click: httpd sets the target, main loop processes it.
// This avoids running LVGL event callbacks on the httpd stack (only 16KB).
static volatile lv_obj_t* _pending_click_obj = nullptr;
static volatile bool _pending_click_done = false;
static SemaphoreHandle_t _click_done_sem = nullptr;

// ── Helpers for esp_http_server ─────────────────────────────────────────────

// Send a complete response with content type and body
static esp_err_t _send(httpd_req_t* req, int status, const char* type, const char* body) {
    httpd_resp_set_status(req, status == 200 ? "200 OK" :
                               status == 204 ? "204 No Content" :
                               status == 302 ? "302 Found" :
                               status == 400 ? "400 Bad Request" :
                               status == 404 ? "404 Not Found" :
                               status == 500 ? "500 Internal Server Error" :
                               status == 503 ? "503 Service Unavailable" : "200 OK");
    if (type) httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, body, body ? strlen(body) : 0);
}

static esp_err_t _send_json(httpd_req_t* req, int status, const char* json) {
    return _send(req, status, "application/json", json);
}

// Read full POST body into a buffer (caller must free)
static char* _recv_body(httpd_req_t* req, size_t max_len = 4096) {
    int total = req->content_len;
    if (total <= 0 || (size_t)total > max_len) return nullptr;
    char* buf = (char*)malloc(total + 1);
    if (!buf) return nullptr;
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) { free(buf); return nullptr; }
        received += ret;
    }
    buf[total] = '\0';
    return buf;
}

// Get query parameter value. Returns true if found.
static bool _get_query_param(httpd_req_t* req, const char* key, char* val, size_t val_size) {
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) return false;
    char* qstr = (char*)malloc(qlen + 1);
    if (!qstr) return false;
    if (httpd_req_get_url_query_str(req, qstr, qlen + 1) != ESP_OK) { free(qstr); return false; }
    esp_err_t err = httpd_query_key_value(qstr, key, val, val_size);
    free(qstr);
    return err == ESP_OK;
}

// Simple JSON string extraction: find "key":"value" and copy value into buf
static bool _json_extract_string(const char* json, const char* key, char* buf, size_t buf_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return false;
    const char* colon = strchr(pos + strlen(search), ':');
    if (!colon) return false;
    const char* q1 = strchr(colon + 1, '"');
    if (!q1) return false;
    const char* q2 = strchr(q1 + 1, '"');
    if (!q2) return false;
    size_t len = q2 - q1 - 1;
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, q1 + 1, len);
    buf[len] = '\0';
    return true;
}

// Simple JSON integer extraction: find "key":123
static bool _json_extract_int(const char* json, const char* key, int* out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return false;
    const char* colon = strchr(pos + strlen(search), ':');
    if (!colon) return false;
    *out = atoi(colon + 1);
    return true;
}

// Register a URI handler (macro for brevity)
#define REG(server, uri_str, method_val, handler_fn) do { \
    httpd_uri_t _u = {}; \
    _u.uri = uri_str; \
    _u.method = method_val; \
    _u.handler = handler_fn; \
    _u.user_ctx = _instance; \
    httpd_register_uri_handler(server, &_u); \
} while(0)

// Helper to get WiFi IP as string
static void _get_wifi_ip(char* buf, size_t size) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {};
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(buf, size, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(buf, size, "0.0.0.0");
    }
}

// Helper to get WiFi RSSI
static int _get_wifi_rssi() {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

// Helper to check if WiFi STA is connected
static bool _wifi_connected() {
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

// Helper to get WiFi channel
static int _get_wifi_channel() {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.primary;
    return 0;
}

// Helper to get connected SSID
static void _get_wifi_ssid(char* buf, size_t size) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(buf, size, "%s", (const char*)ap.ssid);
    } else {
        buf[0] = '\0';
    }
}

// Helper to get MAC string
static void _get_mac_str(char* buf, size_t size) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(buf, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Helper to get gateway IP
static void _get_gateway_ip(char* buf, size_t size) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {};
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(buf, size, IPSTR, IP2STR(&ip_info.gw));
    } else {
        snprintf(buf, size, "0.0.0.0");
    }
}

// Helper to get DNS IP
static void _get_dns_ip(char* buf, size_t size) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            snprintf(buf, size, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
            return;
        }
    }
    snprintf(buf, size, "0.0.0.0");
}

// Helper to check if AP is active
static bool _wifi_ap_active() {
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        return (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    }
    return false;
}

// Helper to get AP IP
static void _get_ap_ip(char* buf, size_t size) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info = {};
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(buf, size, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(buf, size, "192.168.4.1");
    }
}

// Helper to get AP SSID
static void _get_ap_ssid(char* buf, size_t size) {
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
        snprintf(buf, size, "%s", (const char*)cfg.ap.ssid);
    } else {
        buf[0] = '\0';
    }
}

// Redirect helper (302)
static esp_err_t _send_redirect(httpd_req_t* req, const char* location) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, "", 0);
}

// ── Dark neon hacker theme ──────────────────────────────────────────────────
// Shared CSS used by all pages — black bg, neon cyan accents, monospace font

static const char THEME_CSS[] = R"rawliteral(
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

static const char NAV_HTML[] = R"rawliteral(
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

static const char DASHBOARD_HTML[] = R"rawliteral(
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

static const char OTA_HTML[] = R"rawliteral(
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

static const char CONFIG_HTML[] = R"rawliteral(
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

static const char FILES_HTML[] = R"rawliteral(
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

// ── Helper: build uptime string ─────────────────────────────────────────────

static void uptimeString(char* buf, size_t size) {
    uint32_t sec = millis() / 1000;
    uint32_t d = sec / 86400; sec %= 86400;
    uint32_t h = sec / 3600;  sec %= 3600;
    uint32_t m = sec / 60;    sec %= 60;
    snprintf(buf, size, "%ud %uh %um %us", d, h, m, sec);
}

// ── Helper: std::string template replace ────────────────────────────────

static void strReplace(std::string& str, const char* key, const char* value) {
    std::string k(key);
    size_t pos = 0;
    while ((pos = str.find(k, pos)) != std::string::npos) {
        str.replace(pos, k.length(), value);
        pos += strlen(value);
    }
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

    // Allocate shared JSON buffer in PSRAM (saves 4KB BSS)
    if (!_shared_json) {
        _shared_json = (char*)heap_caps_malloc(SHARED_JSON_SIZE, MALLOC_CAP_SPIRAM);
        if (!_shared_json) _shared_json = (char*)malloc(SHARED_JSON_SIZE);
    }

    _port = port;
    _instance = this;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 96;
    config.max_open_sockets = 7;
    config.stack_size = 16384;
    config.recv_wait_timeout = 3;    // seconds — short timeout to reclaim sockets quickly
    config.send_wait_timeout = 3;    // seconds — prevents socket exhaustion under load
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&_server, &config) != ESP_OK) {
        DBG_ERROR("web", "Failed to start HTTP server on port %u", port);
        return false;
    }

    _running = true;

    // Cache IP
    _get_wifi_ip(_ip, sizeof(_ip));

    DBG_INFO("web", "Server started on port %u  IP: %s", _port, _ip);
    return true;
}

void WebServerHAL::stop() {
    if (!_running || !_server) return;
    stopCaptivePortal();
    httpd_stop(_server);
    _server = nullptr;
    _running = false;
    DBG_INFO("web", "Server stopped");
}

bool WebServerHAL::isRunning() const { return _running; }

void WebServerHAL::process() {
    // Process any deferred click from the httpd task.
    // This runs on the main loop task where the LVGL stack is large enough
    // to handle event callbacks that create/destroy widgets.
#if WEB_HAS_SHELL
    lv_obj_t* click_target = (lv_obj_t*)_pending_click_obj;
    if (click_target) {
        _pending_click_obj = nullptr;
        // Take LVGL mutex — we're about to modify the widget tree
        if (lvgl_driver::lock(1000)) {
            lv_obj_send_event(click_target, LV_EVENT_PRESSED, nullptr);
            lv_obj_send_event(click_target, LV_EVENT_CLICKED, nullptr);
            lv_obj_send_event(click_target, LV_EVENT_RELEASED, nullptr);
            lvgl_driver::unlock();
        }
        _pending_click_done = true;
        if (_click_done_sem) xSemaphoreGive(_click_done_sem);
    }
#endif
}

// ── Route registration ──────────────────────────────────────────────────────

void WebServerHAL::on(const char* uri, const char* method, WebRequestHandler handler) {
    if (!_server) return;
    // Note: the on() public API is rarely used externally.
    // Internal route registration now uses REG() macro + handler functions.
    // This method is kept for backwards compatibility but is a no-op with
    // esp_http_server since we can't wrap std::function into C function pointers.
    (void)uri; (void)method; (void)handler;
    DBG_WARN("web", "on() API not supported with esp_http_server — use REG()");
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
    // Legacy API — cannot send response without httpd_req_t context.
    // With esp_http_server, responses are sent inside handler functions.
    (void)code; (void)contentType; (void)body;
}

void WebServerHAL::sendJson(int code, const char* json) {
    (void)code; (void)json;
}

// ── Captive Portal ──────────────────────────────────────────────────────

// Captive portal DNS task — responds to all DNS queries with the AP IP
static void _captive_dns_task(void* param) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(nullptr); return; }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    // Set receive timeout so we can check for task deletion
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    struct sockaddr_in client_addr;
    socklen_t client_len;

    while (_captive_portal_active) {
        client_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr*)&client_addr, &client_len);
        if (len < 12) continue;  // too short or timeout

        // Build minimal DNS response: copy query, set response flags, add answer
        uint8_t resp[512];
        if ((size_t)len > sizeof(resp) - 16) continue;
        memcpy(resp, buf, len);
        resp[2] = 0x81; resp[3] = 0x80;  // QR=1, AA=1, RD=1, RA=1
        resp[6] = 0x00; resp[7] = 0x01;  // 1 answer

        // Append answer: pointer to name in query + type A + class IN + TTL + IP
        int pos = len;
        resp[pos++] = 0xC0; resp[pos++] = 0x0C;  // name pointer
        resp[pos++] = 0x00; resp[pos++] = 0x01;  // type A
        resp[pos++] = 0x00; resp[pos++] = 0x01;  // class IN
        resp[pos++] = 0x00; resp[pos++] = 0x00;
        resp[pos++] = 0x00; resp[pos++] = 0x0A;  // TTL 10s
        resp[pos++] = 0x00; resp[pos++] = 0x04;  // data length

        // Get AP IP
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t ip_info = {};
        if (netif) esp_netif_get_ip_info(netif, &ip_info);
        uint32_t ip = ip_info.ip.addr;
        resp[pos++] = (ip >>  0) & 0xFF;
        resp[pos++] = (ip >>  8) & 0xFF;
        resp[pos++] = (ip >> 16) & 0xFF;
        resp[pos++] = (ip >> 24) & 0xFF;

        sendto(sock, resp, pos, 0, (struct sockaddr*)&client_addr, client_len);
    }
    close(sock);
    vTaskDelete(nullptr);
}

// Captive portal redirect handler for detection endpoints
static esp_err_t _captive_redirect_handler(httpd_req_t* req) {
    return _send_redirect(req, "http://192.168.4.1/wifi");
}

void WebServerHAL::startCaptivePortal() {
    if (!_server) return;

    _captive_portal_active = true;

    // Start DNS responder task
    xTaskCreate(_captive_dns_task, "dns_captive", 3072, nullptr, 2, &_dns_task);

    // Android/iOS captive portal detection endpoints
    REG(_server, "/generate_204",       HTTP_GET, _captive_redirect_handler);
    REG(_server, "/hotspot-detect.html", HTTP_GET, _captive_redirect_handler);
    REG(_server, "/connecttest.txt",    HTTP_GET, _captive_redirect_handler);

    DBG_INFO("web", "Captive portal started — all DNS queries redirect to AP IP");
}

void WebServerHAL::stopCaptivePortal() {
    if (_captive_portal_active) {
        _captive_portal_active = false;
        // DNS task will exit on next timeout iteration
        if (_dns_task) {
            vTaskDelay(pdMS_TO_TICKS(1500));  // wait for task to exit
            _dns_task = nullptr;
        }
        DBG_INFO("web", "Captive portal stopped");
    }
}

// ── mDNS ────────────────────────────────────────────────────────────────────

bool WebServerHAL::startMDNS(const char* hostname) {
    if (mdns_init() != ESP_OK) {
        DBG_ERROR("web", "mDNS init failed");
        return false;
    }
    mdns_hostname_set(hostname);
    mdns_instance_name_set("Tritium Node");
    mdns_service_add(nullptr, "_http", "_tcp", _port, nullptr, 0);
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

// ── Dashboard handler ─────────────────────────────────────────────────────

#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
static esp_err_t _dashboard_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, DASHBOARD_HTML_V2, strlen(DASHBOARD_HTML_V2));
}
static esp_err_t _terminal_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, TERMINAL_HTML, strlen(TERMINAL_HTML));
}

static esp_err_t _settings_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, SETTINGS_HTML, strlen(SETTINGS_HTML));
}
#else
static esp_err_t _dashboard_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();

    std::string html(DASHBOARD_HTML);
    strReplace(html, "%THEME%",     THEME_CSS);
    strReplace(html, "%NAV%",       NAV_HTML);
    strReplace(html, "%BOARD%",     "ESP32-S3");

    char macStr[18];
    _get_mac_str(macStr, sizeof(macStr));
    strReplace(html, "%MAC%", macStr);

    char ipStr[16];
    _get_wifi_ip(ipStr, sizeof(ipStr));
    strReplace(html, "%IP%", ipStr);

    char uptBuf[32];
    uptimeString(uptBuf, sizeof(uptBuf));
    strReplace(html, "%UPTIME%", uptBuf);

    char numBuf[16];
    snprintf(numBuf, sizeof(numBuf), "%lu", (unsigned long)esp_get_free_heap_size());
    strReplace(html, "%HEAP%", numBuf);
    snprintf(numBuf, sizeof(numBuf), "%lu", (unsigned long)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    strReplace(html, "%PSRAM_TOTAL%", numBuf);
    snprintf(numBuf, sizeof(numBuf), "%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    strReplace(html, "%PSRAM_FREE%", numBuf);

    int rssi = _get_wifi_rssi();
    snprintf(numBuf, sizeof(numBuf), "%d", rssi);
    strReplace(html, "%RSSI%", numBuf);
    snprintf(numBuf, sizeof(numBuf), "%d", rssiToPercent(rssi));
    strReplace(html, "%RSSI_PCT%", numBuf);
    snprintf(numBuf, sizeof(numBuf), "%lu", (unsigned long)_instance->_requestCount);
    strReplace(html, "%REQCOUNT%", numBuf);

    // Build capabilities badges
    std::string caps;
    auto addBadge = [&](const char* name, bool active) {
        caps += "<span class=\"cap-badge ";
        caps += active ? "active" : "inactive";
        caps += "\"><span class=\"status-dot ";
        caps += active ? "on" : "off";
        caps += "\"></span>";
        caps += name;
        caps += "</span>";
    };

    addBadge("WiFi", _wifi_connected());
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
    strReplace(html, "%CAPABILITIES%", caps.c_str());

#if defined(ENABLE_BLE_SCANNER)
    strReplace(html, "%BLE_DISPLAY%", "block");
    strReplace(html, "%BLE_STATUS%", "Checking...");
    strReplace(html, "%BLE_COUNT%", "...");
#else
    strReplace(html, "%BLE_DISPLAY%", "none");
    strReplace(html, "%BLE_STATUS%", "");
    strReplace(html, "%BLE_COUNT%", "");
#endif

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html.c_str(), html.length());
}
#endif

void WebServerHAL::addDashboard() {
    if (!_server) return;

    REG(_server, "/", HTTP_GET, _dashboard_handler);

#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    REG(_server, "/terminal", HTTP_GET, _terminal_handler);
    REG(_server, "/settings", HTTP_GET, _settings_page_handler);
#endif

    DBG_INFO("web", "Dashboard page added at /");
}

// ── addOtaPage() ────────────────────────────────────────────────────────────

#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
static esp_err_t _ota_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, OTA_HTML_V2, strlen(OTA_HTML_V2));
}
#else
static esp_err_t _ota_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    std::string html(OTA_HTML);
    strReplace(html, "%THEME%", THEME_CSS);
    strReplace(html, "%NAV%",   NAV_HTML);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html.c_str(), html.length());
}
#endif

// OTA firmware upload via POST /update — receives raw binary body
static esp_err_t _ota_upload_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();

    const esp_partition_t* update_part = esp_ota_get_next_update_partition(nullptr);
    if (!update_part) {
        return _send(req, 500, "text/plain", "No OTA partition available");
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        DBG_ERROR("web", "OTA begin failed: %s", esp_err_to_name(err));
        return _send(req, 500, "text/plain", "OTA begin failed");
    }

    char buf[1024];
    int total = req->content_len;
    int received = 0;
    bool ok = true;

    DBG_INFO("web", "OTA upload start: %d bytes", total);

    while (received < total) {
        int ret = httpd_req_recv(req, buf, sizeof(buf));
        if (ret <= 0) {
            DBG_ERROR("web", "OTA recv failed at %d/%d", received, total);
            ok = false;
            break;
        }
        err = esp_ota_write(ota_handle, buf, ret);
        if (err != ESP_OK) {
            DBG_ERROR("web", "OTA write failed: %s", esp_err_to_name(err));
            ok = false;
            break;
        }
        received += ret;
    }

    if (ok) {
        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            DBG_ERROR("web", "OTA end failed: %s", esp_err_to_name(err));
            return _send(req, 500, "text/plain", "OTA finalize failed");
        }
        err = esp_ota_set_boot_partition(update_part);
        if (err != ESP_OK) {
            DBG_ERROR("web", "OTA set boot partition failed: %s", esp_err_to_name(err));
            return _send(req, 500, "text/plain", "OTA set boot failed");
        }
        DBG_INFO("web", "OTA success: %d bytes", received);
        _send(req, 200, "text/plain", "Update OK — rebooting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;
    } else {
        esp_ota_abort(ota_handle);
        return _send(req, 500, "text/plain", "Update FAILED");
    }
}

void WebServerHAL::addOtaPage() {
    if (!_server) return;

    REG(_server, "/update", HTTP_GET,  _ota_page_handler);
    REG(_server, "/update", HTTP_POST, _ota_upload_handler);
    REG(_server, "/ota",    HTTP_GET,  _ota_page_handler);

    DBG_INFO("web", "OTA page added at /update and /ota");
}

// ── addConfigEditor() ───────────────────────────────────────────────────────

// ── Config editor handlers ───────────────────────────────────────────────

static esp_err_t _config_get_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();

    // Read config from POSIX VFS
    char config[2048] = "{}";
    FILE* f = fopen("/littlefs/config.json", "r");
    if (f) {
        size_t n = fread(config, 1, sizeof(config) - 1, f);
        config[n] = '\0';
        fclose(f);
    }

    std::string html(CONFIG_HTML);
    strReplace(html, "%THEME%",  THEME_CSS);
    strReplace(html, "%NAV%",    NAV_HTML);
    strReplace(html, "%CONFIG%", config);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html.c_str(), html.length());
}

static esp_err_t _config_post_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();

    char* body = _recv_body(req, 4096);
    if (body) {
        // Extract "json=" form field value (URL-encoded form data)
        // For simplicity, write entire body as config if it looks like JSON
        const char* json = body;
        // Check for form-encoded: json=...
        if (strncmp(body, "json=", 5) == 0) {
            json = body + 5;
        }
        FILE* f = fopen("/littlefs/config.json", "w");
        if (f) {
            fputs(json, f);
            fclose(f);
            DBG_INFO("web", "Config saved (%d bytes)", (int)strlen(json));
        }
        free(body);
    }
    return _send_redirect(req, "/config");
}

void WebServerHAL::addConfigEditor() {
    if (!_server) return;

    REG(_server, "/config", HTTP_GET,  _config_get_handler);
    REG(_server, "/config", HTTP_POST, _config_post_handler);

    DBG_INFO("web", "Config editor added at /config");
}

// ── addFileManager() ────────────────────────────────────────────────────────

// ── File manager handlers ────────────────────────────────────────────────

#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
static esp_err_t _files_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, FILES_HTML_V2, strlen(FILES_HTML_V2));
}
#else
static esp_err_t _files_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();

    std::string fileList;
    DIR* dir = opendir("/littlefs");
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_type == DT_REG) {
                char fullpath[128];
                snprintf(fullpath, sizeof(fullpath), "/littlefs/%s", ent->d_name);
                struct stat st;
                size_t fsize = 0;
                if (stat(fullpath, &st) == 0) fsize = st.st_size;

                fileList += "<tr><td>";
                fileList += ent->d_name;
                fileList += "</td><td>";
                char sizebuf[16];
                snprintf(sizebuf, sizeof(sizebuf), "%u", (unsigned)fsize);
                fileList += sizebuf;
                fileList += " B</td><td>";
                fileList += "<form method='POST' action='/files/delete' style='display:inline'>";
                fileList += "<input type='hidden' name='path' value='/";
                fileList += ent->d_name;
                fileList += "'>";
                fileList += "<button class='danger' type='submit'>Delete</button>";
                fileList += "</form></td></tr>";
            }
        }
        closedir(dir);
    }
    if (fileList.empty()) {
        fileList = "<tr><td colspan='3' style='color:#666'>No files</td></tr>";
    }

    std::string html(FILES_HTML);
    strReplace(html, "%THEME%",    THEME_CSS);
    strReplace(html, "%NAV%",      NAV_HTML);
    strReplace(html, "%FILELIST%", fileList.c_str());
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html.c_str(), html.length());
}
#endif

static esp_err_t _files_upload_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();

    // For simplicity, receive the entire body and write to a file.
    // The filename must be provided as a query parameter: ?name=foo.txt
    char fname[64] = "upload.bin";
    _get_query_param(req, "name", fname, sizeof(fname));

    char fullpath[128];
    snprintf(fullpath, sizeof(fullpath), "/littlefs/%s", fname);

    FILE* f = fopen(fullpath, "w");
    if (!f) {
        return _send(req, 500, "text/plain", "Failed to create file");
    }

    char buf[1024];
    int total = req->content_len;
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf, sizeof(buf));
        if (ret <= 0) break;
        fwrite(buf, 1, ret, f);
        received += ret;
    }
    fclose(f);
    DBG_INFO("web", "File uploaded: %s (%d bytes)", fname, received);
    return _send_redirect(req, "/files");
}

static esp_err_t _files_delete_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();

    char* body = _recv_body(req, 512);
    if (body) {
        // Extract path= from form-encoded body
        const char* p = strstr(body, "path=");
        if (p) {
            p += 5;
            char path[128];
            // URL-decode the path (basic: just copy until & or end)
            size_t i = 0;
            while (*p && *p != '&' && i < sizeof(path) - 1) {
                if (*p == '%' && p[1] && p[2]) {
                    char hex[3] = { p[1], p[2], 0 };
                    path[i++] = (char)strtol(hex, nullptr, 16);
                    p += 3;
                } else {
                    path[i++] = *p++;
                }
            }
            path[i] = '\0';

            char fullpath[256];
            snprintf(fullpath, sizeof(fullpath), "/littlefs%s", path);
            if (unlink(fullpath) == 0) {
                DBG_INFO("web", "File deleted: %s", fullpath);
            } else {
                DBG_WARN("web", "File delete failed: %s", fullpath);
            }
        }
        free(body);
    }
    return _send_redirect(req, "/files");
}

// Upload to SD card: POST /api/fs/upload?path=/data/map.mbtiles
// Writes to /sdcard/<path>. Creates parent directories as needed.
// Streams the body directly to disk — supports large files.
static esp_err_t _api_fs_upload_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();

    char rel_path[128] = "";
    _get_query_param(req, "path", rel_path, sizeof(rel_path));
    if (rel_path[0] == '\0') {
        return _send_json(req, 400, "{\"ok\":false,\"error\":\"missing ?path= parameter\"}");
    }

    char fullpath[192];
    snprintf(fullpath, sizeof(fullpath), "/sdcard%s%s",
             rel_path[0] == '/' ? "" : "/", rel_path);

    // Create parent directories
    char dir[192];
    strncpy(dir, fullpath, sizeof(dir));
    for (char* p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
    }

    // Remove existing file first (FAT32 can struggle overwriting large files)
    remove(fullpath);

    FILE* f = fopen(fullpath, "w");
    if (!f) {
        char err[256];
        snprintf(err, sizeof(err),
                 "{\"ok\":false,\"error\":\"Cannot create %s (errno=%d)\"}", fullpath, errno);
        return _send_json(req, 500, err);
    }

    char buf[4096];
    int total = req->content_len;
    int received = 0;
    while (received < total) {
        int to_read = (total - received) < (int)sizeof(buf) ? (total - received) : (int)sizeof(buf);
        int ret = httpd_req_recv(req, buf, to_read);
        if (ret <= 0) break;
        fwrite(buf, 1, ret, f);
        received += ret;
    }
    fclose(f);

    DBG_INFO("web", "SD upload: %s (%d bytes)", fullpath, received);

    char json[256];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"path\":\"%s\",\"bytes\":%d}", fullpath, received);
    return _send_json(req, 200, json);
}

// Delete SD card file: DELETE /api/fs/delete?path=/data/map.mbtiles
static esp_err_t _api_fs_delete_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();

    char rel_path[128] = "";
    _get_query_param(req, "path", rel_path, sizeof(rel_path));
    if (rel_path[0] == '\0') {
        return _send_json(req, 400, "{\"ok\":false,\"error\":\"missing ?path= parameter\"}");
    }

    char fullpath[192];
    snprintf(fullpath, sizeof(fullpath), "/sdcard%s%s",
             rel_path[0] == '/' ? "" : "/", rel_path);

    if (remove(fullpath) == 0) {
        char json[256];
        snprintf(json, sizeof(json), "{\"ok\":true,\"deleted\":\"%s\"}", fullpath);
        return _send_json(req, 200, json);
    }
    char err[256];
    snprintf(err, sizeof(err),
             "{\"ok\":false,\"error\":\"Cannot delete %s (errno=%d)\"}", fullpath, errno);
    return _send_json(req, 500, err);
}

void WebServerHAL::addFileManager() {
    if (!_server) return;

    REG(_server, "/files",          HTTP_GET,  _files_page_handler);
    REG(_server, "/files/upload",   HTTP_POST, _files_upload_handler);
    REG(_server, "/files/delete",   HTTP_POST, _files_delete_handler);
    REG(_server, "/api/fs/upload",  HTTP_POST, _api_fs_upload_handler);
    REG(_server, "/api/fs/delete",  HTTP_POST, _api_fs_delete_handler);

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

// ── API handler functions ────────────────────────────────────────────────

static esp_err_t _api_status_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char ipStr[16];
    _get_wifi_ip(ipStr, sizeof(ipStr));
    char buf[768];
    int pos = snprintf(buf, sizeof(buf),
        "{\"uptime_s\":%lu,\"free_heap\":%lu,\"psram_free\":%lu,"
        "\"rssi\":%d,\"ip\":\"%s\",\"requests\":%lu",
        (unsigned long)(millis() / 1000),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        _get_wifi_rssi(),
        ipStr,
        (unsigned long)_instance->_requestCount);
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
    return _send_json(req, 200, buf);
}

static esp_err_t _api_board_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char buf[384];
    snprintf(buf, sizeof(buf),
        "{\"board\":\"ESP32-S3\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"flash_size\":%lu,\"psram_size\":%lu,\"cpu_freq\":%d,"
        "\"sdk\":\"%s\"}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        (unsigned long)(4 * 1024 * 1024),  // TODO: read from spi_flash
        (unsigned long)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        esp_get_idf_version());
    return _send_json(req, 200, buf);
}

#if WEB_HAS_FINGERPRINT
static esp_err_t _api_fingerprint_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    const board_fingerprint_t* fp = board_fingerprint_get();
    if (!fp) {
        return _send_json(req, 503, "{\"error\":\"not scanned\"}");
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
    return _send_json(req, 200, _shared_json);
}
#endif

static esp_err_t _api_reboot_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    _send_json(req, 200, "{\"status\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t _api_scan_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    // Use esp_wifi_scan_start for a blocking scan
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = true;
    esp_wifi_scan_start(&scan_cfg, true);
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t* records = nullptr;
    if (n > 0) {
        records = (wifi_ap_record_t*)malloc(n * sizeof(wifi_ap_record_t));
        if (records) esp_wifi_scan_get_ap_records(&n, records);
    }
    int pos = snprintf(_shared_json, SHARED_JSON_SIZE, "{\"count\":%u,\"networks\":[", n);
    for (uint16_t i = 0; i < n && records && pos < (int)SHARED_JSON_SIZE - 120; i++) {
        if (i > 0) _shared_json[pos++] = ',';
        pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"encryption\":%d}",
            (const char*)records[i].ssid, records[i].rssi,
            records[i].primary, records[i].authmode);
    }
    pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "]}");
    if (records) free(records);
    return _send_json(req, 200, _shared_json);
}

static esp_err_t _api_node_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char ipStr[16];
    _get_wifi_ip(ipStr, sizeof(ipStr));

    int pos = snprintf(_shared_json, SHARED_JSON_SIZE,
        "{\"tritium\":true,\"version\":\"1.0\","
        "\"device_id\":\"%02X%02X%02X%02X%02X%02X\","
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"ip\":\"%s\",\"port\":%u,"
        "\"uptime_s\":%lu,"
        "\"free_heap\":%lu,\"psram_free\":%lu,"
        "\"flash_size\":%lu,\"psram_size\":%lu,"
        "\"cpu_freq\":%d,"
        "\"rssi\":%d,"
        "\"sdk\":\"%s\","
        "\"requests\":%lu,",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        ipStr,
        _instance->_port,
        (unsigned long)(millis() / 1000),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long)(4 * 1024 * 1024),
        (unsigned long)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        _get_wifi_rssi(),
        esp_get_idf_version(),
        (unsigned long)_instance->_requestCount);

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
    return _send_json(req, 200, _shared_json);
}

static esp_err_t _api_mesh_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    if (_instance->_meshProvider) {
        int len = _instance->_meshProvider(_shared_json, SHARED_JSON_SIZE);
        if (len > 0) return _send_json(req, 200, _shared_json);
    }
    return _send_json(req, 200, "{\"enabled\":false,\"message\":\"ESP-NOW mesh not available\"}");
}

static esp_err_t _api_diag_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    if (_instance->_diagProvider) {
        int len = _instance->_diagProvider(_shared_json, SHARED_JSON_SIZE);
        if (len > 0) return _send_json(req, 200, _shared_json);
    }
    return _send_json(req, 200, "{\"enabled\":false,\"message\":\"Diagnostics not available\"}");
}

static esp_err_t _api_diag_health_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    if (_instance->_diagHealthProvider) {
        int len = _instance->_diagHealthProvider(_shared_json, SHARED_JSON_SIZE);
        if (len > 0) return _send_json(req, 200, _shared_json);
    }
    return _send_json(req, 200, "{\"enabled\":false}");
}

static esp_err_t _api_diag_events_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    if (_instance->_diagEventsProvider) {
        int len = _instance->_diagEventsProvider(_shared_json, SHARED_JSON_SIZE);
        if (len > 0) return _send_json(req, 200, _shared_json);
    }
    return _send_json(req, 200, "{\"count\":0,\"events\":[]}");
}

static esp_err_t _api_diag_anomalies_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    if (_instance->_diagAnomaliesProvider) {
        int len = _instance->_diagAnomaliesProvider(_shared_json, SHARED_JSON_SIZE);
        if (len > 0) return _send_json(req, 200, _shared_json);
    }
    return _send_json(req, 200, "{\"count\":0,\"anomalies\":[]}");
}

#if WEB_HAS_DIAGLOG
static esp_err_t _api_diaglog_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char val[16];
    int offset = 0, count = 50;
    if (_get_query_param(req, "offset", val, sizeof(val))) offset = atoi(val);
    if (_get_query_param(req, "count", val, sizeof(val))) {
        count = atoi(val);
        if (count < 1) count = 1;
        if (count > 200) count = 200;
    }
    int len = diaglog_get_json(_shared_json, SHARED_JSON_SIZE, offset, count);
    if (len > 0) return _send_json(req, 200, _shared_json);
    return _send_json(req, 200, "{\"boot_count\":0,\"total\":0,\"returned\":0,\"events\":[]}");
}

static esp_err_t _api_diaglog_clear_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    diaglog_clear();
    return _send_json(req, 200, "{\"cleared\":true}");
}
#endif

static esp_err_t _api_logs_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    int len = WebServerHAL::getLogJson(_shared_json, SHARED_JSON_SIZE);
    if (len > 0) return _send_json(req, 200, _shared_json);
    return _send_json(req, 200, "{\"lines\":[]}");
}


// ── Inline HTML pages (fallback when V2 pages not available) ────────────

static const char SCREENSHOT_HTML[] = R"rawliteral(
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

static const char WIFI_HTML[] = R"rawliteral(
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

static const char BLE_HTML[] = R"rawliteral(
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

static const char COMMISSION_HTML[] = R"rawliteral(
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

static const char SYSTEM_HTML[] = R"rawliteral(
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

static const char LOGS_HTML[] = R"rawliteral(
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

static const char ERROR_HTML[] = R"rawliteral(
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

static const char MAP_HTML[] = R"rawliteral(
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

// ── Remaining API handlers (screenshot, remote, settings, etc.) ──────────

static esp_err_t _api_screenshot_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    if (!_instance->_screenshotProvider) {
        return _send(req, 503, "text/plain", "No screenshot provider");
    }
    int w = 0, h = 0;
    uint16_t* fb = _instance->_screenshotProvider(w, h);
    if (!fb || w == 0 || h == 0) {
        return _send(req, 503, "text/plain", "No framebuffer available");
    }

    // BMP file: 54-byte header + row data (rows padded to 4-byte boundary)
    int row_bytes = w * 3;
    int row_pad = (4 - (row_bytes % 4)) % 4;
    int padded_row = row_bytes + row_pad;
    uint32_t pixel_data_size = (uint32_t)padded_row * h;
    uint32_t file_size = 54 + pixel_data_size;

    // Build BMP header
    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size & 0xFF; hdr[3] = (file_size >> 8) & 0xFF;
    hdr[4] = (file_size >> 16) & 0xFF; hdr[5] = (file_size >> 24) & 0xFF;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
    hdr[20] = (w >> 16) & 0xFF; hdr[21] = (w >> 24) & 0xFF;
    int32_t neg_h = -h;
    hdr[22] = neg_h & 0xFF; hdr[23] = (neg_h >> 8) & 0xFF;
    hdr[24] = (neg_h >> 16) & 0xFF; hdr[25] = (neg_h >> 24) & 0xFF;
    hdr[26] = 1; hdr[28] = 24;
    hdr[34] = pixel_data_size & 0xFF; hdr[35] = (pixel_data_size >> 8) & 0xFF;
    hdr[36] = (pixel_data_size >> 16) & 0xFF; hdr[37] = (pixel_data_size >> 24) & 0xFF;

    // Set response headers
    char len_str[16];
    snprintf(len_str, sizeof(len_str), "%lu", (unsigned long)file_size);
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Length", len_str);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
#if WEB_HAS_DISPLAY_HEALTH
    // Display headers sent as custom X- headers
#endif

    // Send BMP header
    httpd_resp_send_chunk(req, (const char*)hdr, 54);

    // Convert and stream row by row
    uint8_t* row_buf = (uint8_t*)malloc(padded_row);
    if (!row_buf) {
        return httpd_resp_send_chunk(req, nullptr, 0);
    }

    bool needs_swap = true;
#if defined(BOARD_TOUCH_LCD_43C_BOX)
    needs_swap = false;
#endif

    for (int y = 0; y < h; y++) {
        uint16_t* row_src = &fb[y * w];
        for (int x = 0; x < w; x++) {
            uint16_t px = row_src[x];
            if (needs_swap) px = (px >> 8) | (px << 8);
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5) & 0x3F;
            uint8_t b5 = px & 0x1F;
            row_buf[x * 3 + 0] = (b5 << 3) | (b5 >> 2);
            row_buf[x * 3 + 1] = (g6 << 2) | (g6 >> 4);
            row_buf[x * 3 + 2] = (r5 << 3) | (r5 >> 2);
        }
        for (int p = 0; p < row_pad; p++) row_buf[row_bytes + p] = 0;
        httpd_resp_send_chunk(req, (const char*)row_buf, padded_row);
    }
    free(row_buf);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t _api_screenshot_json_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    int w = 0, h = 0;
    uint16_t* fb = nullptr;
    if (_instance->_screenshotProvider) fb = _instance->_screenshotProvider(w, h);
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
    return _send_json(req, 200, json);
}

// ── Remote control API handlers ─────────────────────────────────────────

#if WEB_HAS_TOUCH_INPUT
static esp_err_t _api_remote_touch_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"error\":\"empty body\"}");

    int x = -1, y = -1;
    _json_extract_int(body, "x", &x);
    _json_extract_int(body, "y", &y);
    if (x < 0 || y < 0) { free(body); return _send_json(req, 400, "{\"error\":\"missing x or y\"}"); }

    bool pressed = true;
    // Check for "pressed":false
    if (strstr(body, "\"pressed\"") && strstr(body, "false")) pressed = false;

    free(body);
    touch_input::inject((uint16_t)x, (uint16_t)y, pressed);
    DBG_INFO("REMOTE", "Touch inject x=%d y=%d pressed=%d", x, y, pressed);
    return _send_json(req, 200, "{\"ok\":true}");
}

static esp_err_t _api_remote_tap_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"error\":\"empty body\"}");

    int x = -1, y = -1;
    _json_extract_int(body, "x", &x);
    _json_extract_int(body, "y", &y);
    free(body);
    if (x < 0 || y < 0) return _send_json(req, 400, "{\"error\":\"missing x or y\"}");

    touch_input::inject((uint16_t)x, (uint16_t)y, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    touch_input::inject((uint16_t)x, (uint16_t)y, false);
    DBG_INFO("REMOTE", "Tap x=%d y=%d", x, y);
    return _send_json(req, 200, "{\"ok\":true}");
}

static esp_err_t _api_remote_swipe_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"error\":\"empty body\"}");

    int x1 = -1, y1 = -1, x2 = -1, y2 = -1, dur = 300;
    _json_extract_int(body, "x1", &x1);
    _json_extract_int(body, "y1", &y1);
    _json_extract_int(body, "x2", &x2);
    _json_extract_int(body, "y2", &y2);
    _json_extract_int(body, "duration_ms", &dur);
    free(body);

    if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0)
        return _send_json(req, 400, "{\"error\":\"missing x1/y1/x2/y2\"}");
    if (dur <= 0) dur = 300;
    if (dur > 2000) dur = 2000;

    int steps = dur / 20;
    if (steps < 2) steps = 2;
    if (steps > 100) steps = 100;

    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        uint16_t cx = (uint16_t)(x1 + t * (x2 - x1));
        uint16_t cy = (uint16_t)(y1 + t * (y2 - y1));
        touch_input::inject(cx, cy, true);
        if (i < steps) vTaskDelay(pdMS_TO_TICKS(dur / steps));
    }
    touch_input::inject((uint16_t)x2, (uint16_t)y2, false);
    DBG_INFO("REMOTE", "Swipe (%d,%d)->(%d,%d) %dms", x1, y1, x2, y2, dur);
    return _send_json(req, 200, "{\"ok\":true}");
}
#endif // WEB_HAS_TOUCH_INPUT

static esp_err_t _api_remote_screenshot_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    if (!_instance->_screenshotProvider) {
        return _send(req, 503, "text/plain", "No screenshot provider");
    }
    int w = 0, h = 0;
    uint16_t* fb = _instance->_screenshotProvider(w, h);
    if (!fb || w == 0 || h == 0) {
        return _send(req, 503, "text/plain", "No framebuffer available");
    }

    size_t data_size = (size_t)w * h * 2;
    bool needs_swap = true;
#if defined(BOARD_TOUCH_LCD_43C_BOX)
    needs_swap = false;
#endif

    char w_str[8], h_str[8], len_str[16];
    snprintf(w_str, sizeof(w_str), "%d", w);
    snprintf(h_str, sizeof(h_str), "%d", h);
    snprintf(len_str, sizeof(len_str), "%u", (unsigned)data_size);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Length", len_str);
    httpd_resp_set_hdr(req, "X-Width", w_str);
    httpd_resp_set_hdr(req, "X-Height", h_str);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-Width, X-Height");

    if (needs_swap) {
        const int row_words = w;
        uint16_t* row_buf = (uint16_t*)malloc(row_words * 2);
        if (!row_buf) {
            httpd_resp_send_chunk(req, (const char*)fb, data_size);
        } else {
            for (int y = 0; y < h; y++) {
                uint16_t* src = &fb[y * w];
                for (int x = 0; x < w; x++) {
                    uint16_t px = src[x];
                    row_buf[x] = (px >> 8) | (px << 8);
                }
                httpd_resp_send_chunk(req, (const char*)row_buf, row_words * 2);
            }
            free(row_buf);
        }
    } else {
        httpd_resp_send_chunk(req, (const char*)fb, data_size);
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t _api_remote_info_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    int w = 0, h = 0;
    bool has_fb = false;
    if (_instance->_screenshotProvider) {
        uint16_t* fb = _instance->_screenshotProvider(w, h);
        has_fb = (fb != nullptr && w > 0 && h > 0);
    }
    bool has_touch = false, has_shell = false;
#if WEB_HAS_TOUCH_INPUT
    has_touch = true;
#endif
#if defined(ENABLE_SHELL)
    has_shell = true;
#endif
    char ipStr[16];
    _get_wifi_ip(ipStr, sizeof(ipStr));
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
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long)(millis() / 1000),
        ipStr, _get_wifi_rssi());
#if WEB_HAS_TOUCH_INPUT
    pos += snprintf(json + pos, sizeof(json) - pos,
        ",\"remote_active\":%s,\"last_touch_ms\":%lu",
        touch_input::isRemoteActive() ? "true" : "false",
        (unsigned long)touch_input::lastActivityMs());
#endif
    pos += snprintf(json + pos, sizeof(json) - pos, "}");
    return _send_json(req, 200, json);
}

// ── Settings REST API handlers ──────────────────────────────────────────

#if WEB_HAS_SETTINGS
static esp_err_t _api_settings_get_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    int len = TritiumSettings::instance().toJson(_shared_json, SHARED_JSON_SIZE);
    if (len > 0) return _send_json(req, 200, _shared_json);
    return _send_json(req, 500, "{\"error\":\"failed to export settings\"}");
}

static esp_err_t _api_settings_put_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"error\":\"empty body\"}");
    bool ok = TritiumSettings::instance().fromJson(body);
    free(body);
    if (ok) return _send_json(req, 200, "{\"ok\":true}");
    return _send_json(req, 400, "{\"ok\":false,\"error\":\"failed to import settings\"}");
}

static esp_err_t _api_settings_reset_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    const char* domain = nullptr;
    char domainBuf[32] = {0};
    if (body) {
        _json_extract_string(body, "domain", domainBuf, sizeof(domainBuf));
        if (domainBuf[0]) domain = domainBuf;
        free(body);
    }
    bool ok = TritiumSettings::instance().factoryReset(domain);
    char resp[64];
    if (domain) {
        snprintf(resp, sizeof(resp), "{\"ok\":%s,\"domain\":\"%s\"}", ok ? "true" : "false", domain);
    } else {
        snprintf(resp, sizeof(resp), "{\"ok\":%s}", ok ? "true" : "false");
    }
    return _send_json(req, 200, resp);
}
#endif

// ── Polling fallback handlers ───────────────────────────────────────────

#if defined(ENABLE_SETTINGS) || defined(ENABLE_OS_EVENTS)

static esp_err_t _api_events_poll_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    uint32_t since = 0;
    char val[16];
    if (_get_query_param(req, "since", val, sizeof(val))) since = (uint32_t)atol(val);
    int len = getEventPollJson(_shared_json, SHARED_JSON_SIZE, since);
    if (len > 0) return _send_json(req, 200, _shared_json);
    return _send_json(req, 200, "{\"seq\":0,\"events\":[]}");
}

static esp_err_t _api_serial_poll_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    int max_lines = 50;
    char val[16];
    if (_get_query_param(req, "lines", val, sizeof(val))) {
        max_lines = atoi(val);
        if (max_lines < 1) max_lines = 1;
        if (max_lines > 48) max_lines = 48;
    }
    int len = serial_capture::getLinesJson(_shared_json, SHARED_JSON_SIZE, max_lines);
    if (len > 0) return _send_json(req, 200, _shared_json);
    return _send_json(req, 200, "{\"count\":0,\"lines\":[]}");
}

static esp_err_t _api_serial_send_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"ok\":false,\"error\":\"empty body\"}");

    char cmd[128] = {0};
    if (!_json_extract_string(body, "cmd", cmd, sizeof(cmd)) || cmd[0] == '\0') {
        free(body);
        return _send_json(req, 400, "{\"ok\":false,\"error\":\"missing cmd field\"}");
    }
    free(body);
    serial_capture::injectCommand(cmd);
    return _send_json(req, 200, "{\"ok\":true}");
}

#endif // ENABLE_SETTINGS || ENABLE_OS_EVENTS

// ── Debug API handlers ──────────────────────────────────────────────────

#if WEB_HAS_TOUCH_INPUT
static esp_err_t _api_debug_touch_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
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
    return _send_json(req, 200, _shared_json);
}

static esp_err_t _api_debug_gt911_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    extern TouchHAL touch;
    touch.dumpDiag(_shared_json, SHARED_JSON_SIZE);
    return _send_json(req, 200, _shared_json);
}
#endif

#if WEB_HAS_SHELL
static esp_err_t _api_debug_lvgl_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
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
    return _send_json(req, 200, _shared_json);
}

static esp_err_t _api_shell_apps_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    int count = tritium_shell::getAppCount();
    int active = tritium_shell::getActiveApp();
    int pos = snprintf(_shared_json, SHARED_JSON_SIZE,
        "{\"count\":%d,\"active\":%d,\"apps\":[", count, active);
    for (int i = 0; i < count && pos < (int)SHARED_JSON_SIZE - 128; i++) {
        const tritium_shell::AppDescriptor* app = tritium_shell::getApp(i);
        if (!app) continue;
        if (i > 0) pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, ",");
        pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos,
            "{\"index\":%d,\"name\":\"%s\",\"description\":\"%s\","
            "\"system\":%s,\"available\":%s}",
            i, app->name ? app->name : "",
            app->description ? app->description : "",
            app->is_system ? "true" : "false",
            app->available ? "true" : "false");
    }
    pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "]}");
    return _send_json(req, 200, _shared_json);
}

static esp_err_t _api_shell_launch_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"ok\":false,\"error\":\"empty body\"}");

    int count = tritium_shell::getAppCount();
    int idx = -1;
    _json_extract_int(body, "index", &idx);

    // Try name if no index
    if (idx < 0) {
        char name[64] = {0};
        if (_json_extract_string(body, "name", name, sizeof(name))) {
            for (int i = 0; i < count; i++) {
                const tritium_shell::AppDescriptor* app = tritium_shell::getApp(i);
                if (app && app->name && strcasecmp(name, app->name) == 0) {
                    idx = i;
                    break;
                }
            }
        }
    }
    free(body);

    if (idx < 0 || idx >= count) {
        return _send_json(req, 400, "{\"ok\":false,\"error\":\"invalid app index or name\"}");
    }

    // Lock LVGL — showApp modifies the widget tree
    if (!lvgl_driver::lock(2000)) {
        return _send_json(req, 503, "{\"ok\":false,\"error\":\"lvgl busy\"}");
    }
    tritium_shell::showApp(idx);
    const tritium_shell::AppDescriptor* app = tritium_shell::getApp(idx);
    const char* app_name = (app && app->name) ? app->name : "";
    lvgl_driver::unlock();

    snprintf(_shared_json, SHARED_JSON_SIZE,
        "{\"ok\":true,\"app\":\"%s\"}", app_name);
    return _send_json(req, 200, _shared_json);
}

static esp_err_t _api_shell_home_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    if (!lvgl_driver::lock(2000)) {
        return _send_json(req, 503, "{\"ok\":false,\"error\":\"lvgl busy\"}");
    }
    tritium_shell::showLauncher();
    lvgl_driver::unlock();
    return _send_json(req, 200, "{\"ok\":true}");
}

// ── UI Introspection API ─────────────────────────────────────────────────
// Provides widget tree enumeration, touch injection by widget ID, and
// widget value manipulation for remote debugging and automated testing.

// Get widget class name (e.g. "lv_btn", "lv_label", "lv_slider")
static const char* _widget_class_name(const lv_obj_t* obj) {
    const lv_obj_class_t* cls = lv_obj_get_class(obj);
    if (cls && cls->name) return cls->name;
    return "lv_obj";
}

// Check if widget is interactive (clickable, checkable, editable)
static bool _widget_is_interactive(const lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ||
           lv_obj_has_flag(obj, LV_OBJ_FLAG_CHECKABLE);
}

// Get label text from a widget (direct label or first child label)
static const char* _widget_get_text(const lv_obj_t* obj) {
    const lv_obj_class_t* cls = lv_obj_get_class(obj);
    if (cls == &lv_label_class) {
        return lv_label_get_text(obj);
    }
    // Check first child for label
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt && i < 3; i++) {
        lv_obj_t* child = lv_obj_get_child(obj, i);
        if (lv_obj_get_class(child) == &lv_label_class) {
            return lv_label_get_text(child);
        }
    }
    return nullptr;
}

// Escape a string for JSON output, writing to buf. Returns chars written.
static int _json_escape(char* buf, size_t size, const char* str) {
    int pos = 0;
    buf[pos++] = '"';
    for (const char* p = str; *p && pos < (int)size - 3; p++) {
        if (*p == '"' || *p == '\\') {
            buf[pos++] = '\\';
            if (pos < (int)size - 2) buf[pos++] = *p;
        } else if (*p == '\n') {
            buf[pos++] = '\\';
            if (pos < (int)size - 2) buf[pos++] = 'n';
        } else if ((uint8_t)*p >= 0x20) {
            buf[pos++] = *p;
        }
    }
    buf[pos++] = '"';
    buf[pos] = '\0';
    return pos;
}

// Recursively serialize LVGL widget tree to JSON
static int _serialize_widget(lv_obj_t* obj, char* buf, size_t size, int depth) {
    if (!obj || depth > 12 || size < 128) return 0;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return 0;

    int pos = 0;
    const char* cls_name = _widget_class_name(obj);
    const lv_obj_class_t* cls = lv_obj_get_class(obj);

    // Get absolute coordinates
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    pos += snprintf(buf + pos, size - pos,
        "{\"id\":\"%p\",\"type\":\"%s\","
        "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
        "\"clickable\":%s,\"state\":%u",
        (void*)obj, cls_name,
        (int)coords.x1, (int)coords.y1,
        (int)(coords.x2 - coords.x1 + 1),
        (int)(coords.y2 - coords.y1 + 1),
        _widget_is_interactive(obj) ? "true" : "false",
        (unsigned)lv_obj_get_state(obj));

    // Add text if available
    const char* text = _widget_get_text(obj);
    if (text && text[0]) {
        pos += snprintf(buf + pos, size - pos, ",\"text\":");
        pos += _json_escape(buf + pos, size - pos, text);
    }

    // Add value for specific widget types — covers every widget used in Tritium-OS
    if (cls == &lv_slider_class) {
        pos += snprintf(buf + pos, size - pos, ",\"value\":%d,\"min\":%d,\"max\":%d",
                        (int)lv_slider_get_value(obj),
                        (int)lv_slider_get_min_value(obj),
                        (int)lv_slider_get_max_value(obj));
    } else if (cls == &lv_bar_class) {
        pos += snprintf(buf + pos, size - pos, ",\"value\":%d,\"min\":%d,\"max\":%d",
                        (int)lv_bar_get_value(obj),
                        (int)lv_bar_get_min_value(obj),
                        (int)lv_bar_get_max_value(obj));
    } else if (cls == &lv_checkbox_class || cls == &lv_switch_class) {
        bool checked = lv_obj_get_state(obj) & LV_STATE_CHECKED;
        pos += snprintf(buf + pos, size - pos, ",\"checked\":%s",
                        checked ? "true" : "false");
    } else if (cls == &lv_textarea_class) {
        const char* ta_text = lv_textarea_get_text(obj);
        if (ta_text) {
            pos += snprintf(buf + pos, size - pos, ",\"value\":");
            pos += _json_escape(buf + pos, size - pos, ta_text);
        }
    } else if (cls == &lv_dropdown_class) {
        char sel_text[64] = {};
        lv_dropdown_get_selected_str(obj, sel_text, sizeof(sel_text));
        pos += snprintf(buf + pos, size - pos, ",\"selected\":%u,\"selected_text\":",
                        (unsigned)lv_dropdown_get_selected(obj));
        pos += _json_escape(buf + pos, size - pos, sel_text);
        pos += snprintf(buf + pos, size - pos, ",\"option_count\":%u",
                        (unsigned)lv_dropdown_get_option_count(obj));
    } else if (cls == &lv_buttonmatrix_class) {
        uint32_t sel = lv_buttonmatrix_get_selected_button(obj);
        pos += snprintf(buf + pos, size - pos, ",\"selected_btn\":%u", (unsigned)sel);
        const char* sel_txt = lv_buttonmatrix_get_button_text(obj, sel);
        if (sel_txt) {
            pos += snprintf(buf + pos, size - pos, ",\"selected_text\":");
            pos += _json_escape(buf + pos, size - pos, sel_txt);
        }
    }

    // Recurse into children
    uint32_t child_cnt = lv_obj_get_child_count(obj);
    if (child_cnt > 0 && pos < (int)size - 64) {
        pos += snprintf(buf + pos, size - pos, ",\"children\":[");
        bool first = true;
        for (uint32_t i = 0; i < child_cnt && pos < (int)size - 128; i++) {
            lv_obj_t* child = lv_obj_get_child(obj, i);
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
            if (!first) buf[pos++] = ',';
            int wrote = _serialize_widget(child, buf + pos, size - pos, depth + 1);
            if (wrote > 0) {
                pos += wrote;
                first = false;
            }
        }
        pos += snprintf(buf + pos, size - pos, "]");
    }

    pos += snprintf(buf + pos, size - pos, "}");
    return pos;
}

// Flatten widget tree into a list (for easier testing/parsing)
static int _flatten_widgets(lv_obj_t* obj, char* buf, size_t size, int depth, bool* first) {
    if (!obj || depth > 12 || size < 256) return 0;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return 0;

    int pos = 0;
    if (!*first) buf[pos++] = ',';
    *first = false;

    // Serialize this widget (without children array — flat list)
    const char* cls_name = _widget_class_name(obj);
    const lv_obj_class_t* cls = lv_obj_get_class(obj);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    pos += snprintf(buf + pos, size - pos,
        "{\"id\":\"%p\",\"type\":\"%s\",\"depth\":%d,"
        "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
        "\"clickable\":%s,\"state\":%u",
        (void*)obj, cls_name, depth,
        (int)coords.x1, (int)coords.y1,
        (int)(coords.x2 - coords.x1 + 1),
        (int)(coords.y2 - coords.y1 + 1),
        _widget_is_interactive(obj) ? "true" : "false",
        (unsigned)lv_obj_get_state(obj));

    // Text
    const char* text = _widget_get_text(obj);
    if (text && text[0]) {
        pos += snprintf(buf + pos, size - pos, ",\"text\":");
        pos += _json_escape(buf + pos, size - pos, text);
    }

    // Widget-specific values (same as tree mode)
    if (cls == &lv_slider_class) {
        pos += snprintf(buf + pos, size - pos, ",\"value\":%d,\"min\":%d,\"max\":%d",
                        (int)lv_slider_get_value(obj),
                        (int)lv_slider_get_min_value(obj),
                        (int)lv_slider_get_max_value(obj));
    } else if (cls == &lv_bar_class) {
        pos += snprintf(buf + pos, size - pos, ",\"value\":%d,\"min\":%d,\"max\":%d",
                        (int)lv_bar_get_value(obj),
                        (int)lv_bar_get_min_value(obj),
                        (int)lv_bar_get_max_value(obj));
    } else if (cls == &lv_checkbox_class || cls == &lv_switch_class) {
        bool checked = lv_obj_get_state(obj) & LV_STATE_CHECKED;
        pos += snprintf(buf + pos, size - pos, ",\"checked\":%s",
                        checked ? "true" : "false");
    } else if (cls == &lv_textarea_class) {
        const char* ta_text = lv_textarea_get_text(obj);
        if (ta_text) {
            pos += snprintf(buf + pos, size - pos, ",\"value\":");
            pos += _json_escape(buf + pos, size - pos, ta_text);
        }
    } else if (cls == &lv_dropdown_class) {
        char sel_text[64] = {};
        lv_dropdown_get_selected_str(obj, sel_text, sizeof(sel_text));
        pos += snprintf(buf + pos, size - pos, ",\"selected\":%u,\"selected_text\":",
                        (unsigned)lv_dropdown_get_selected(obj));
        pos += _json_escape(buf + pos, size - pos, sel_text);
    } else if (cls == &lv_buttonmatrix_class) {
        uint32_t sel = lv_buttonmatrix_get_selected_button(obj);
        pos += snprintf(buf + pos, size - pos, ",\"selected_btn\":%u", (unsigned)sel);
        const char* sel_txt = lv_buttonmatrix_get_button_text(obj, sel);
        if (sel_txt) {
            pos += snprintf(buf + pos, size - pos, ",\"selected_text\":");
            pos += _json_escape(buf + pos, size - pos, sel_txt);
        }
    }

    pos += snprintf(buf + pos, size - pos, "}");

    // Recurse into children
    uint32_t child_cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_cnt && pos < (int)size - 256; i++) {
        pos += _flatten_widgets(lv_obj_get_child(obj, i),
                                buf + pos, size - pos, depth + 1, first);
    }
    return pos;
}

// GET /api/ui/tree — Full widget tree of current screen
// Query params:
//   ?flat=1 — returns flat array instead of nested tree (easier for testing)
static esp_err_t _api_ui_tree_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();

    // Allocate a large buffer in PSRAM for the widget tree
    static const size_t UI_TREE_BUF_SIZE = 32768;
    char* buf = (char*)heap_caps_malloc(UI_TREE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) return _send_json(req, 500, "{\"error\":\"out of memory\"}");

    // Check for flat mode
    char flat_val[4] = {};
    bool flat = _get_query_param(req, "flat", flat_val, sizeof(flat_val)) &&
                (flat_val[0] == '1' || flat_val[0] == 't');

    // Lock LVGL mutex — we're on the httpd task, not the LVGL task
    if (!lvgl_driver::lock(2000)) {
        heap_caps_free(buf);
        return _send_json(req, 503, "{\"error\":\"lvgl busy\"}");
    }

    lv_obj_t* screen = lv_screen_active();
    int len = 0;

    if (flat) {
        // Flat mode: return array of all widgets
        buf[len++] = '[';
        bool first = true;
        len += _flatten_widgets(screen, buf + len, UI_TREE_BUF_SIZE - len - 2, 0, &first);
        buf[len++] = ']';
        buf[len] = '\0';
    } else {
        // Tree mode: nested object hierarchy
        len = _serialize_widget(screen, buf, UI_TREE_BUF_SIZE - 1, 0);
        buf[len] = '\0';
    }

    lvgl_driver::unlock();

    esp_err_t ret = _send_json(req, 200, buf);
    heap_caps_free(buf);
    return ret;
}

// POST /api/ui/click — Click a widget by address ID
// Body: {"id":"0x3fca1234"}
static esp_err_t _api_ui_click_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"ok\":false,\"error\":\"empty body\"}");

    char id_str[20] = {};
    _json_extract_string(body, "id", id_str, sizeof(id_str));
    free(body);

    if (!id_str[0]) {
        return _send_json(req, 400, "{\"ok\":false,\"error\":\"missing id\"}");
    }

    // Parse hex address
    uintptr_t addr = (uintptr_t)strtoul(id_str, nullptr, 16);
    lv_obj_t* obj = (lv_obj_t*)addr;

    // Lock LVGL mutex for thread-safe widget access
    if (!lvgl_driver::lock(2000)) {
        return _send_json(req, 503, "{\"ok\":false,\"error\":\"lvgl busy\"}");
    }

    // Validate: walk the tree to confirm this is a real widget
    // (prevents arbitrary memory access)
    bool found = false;
    lv_obj_t* screen = lv_screen_active();
    // Simple BFS validation using a stack
    lv_obj_t* stack[64];
    int sp = 0;
    stack[sp++] = screen;
    while (sp > 0 && !found) {
        lv_obj_t* cur = stack[--sp];
        if (cur == obj) { found = true; break; }
        uint32_t cnt = lv_obj_get_child_count(cur);
        for (uint32_t i = 0; i < cnt && sp < 63; i++) {
            stack[sp++] = lv_obj_get_child(cur, i);
        }
    }

    if (!found) {
        lvgl_driver::unlock();
        return _send_json(req, 404, "{\"ok\":false,\"error\":\"widget not found\"}");
    }

    // Get widget info while holding mutex
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int cx = (coords.x1 + coords.x2) / 2;
    int cy = (coords.y1 + coords.y2) / 2;
    const char* type_name = _widget_class_name(obj);

    // Defer the click to the main loop (process() method) so the event
    // callback runs on the main task's stack, not the httpd task's stack.
    // This prevents stack overflow when callbacks create complex UI.
    if (!_click_done_sem) {
        _click_done_sem = xSemaphoreCreateBinary();
    }
    _pending_click_done = false;
    _pending_click_obj = obj;

    lvgl_driver::unlock();

    // Wait for the main loop to process the click (up to 2s)
    if (_click_done_sem) {
        xSemaphoreTake(_click_done_sem, pdMS_TO_TICKS(2000));
    }

    snprintf(_shared_json, SHARED_JSON_SIZE,
        "{\"ok\":true,\"x\":%d,\"y\":%d,\"type\":\"%s\"}",
        cx, cy, type_name);
    return _send_json(req, 200, _shared_json);
}

// POST /api/ui/set — Set a widget's value
// Body: {"id":"0x3fca1234", "value": 50}       (slider)
// Body: {"id":"0x3fca1234", "checked": true}    (checkbox/switch)
// Body: {"id":"0x3fca1234", "text": "hello"}    (textarea)
// Body: {"id":"0x3fca1234", "selected": 2}      (dropdown)
static esp_err_t _api_ui_set_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"ok\":false,\"error\":\"empty body\"}");

    char id_str[20] = {};
    _json_extract_string(body, "id", id_str, sizeof(id_str));

    if (!id_str[0]) {
        free(body);
        return _send_json(req, 400, "{\"ok\":false,\"error\":\"missing id\"}");
    }

    uintptr_t addr = (uintptr_t)strtoul(id_str, nullptr, 16);
    lv_obj_t* obj = (lv_obj_t*)addr;

    // Lock LVGL mutex for thread-safe widget access
    if (!lvgl_driver::lock(2000)) {
        free(body);
        return _send_json(req, 503, "{\"ok\":false,\"error\":\"lvgl busy\"}");
    }

    // Validate widget exists in tree
    bool found = false;
    lv_obj_t* stack[64];
    int sp = 0;
    stack[sp++] = lv_screen_active();
    while (sp > 0 && !found) {
        lv_obj_t* cur = stack[--sp];
        if (cur == obj) { found = true; break; }
        uint32_t cnt = lv_obj_get_child_count(cur);
        for (uint32_t i = 0; i < cnt && sp < 63; i++) {
            stack[sp++] = lv_obj_get_child(cur, i);
        }
    }

    if (!found) {
        lvgl_driver::unlock();
        free(body);
        return _send_json(req, 404, "{\"ok\":false,\"error\":\"widget not found\"}");
    }

    const lv_obj_class_t* cls = lv_obj_get_class(obj);
    bool ok = false;

    if (cls == &lv_slider_class) {
        int val = 0;
        if (_json_extract_int(body, "value", &val)) {
            lv_slider_set_value(obj, val, LV_ANIM_OFF);
            lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, nullptr);
            ok = true;
        }
    } else if (cls == &lv_bar_class) {
        int val = 0;
        if (_json_extract_int(body, "value", &val)) {
            lv_bar_set_value(obj, val, LV_ANIM_OFF);
            ok = true;
        }
    } else if (cls == &lv_checkbox_class || cls == &lv_switch_class) {
        // Parse "checked":true/false
        const char* chk = strstr(body, "\"checked\"");
        if (chk) {
            bool checked = strstr(chk, "true") != nullptr;
            if (checked) lv_obj_add_state(obj, LV_STATE_CHECKED);
            else lv_obj_remove_state(obj, LV_STATE_CHECKED);
            lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, nullptr);
            ok = true;
        }
    } else if (cls == &lv_textarea_class) {
        char text[256] = {};
        if (_json_extract_string(body, "text", text, sizeof(text))) {
            lv_textarea_set_text(obj, text);
            ok = true;
        }
    } else if (cls == &lv_dropdown_class) {
        int sel = 0;
        if (_json_extract_int(body, "selected", &sel)) {
            lv_dropdown_set_selected(obj, sel);
            lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, nullptr);
            ok = true;
        }
    }

    const char* type_name = _widget_class_name(obj);
    lvgl_driver::unlock();

    free(body);

    if (ok) {
        snprintf(_shared_json, SHARED_JSON_SIZE,
            "{\"ok\":true,\"type\":\"%s\"}", type_name);
        return _send_json(req, 200, _shared_json);
    }
    return _send_json(req, 400, "{\"ok\":false,\"error\":\"unsupported widget type or missing value\"}");
}

#endif // WEB_HAS_SHELL

// ── BLE API handler ─────────────────────────────────────────────────────

static esp_err_t _api_ble_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    if (_instance->_bleProvider) {
        int len = _instance->_bleProvider(_shared_json, SHARED_JSON_SIZE);
        if (len > 0) return _send_json(req, 200, _shared_json);
    }
    return _send_json(req, 200, "{\"active\":false,\"total\":0,\"known\":0,\"devices\":[]}");
}

// ── GIS API handlers ────────────────────────────────────────────────────

static esp_err_t _api_gis_layers_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    if (_instance->_gisLayerProvider) {
        int len = _instance->_gisLayerProvider(_shared_json, SHARED_JSON_SIZE);
        if (len > 0) return _send_json(req, 200, _shared_json);
    }
    return _send_json(req, 200, "[]");
}

// ── Mesh API handlers ───────────────────────────────────────────────────

static esp_err_t _api_mesh_ping_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char val[20] = {0};
    if (!_get_query_param(req, "mac", val, sizeof(val)) || strlen(val) < 17) {
        return _send_json(req, 400, "{\"ok\":false,\"error\":\"Missing mac param\"}");
    }
    uint8_t m[6];
    if (sscanf(val, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return _send_json(req, 400, "{\"ok\":false,\"error\":\"Bad MAC format\"}");
    }
    return _send_json(req, 200, "{\"ok\":true}");
}

static esp_err_t _api_mesh_state_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (body) free(body);
    return _send_json(req, 200, "{\"ok\":true}");
}

static esp_err_t _api_mesh_send_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (body) free(body);
    return _send_json(req, 200, "{\"ok\":true}");
}

// ── WiFi API handlers ───────────────────────────────────────────────────

static esp_err_t _api_wifi_status_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char buf[1536];
    int pos = 0;

    bool connected = _wifi_connected();
    bool ap_active = _wifi_ap_active();

    char ssid[33] = {0}, ipStr[16] = {0}, macStr[18] = {0};
    _get_wifi_ssid(ssid, sizeof(ssid));
    _get_wifi_ip(ipStr, sizeof(ipStr));
    _get_mac_str(macStr, sizeof(macStr));

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"connected\":%s,\"ap_active\":%s,\"state\":\"%s\","
        "\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"channel\":%d,",
        connected ? "true" : "false",
        ap_active ? "true" : "false",
        connected ? (ap_active ? "ap_and_sta" : "connected") :
            (ap_active ? "ap_only" : "disconnected"),
        connected ? ssid : "",
        connected ? ipStr : "",
        connected ? _get_wifi_rssi() : 0,
        connected ? _get_wifi_channel() : 0);

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "\"mac\":\"%s\",", macStr);

    // AP info
    if (ap_active) {
        char ap_ssid[33] = {0}, ap_ip[16] = {0};
        _get_ap_ssid(ap_ssid, sizeof(ap_ssid));
        _get_ap_ip(ap_ip, sizeof(ap_ip));
        wifi_sta_list_t sta_list;
        int clients = 0;
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) clients = sta_list.num;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"clients\":%d,",
            ap_ssid, ap_ip, clients);
    } else {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "\"ap_ssid\":\"\",\"ap_ip\":\"\",\"clients\":0,");
    }

    // Failover
    bool failover = true;
#if WEB_HAS_WIFI_MANAGER
    if (WifiManager::_instance) {
        failover = WifiManager::_instance->isAutoFailoverEnabled();
    }
#endif
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\"failover\":%s,", failover ? "true" : "false");

    // Saved networks
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\"saved\":[");
#if WEB_HAS_WIFI_MANAGER
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
#endif
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return _send_json(req, 200, buf);
}

static esp_err_t _api_wifi_scan_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
#if WEB_HAS_WIFI_MANAGER
    if (WifiManager::_instance) {
        WifiManager::_instance->startScan();
        ScanResult results[WIFI_MAX_SCAN_RESULTS];
        int count = WifiManager::_instance->getScanResults(results, WIFI_MAX_SCAN_RESULTS);
        int pos = snprintf(_shared_json, SHARED_JSON_SIZE, "{\"networks\":[");
        for (int i = 0; i < count && pos < (int)SHARED_JSON_SIZE - 120; i++) {
            if (i > 0) _shared_json[pos++] = ',';
            const char* auth_str = "UNKNOWN";
            switch (results[i].auth) {
                case WifiAuth::OPEN:         auth_str = "OPEN"; break;
                case WifiAuth::WEP:          auth_str = "WEP"; break;
                case WifiAuth::WPA_PSK:      auth_str = "WPA_PSK"; break;
                case WifiAuth::WPA2_PSK:     auth_str = "WPA2_PSK"; break;
                case WifiAuth::WPA_WPA2_PSK: auth_str = "WPA_WPA2_PSK"; break;
                case WifiAuth::WPA3_PSK:     auth_str = "WPA3_PSK"; break;
                default: break;
            }
            pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos,
                "{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"auth\":\"%s\",\"known\":%s}",
                results[i].ssid, results[i].rssi, results[i].channel,
                auth_str, results[i].known ? "true" : "false");
        }
        pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "]}");
        return _send_json(req, 200, _shared_json);
    }
#endif
    // Fallback: direct ESP-IDF scan
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = true;
    esp_wifi_scan_start(&scan_cfg, true);
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t* records = nullptr;
    if (n > 0) {
        records = (wifi_ap_record_t*)malloc(n * sizeof(wifi_ap_record_t));
        if (records) esp_wifi_scan_get_ap_records(&n, records);
    }
    int pos = snprintf(_shared_json, SHARED_JSON_SIZE, "{\"networks\":[");
    for (uint16_t i = 0; i < n && records && pos < (int)SHARED_JSON_SIZE - 120; i++) {
        if (i > 0) _shared_json[pos++] = ',';
        pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"auth\":\"%s\",\"known\":false}",
            (const char*)records[i].ssid, records[i].rssi, records[i].primary,
            records[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "WPA2_PSK");
    }
    pos += snprintf(_shared_json + pos, SHARED_JSON_SIZE - pos, "]}");
    if (records) free(records);
    return _send_json(req, 200, _shared_json);
}

static esp_err_t _api_wifi_connect_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"error\":\"empty body\"}");

    char ssid[33] = {0}, pass[64] = {0};
    _json_extract_string(body, "ssid", ssid, sizeof(ssid));
    _json_extract_string(body, "password", pass, sizeof(pass));
    free(body);

    bool ok = false;
    if (ssid[0]) {
        DBG_INFO("web", "WiFi connecting to: %s", ssid);
#if WEB_HAS_WIFI_MANAGER
        if (WifiManager::_instance) {
            ok = WifiManager::_instance->connectTo(ssid, pass, true);
        } else
#endif
        {
            wifi_config_t wifi_cfg = {};
            strncpy((char*)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
            strncpy((char*)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
            esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
            esp_wifi_connect();
            uint32_t start = millis();
            while (!_wifi_connected() && (millis() - start) < 15000) {
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            ok = _wifi_connected();
        }
    }

    char ipStr[16] = {0};
    if (ok) _get_wifi_ip(ipStr, sizeof(ipStr));
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"}",
        ok ? "true" : "false", ok ? ssid : "", ipStr);
    return _send_json(req, 200, resp);
}

static esp_err_t _api_wifi_remove_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"ok\":false}");
    char ssid[33] = {0};
    _json_extract_string(body, "ssid", ssid, sizeof(ssid));
    free(body);
    bool ok = false;
#if WEB_HAS_WIFI_MANAGER
    if (ssid[0] && WifiManager::_instance) {
        ok = WifiManager::_instance->removeNetwork(ssid);
        DBG_INFO("web", "WiFi network removed: %s", ssid);
    }
#endif
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":%s}", ok ? "true" : "false");
    return _send_json(req, 200, resp);
}

static esp_err_t _api_wifi_reorder_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"ok\":false}");
    char ssid[33] = {0};
    int priority = -1;
    _json_extract_string(body, "ssid", ssid, sizeof(ssid));
    _json_extract_int(body, "priority", &priority);
    free(body);
    bool ok = false;
#if WEB_HAS_WIFI_MANAGER
    if (ssid[0] && WifiManager::_instance) {
        ok = WifiManager::_instance->reorderNetwork(ssid, (int8_t)priority);
    }
#endif
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":%s}", ok ? "true" : "false");
    return _send_json(req, 200, resp);
}

static esp_err_t _api_wifi_ap_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    bool activate = body && strstr(body, "true");
    if (body) free(body);
    bool ok = false;
#if WEB_HAS_WIFI_MANAGER
    if (WifiManager::_instance) {
        ok = activate ? WifiManager::_instance->startAP() : WifiManager::_instance->stopAP();
    }
#endif
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":%s}", ok ? "true" : "false");
    return _send_json(req, 200, resp);
}

static esp_err_t _api_wifi_disconnect_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
#if WEB_HAS_WIFI_MANAGER
    if (WifiManager::_instance) {
        WifiManager::_instance->disconnect();
    } else
#endif
    {
        esp_wifi_disconnect();
    }
    DBG_INFO("web", "WiFi disconnected via API");
    return _send_json(req, 200, "{\"ok\":true,\"message\":\"Disconnected\"}");
}

static esp_err_t _api_wifi_reconnect_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    bool ok = false;
#if WEB_HAS_WIFI_MANAGER
    if (WifiManager::_instance) {
        ok = WifiManager::_instance->connect();
    }
#endif
    DBG_INFO("web", "WiFi reconnect requested via API (ok=%d)", ok);
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"message\":\"Reconnecting...\"}", ok ? "true" : "false");
    return _send_json(req, 200, resp);
}

static esp_err_t _api_wifi_failover_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    bool enabled = body && strstr(body, "\"enabled\"") && strstr(body, "true");
    int timeout_s = 30;
    if (body) {
        _json_extract_int(body, "timeout_s", &timeout_s);
        free(body);
    }
    bool ok = false;
#if WEB_HAS_WIFI_MANAGER
    if (WifiManager::_instance) {
        WifiManager::_instance->enableAutoFailover(enabled);
        ok = true;
    }
#endif
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"enabled\":%s,\"timeout_s\":%d}",
        ok ? "true" : "false", enabled ? "true" : "false", timeout_s);
    return _send_json(req, 200, resp);
}

static esp_err_t _api_wifi_ap_config_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    char* body = _recv_body(req);
    if (!body) return _send_json(req, 400, "{\"ok\":false}");
    char ssid[33] = {0}, pass[64] = {0};
    _json_extract_string(body, "ssid", ssid, sizeof(ssid));
    _json_extract_string(body, "password", pass, sizeof(pass));
    free(body);
    bool ok = false;
#if WEB_HAS_WIFI_MANAGER
    if (WifiManager::_instance && ssid[0]) {
        WifiManager::_instance->stopAP();
        ok = WifiManager::_instance->startAP(ssid, pass[0] ? pass : nullptr);
        DBG_INFO("web", "AP configured: ssid=%s ok=%d", ssid, ok);
    }
#endif
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"ssid\":\"%s\"}", ok ? "true" : "false", ssid);
    return _send_json(req, 200, resp);
}

// ── Page handler functions for remaining pages ──────────────────────────

static esp_err_t _screenshot_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    return _send(req, 200, "text/html", SCREENSHOT_HTML);
}

#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
static esp_err_t _remote_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, REMOTE_HTML, REMOTE_HTML_LEN);
}
#endif

static esp_err_t _wifi_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    return _send(req, 200, "text/html", WIFI_HTML_V2);
#else
    std::string html(WIFI_HTML);
    strReplace(html, "%THEME%", THEME_CSS);
    strReplace(html, "%NAV%",   NAV_HTML);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, html.c_str(), html.size());
#endif
}

static esp_err_t _ble_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    std::string html(BLE_HTML);
    strReplace(html, "%THEME%", THEME_CSS);
    strReplace(html, "%NAV%",   NAV_HTML);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, html.c_str(), html.size());
}

static esp_err_t _commission_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    std::string html(COMMISSION_HTML);
    strReplace(html, "%THEME%", THEME_CSS);
    strReplace(html, "%NAV%",   NAV_HTML);

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char macStr[18], deviceId[13];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(deviceId, sizeof(deviceId), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    strReplace(html, "%MAC%", macStr);
    strReplace(html, "%DEVICE_ID%", deviceId);

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    bool isAP = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);

    if (isAP) {
        char ap_ssid[33] = {0};
        _get_ap_ssid(ap_ssid, sizeof(ap_ssid));
        char modeStr[64];
        snprintf(modeStr, sizeof(modeStr), "Access Point: %s", ap_ssid);
        strReplace(html, "%MODE%", modeStr);
        char connStr[64];
        snprintf(connStr, sizeof(connStr), "WIFI:T:nopass;S:%s;;", ap_ssid);
        strReplace(html, "%CONN_STR%", connStr);
    } else {
        char ssid[33] = {0}, ipStr[16] = {0};
        _get_wifi_ssid(ssid, sizeof(ssid));
        _get_wifi_ip(ipStr, sizeof(ipStr));
        char modeStr[64];
        snprintf(modeStr, sizeof(modeStr), "WiFi Client (%s)", ssid);
        strReplace(html, "%MODE%", modeStr);
        char connStr[64];
        snprintf(connStr, sizeof(connStr), "http://%s/", ipStr);
        strReplace(html, "%CONN_STR%", connStr);
    }

    // Fleet URL from config
    strReplace(html, "%FLEET_URL%", "");  // TODO: read from config.json via POSIX

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, html.c_str(), html.size());
}

static esp_err_t _commission_post_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    // TODO: parse form body and save fleet_url to config.json
    return _send_redirect(req, "/commission");
}

static esp_err_t _system_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    std::string html(SYSTEM_HTML);
    strReplace(html, "%THEME%", THEME_CSS);
    strReplace(html, "%NAV%",   NAV_HTML);

    // Chip info
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    strReplace(html, "%CHIP_MODEL%", "ESP32-S3");
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%d", chip.revision);
    strReplace(html, "%CHIP_REV%", tmp);
    snprintf(tmp, sizeof(tmp), "%d", chip.cores);
    strReplace(html, "%CORES%", tmp);
    snprintf(tmp, sizeof(tmp), "%d", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    strReplace(html, "%CPU_MHZ%", tmp);
    strReplace(html, "%SDK%", esp_get_idf_version());

    // Memory
    char sizeBuf[32];
    snprintf(sizeBuf, sizeof(sizeBuf), "16.0 MB");  // Known flash size for all boards
    strReplace(html, "%FLASH_SIZE%", sizeBuf);
    strReplace(html, "%FLASH_SPEED%", "80");
    snprintf(sizeBuf, sizeof(sizeBuf), "%.1f KB", esp_get_free_heap_size() / 1024.0f + 100);  // approx total
    strReplace(html, "%HEAP_TOTAL%", sizeBuf);
    snprintf(sizeBuf, sizeof(sizeBuf), "%.1f KB", esp_get_free_heap_size() / 1024.0f);
    strReplace(html, "%HEAP_FREE%", sizeBuf);
    snprintf(sizeBuf, sizeof(sizeBuf), "%.1f KB", esp_get_minimum_free_heap_size() / 1024.0f);
    strReplace(html, "%HEAP_MIN%", sizeBuf);
    snprintf(sizeBuf, sizeof(sizeBuf), "%.1f MB", heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1048576.0f);
    strReplace(html, "%PSRAM_TOTAL%", sizeBuf);
    snprintf(sizeBuf, sizeof(sizeBuf), "%.1f MB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0f);
    strReplace(html, "%PSRAM_FREE%", sizeBuf);

    // Network
    char macStr[18] = {0}, ssid[33] = {0}, ipStr[16] = {0};
    char gwStr[16] = {0}, dnsStr[16] = {0};
    _get_mac_str(macStr, sizeof(macStr));
    _get_wifi_ssid(ssid, sizeof(ssid));
    _get_wifi_ip(ipStr, sizeof(ipStr));
    _get_gateway_ip(gwStr, sizeof(gwStr));
    _get_dns_ip(dnsStr, sizeof(dnsStr));

    strReplace(html, "%MAC%", macStr);
    strReplace(html, "%SSID%", _wifi_connected() ? ssid : "Not connected");
    strReplace(html, "%IP%", ipStr);
    strReplace(html, "%GATEWAY%", gwStr);
    strReplace(html, "%DNS%", dnsStr);
    snprintf(tmp, sizeof(tmp), "%d", _get_wifi_rssi());
    strReplace(html, "%RSSI%", tmp);
    snprintf(tmp, sizeof(tmp), "%d", _get_wifi_channel());
    strReplace(html, "%CHANNEL%", tmp);

    // Runtime
    char uptBuf[32];
    uptimeString(uptBuf, sizeof(uptBuf));
    strReplace(html, "%UPTIME%", uptBuf);
    snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)_instance->_requestCount);
    strReplace(html, "%REQCOUNT%", tmp);

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
    strReplace(html, "%RESET_REASON%", reasonStr);

    // Partitions
    std::string partHtml;
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
    if (partHtml.empty())
        partHtml = "<tr><td colspan='4' style='color:#666'>No partition info</td></tr>";
    strReplace(html, "%PARTITIONS%", partHtml.c_str());

    // Tasks
    char taskBuf[1024];
#if configUSE_TRACE_FACILITY && configTASKLIST_INCLUDE_COREID
    vTaskList(taskBuf);
#else
    snprintf(taskBuf, sizeof(taskBuf), "Name             State  Prio  Stack  Num\n"
        "(Task list requires configUSE_TRACE_FACILITY=1)");
#endif
    strReplace(html, "%TASKS%", taskBuf);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, html.c_str(), html.size());
}

static esp_err_t _logs_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    std::string html(LOGS_HTML);
    strReplace(html, "%THEME%", THEME_CSS);
    strReplace(html, "%NAV%",   NAV_HTML);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, html.c_str(), html.size());
}

static esp_err_t _map_page_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    std::string html(MAP_HTML);
    strReplace(html, "%THEME%", THEME_CSS);
    strReplace(html, "%NAV%",   NAV_HTML);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, html.c_str(), html.size());
}

#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
static esp_err_t _mesh_page_v2_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    return _send(req, 200, "text/html", MESH_HTML_V2);
}
#else
static esp_err_t _mesh_page_fallback_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    // Check for mesh.html on LittleFS via POSIX
    struct stat st;
    if (stat("/littlefs/web/mesh.html", &st) == 0) {
        FILE* f = fopen("/littlefs/web/mesh.html", "r");
        if (f) {
            httpd_resp_set_type(req, "text/html");
            char buf[512];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                httpd_resp_send_chunk(req, buf, n);
            }
            fclose(f);
            return httpd_resp_send_chunk(req, nullptr, 0);
        }
    }
    return _send(req, 200, "text/html",
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>Mesh</title></head><body style='background:#0a0a0f;color:#c8d0dc;font-family:monospace;padding:20px'>"
        "<h2 style='color:#00f0ff'>Mesh Topology</h2>"
        "<p>Upload <code>/web/mesh.html</code> to LittleFS for the full UI.</p>"
        "<p><a href='/api/mesh' style='color:#00f0ff'>Raw mesh JSON &rarr;</a></p>"
        "</body></html>");
}
#endif

// ── Wildcard catch-all handler (replaces onNotFound) ────────────────────

static esp_err_t _catchall_handler(httpd_req_t* req) {
    _instance->_requestCount++; _instance->_lastRequestMs = millis();
    const char* uri = req->uri;

    // CORS preflight
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
        httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-Width, X-Height");
        httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
        return httpd_resp_send(req, "", 0);
    }

    // GIS tile requests
    if (strncmp(uri, "/api/gis/tiles/", 15) == 0 && _instance->_gisTileProvider) {
        const char* path = uri + 15;
        const char* slash1 = strchr(path, '/');
        if (slash1) {
            char layer[32] = {0};
            size_t layerLen = slash1 - path;
            if (layerLen >= sizeof(layer)) layerLen = sizeof(layer) - 1;
            memcpy(layer, path, layerLen);
            int z = 0, x = 0, y = 0;
            if (sscanf(slash1, "/%d/%d/%d.png", &z, &x, &y) == 3) {
                size_t tileLen = 0;
                uint8_t* tileData = _instance->_gisTileProvider(
                    layer, (uint8_t)z, (uint32_t)x, (uint32_t)y, tileLen);
                if (tileData && tileLen > 0) {
                    httpd_resp_set_type(req, "image/png");
                    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
                    esp_err_t err = httpd_resp_send(req, (const char*)tileData, tileLen);
                    free(tileData);
                    return err;
                }
                return _send(req, 404, "text/plain", "Tile not found");
            }
        }
        return _send(req, 400, "text/plain", "Invalid tile path");
    }

    // Settings domain endpoint: /api/settings/{domain}
#if WEB_HAS_SETTINGS
    if (strncmp(uri, "/api/settings/", 14) == 0 && req->method == HTTP_GET) {
        const char* domain = uri + 14;
        if (domain[0] != '\0' && strchr(domain, '/') == nullptr) {
            int len = TritiumSettings::instance().toJson(
                _shared_json, SHARED_JSON_SIZE, domain);
            if (len > 0) return _send_json(req, 200, _shared_json);
            return _send_json(req, 404, "{\"error\":\"domain not found or empty\"}");
        }
    }
#endif

    // Captive portal redirect
    if (_captive_portal_active) {
        return _send_redirect(req, "http://192.168.4.1/wifi");
    }

    // 404 page
    std::string html(ERROR_HTML);
    strReplace(html, "%THEME%", THEME_CSS);
    strReplace(html, "%NAV%",   NAV_HTML);
    strReplace(html, "%ERROR_CODE%", "404");
    strReplace(html, "%ERROR_TITLE%", "Not Found");
    char msg[256];
    snprintf(msg, sizeof(msg), "The path <span class=\"error-uri\">%s</span> does not exist on this node.", uri);
    strReplace(html, "%ERROR_MSG%", msg);
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html.c_str(), html.size());
}

// ── addApiEndpoints() ────────────────────────────────────────────────────

void WebServerHAL::addApiEndpoints() {
    if (!_server) return;

    REG(_server, "/api/status",     HTTP_GET,  _api_status_handler);
    REG(_server, "/api/board",      HTTP_GET,  _api_board_handler);
#if WEB_HAS_FINGERPRINT
    REG(_server, "/api/fingerprint", HTTP_GET, _api_fingerprint_handler);
#endif
    REG(_server, "/api/reboot",     HTTP_POST, _api_reboot_handler);
    REG(_server, "/api/scan",       HTTP_GET,  _api_scan_handler);
    REG(_server, "/api/node",       HTTP_GET,  _api_node_handler);
    REG(_server, "/api/mesh",       HTTP_GET,  _api_mesh_handler);
    REG(_server, "/api/diag",       HTTP_GET,  _api_diag_handler);
    REG(_server, "/api/diag/health",    HTTP_GET, _api_diag_health_handler);
    REG(_server, "/api/diag/events",    HTTP_GET, _api_diag_events_handler);
    REG(_server, "/api/diag/anomalies", HTTP_GET, _api_diag_anomalies_handler);
#if WEB_HAS_DIAGLOG
    REG(_server, "/api/diag/log",       HTTP_GET,  _api_diaglog_handler);
    REG(_server, "/api/diag/log/clear", HTTP_POST, _api_diaglog_clear_handler);
#endif
    REG(_server, "/api/logs",       HTTP_GET, _api_logs_handler);
    REG(_server, "/api/screenshot",      HTTP_GET, _api_screenshot_handler);
    REG(_server, "/api/screenshot.json", HTTP_GET, _api_screenshot_json_handler);

    // Remote control
#if WEB_HAS_TOUCH_INPUT
    REG(_server, "/api/remote/touch", HTTP_POST, _api_remote_touch_handler);
    REG(_server, "/api/remote/tap",   HTTP_POST, _api_remote_tap_handler);
    REG(_server, "/api/remote/swipe", HTTP_POST, _api_remote_swipe_handler);
#endif
    REG(_server, "/api/remote/screenshot", HTTP_GET, _api_remote_screenshot_handler);
    REG(_server, "/api/remote/info",       HTTP_GET, _api_remote_info_handler);

    // Settings
#if WEB_HAS_SETTINGS
    REG(_server, "/api/settings",       HTTP_GET,  _api_settings_get_handler);
    REG(_server, "/api/settings",       HTTP_PUT,  _api_settings_put_handler);
    REG(_server, "/api/settings/reset", HTTP_POST, _api_settings_reset_handler);
#endif

    // Polling fallback
#if defined(ENABLE_SETTINGS) || defined(ENABLE_OS_EVENTS)
    REG(_server, "/api/events/poll",  HTTP_GET,  _api_events_poll_handler);
    REG(_server, "/api/serial/poll",  HTTP_GET,  _api_serial_poll_handler);
    REG(_server, "/api/serial/send",  HTTP_POST, _api_serial_send_handler);
#endif

    // Debug
#if WEB_HAS_TOUCH_INPUT
    REG(_server, "/api/debug/touch", HTTP_GET, _api_debug_touch_handler);
    REG(_server, "/api/debug/gt911", HTTP_GET, _api_debug_gt911_handler);
#endif
#if WEB_HAS_SHELL
    REG(_server, "/api/debug/lvgl", HTTP_GET, _api_debug_lvgl_handler);
    REG(_server, "/api/shell/apps",   HTTP_GET,  _api_shell_apps_handler);
    REG(_server, "/api/shell/launch", HTTP_POST, _api_shell_launch_handler);
    REG(_server, "/api/shell/home",   HTTP_POST, _api_shell_home_handler);
    REG(_server, "/api/ui/tree",      HTTP_GET,  _api_ui_tree_handler);
    REG(_server, "/api/ui/click",     HTTP_POST, _api_ui_click_handler);
    REG(_server, "/api/ui/set",       HTTP_POST, _api_ui_set_handler);
#endif

    // BLE
    REG(_server, "/api/ble", HTTP_GET, _api_ble_handler);

    // GIS
    REG(_server, "/api/gis/layers", HTTP_GET, _api_gis_layers_handler);

    // WiFi
    REG(_server, "/api/wifi/status",     HTTP_GET,  _api_wifi_status_handler);
    REG(_server, "/api/wifi/scan",       HTTP_GET,  _api_wifi_scan_handler);
    REG(_server, "/api/wifi/connect",    HTTP_POST, _api_wifi_connect_handler);
    REG(_server, "/api/wifi/remove",     HTTP_POST, _api_wifi_remove_handler);
    REG(_server, "/api/wifi/reorder",    HTTP_POST, _api_wifi_reorder_handler);
    REG(_server, "/api/wifi/ap",         HTTP_POST, _api_wifi_ap_handler);
    REG(_server, "/api/wifi/disconnect", HTTP_POST, _api_wifi_disconnect_handler);
    REG(_server, "/api/wifi/reconnect",  HTTP_POST, _api_wifi_reconnect_handler);
    REG(_server, "/api/wifi/failover",   HTTP_POST, _api_wifi_failover_handler);
    REG(_server, "/api/wifi/ap/config",  HTTP_POST, _api_wifi_ap_config_handler);

    // Mesh API
    REG(_server, "/api/mesh/ping",  HTTP_POST, _api_mesh_ping_handler);
    REG(_server, "/api/mesh/state", HTTP_POST, _api_mesh_state_handler);
    REG(_server, "/api/mesh/send",  HTTP_POST, _api_mesh_send_handler);

    DBG_INFO("web", "API endpoints added at /api/*");
}

// ── Page registration functions ─────────────────────────────────────────

void WebServerHAL::addScreenshotPage() {
    if (!_server) return;
    REG(_server, "/screenshot", HTTP_GET, _screenshot_page_handler);
    DBG_INFO("web", "Screenshot page at /screenshot");
}

void WebServerHAL::addRemotePage() {
    if (!_server) return;
#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    REG(_server, "/remote", HTTP_GET, _remote_page_handler);
    DBG_INFO("web", "Remote control page at /remote");
#else
    DBG_INFO("web", "Remote page skipped (web pages not enabled)");
#endif
}

void WebServerHAL::addWiFiSetup() {
    if (!_server) return;
    REG(_server, "/wifi", HTTP_GET, _wifi_page_handler);
    DBG_INFO("web", "WiFi manager added at /wifi (API: /api/wifi/*)");
}

void WebServerHAL::addBleViewer() {
    if (!_server) return;
    REG(_server, "/ble", HTTP_GET, _ble_page_handler);
    DBG_INFO("web", "BLE viewer added at /ble");
}

void WebServerHAL::addCommissionPage() {
    if (!_server) return;
    REG(_server, "/commission", HTTP_GET,  _commission_page_handler);
    REG(_server, "/commission", HTTP_POST, _commission_post_handler);
    DBG_INFO("web", "Commission page added at /commission");
}

// ── Log access (delegates to serial_capture ring buffer) ─────────────────

void WebServerHAL::captureLog(const char*) {
    // No-op: serial_capture ring buffer handles all log capture now.
}

int WebServerHAL::getLogJson(char* buf, size_t size) {
    return serial_capture::getLinesJson(buf, size, 48);
}

void WebServerHAL::addSystemPage() {
    if (!_server) return;
    REG(_server, "/system", HTTP_GET, _system_page_handler);
    DBG_INFO("web", "System page added at /system");
}

void WebServerHAL::addLogsPage() {
    if (!_server) return;
    REG(_server, "/logs", HTTP_GET, _logs_page_handler);
    // /api/logs already registered in init() route block
    DBG_INFO("web", "Logs page added at /logs");
}

void WebServerHAL::addMapPage() {
    if (!_server) return;
    REG(_server, "/map", HTTP_GET, _map_page_handler);
    DBG_INFO("web", "Map page added at /map");
}

void WebServerHAL::addErrorPages() {
    if (!_server) return;
    // Register wildcard catch-all — must be last registered URI
    httpd_uri_t catchall = {};
    catchall.uri = "/*";
    catchall.method = HTTP_GET;
    catchall.handler = _catchall_handler;
    catchall.user_ctx = _instance;
    httpd_register_uri_handler(_server, &catchall);
    DBG_INFO("web", "Error pages registered (404/500)");
}

void WebServerHAL::addMeshPage() {
    if (!_server) return;
#if defined(ENABLE_FILE_MANAGER) || defined(ENABLE_OTA) || defined(ENABLE_SETTINGS)
    REG(_server, "/mesh", HTTP_GET, _mesh_page_v2_handler);
#else
    REG(_server, "/mesh", HTTP_GET, _mesh_page_fallback_handler);
#endif
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
    addErrorPages();       // Must be last — registers wildcard catch-all handler
    DBG_INFO("web", "All pages registered");
}

// ── runTest() ───────────────────────────────────────────────────────────────

WebServerHAL::TestResult WebServerHAL::runTest() {
    TestResult result = {};
    uint32_t start = millis();

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

    result.mdns_ok = startMDNS("esp32-test");

    if (_server && _running) {
        addDashboard();
        result.dashboard_ok = true;
    }

    if (_server && _running) {
        addApiEndpoints();
        result.api_ok = true;
    }

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
