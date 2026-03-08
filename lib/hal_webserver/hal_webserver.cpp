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
void WebServerHAL::addAllPages() {}
void WebServerHAL::setBleProvider(BleJsonProvider) {}
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
#include <cstring>

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
  <a href="/wifi">WiFi</a>
  <a href="/ble">BLE Scan</a>
  <a href="/update">OTA Update</a>
  <a href="/config">Config</a>
  <a href="/files">Files</a>
  <a href="/api/status">API</a>
</div>
)rawliteral";

// ── Dashboard page (/) ──────────────────────────────────────────────────────

static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Dashboard</title>
%THEME%
<script>
setTimeout(function(){location.reload()},5000);
</script>
</head><body>
%NAV%
<h1>// System Dashboard</h1>
<div class="card">
<table>
<tr><th>Parameter</th><th>Value</th></tr>
<tr><td>Board</td><td>%BOARD%</td></tr>
<tr><td>MAC Address</td><td>%MAC%</td></tr>
<tr><td>IP Address</td><td>%IP%</td></tr>
<tr><td>Uptime</td><td>%UPTIME%</td></tr>
<tr><td>Free Heap</td><td>%HEAP% bytes</td></tr>
<tr><td>PSRAM Total</td><td>%PSRAM_TOTAL% bytes</td></tr>
<tr><td>PSRAM Free</td><td>%PSRAM_FREE% bytes</td></tr>
<tr><td>WiFi RSSI</td><td>%RSSI% dBm
  <div class="bar-bg"><div class="bar-fill" style="width:%RSSI_PCT%%"></div></div>
</td></tr>
<tr><td>Requests Served</td><td>%REQCOUNT%</td></tr>
</table>
</div>
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

    // Redirect all unknown URIs to /wifi setup page
    _server->onNotFound([]() {
        _server->sendHeader("Location", "http://192.168.4.1/wifi", true);
        _server->send(302, "text/plain", "Redirecting to setup...");
    });

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

        _server->send(200, "text/html", html);
    });

    DBG_INFO("web", "Dashboard page added at /");
}

// ── addOtaPage() ────────────────────────────────────────────────────────────

void WebServerHAL::addOtaPage() {
    if (!_server) return;

    WebServerHAL* self = this;

    // Serve the upload form
    _server->on("/update", HTTP_GET, [self]() {
        self->_requestCount++;
        String html(FPSTR(OTA_HTML));
        html.replace("%THEME%", FPSTR(THEME_CSS));
        html.replace("%NAV%",   FPSTR(NAV_HTML));
        _server->send(200, "text/html", html);
    });

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

    DBG_INFO("web", "OTA page added at /update");
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

// ── addApiEndpoints() ───────────────────────────────────────────────────────

void WebServerHAL::addApiEndpoints() {
    if (!_server) return;

    WebServerHAL* self = this;

    // GET /api/status
    _server->on("/api/status", HTTP_GET, [self]() {
        self->_requestCount++;
        char buf[384];
        snprintf(buf, sizeof(buf),
            "{\"uptime_s\":%lu,\"free_heap\":%lu,\"psram_free\":%lu,"
            "\"rssi\":%d,\"ip\":\"%s\",\"requests\":%lu}",
            (unsigned long)(millis() / 1000),
            (unsigned long)ESP.getFreeHeap(),
            (unsigned long)ESP.getFreePsram(),
            WiFi.RSSI(),
            WiFi.localIP().toString().c_str(),
            (unsigned long)self->_requestCount);
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
    var rows='<tr><th>SSID</th><th>RSSI</th><th>Ch</th><th>Security</th><th></th></tr>';
    d.networks.forEach(n=>{
      var sec=n.encryption==0?'Open':'Encrypted';
      rows+='<tr><td>'+n.ssid+'</td><td>'+n.rssi+' dBm</td><td>'+n.channel+'</td><td>'+sec+'</td>';
      rows+='<td><button onclick="document.getElementById(\'ssid_input\').value=\''+n.ssid+'\'">Select</button></td></tr>';
    });
    if(d.count==0) rows+='<tr><td colspan="5" style="color:#666">No networks found</td></tr>';
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
            static char buf[2048];
            int len = self->_bleProvider(buf, sizeof(buf));
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

// ── addAllPages() ────────────────────────────────────────────────────────

void WebServerHAL::addAllPages() {
    addDashboard();
    addOtaPage();
    addConfigEditor();
    addFileManager();
    addApiEndpoints();
    addWiFiSetup();
    addBleViewer();
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
