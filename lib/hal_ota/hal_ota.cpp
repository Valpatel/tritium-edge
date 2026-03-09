#include "hal_ota.h"
#include "debug_log.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>

static constexpr const char* TAG = "ota";

// ============================================================================
// Platform: ESP32
// ============================================================================
#ifndef SIMULATOR

#include <Arduino.h>
#include <WiFi.h>
#include <Update.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

static WebServer* _server = nullptr;
static OtaHAL* _instance = nullptr;

// ---------------------------------------------------------------------------
// Minimal dark-themed HTML upload page
// ---------------------------------------------------------------------------
static const char OTA_UPLOAD_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA Firmware Update</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#1a1a2e;color:#e0e0e0;font-family:system-ui,sans-serif;
     display:flex;align-items:center;justify-content:center;min-height:100vh}
.card{background:#16213e;border-radius:12px;padding:2rem;max-width:420px;
      width:90%;box-shadow:0 4px 24px rgba(0,0,0,.4)}
h1{font-size:1.4rem;margin-bottom:1rem;color:#0f9b8e}
.info{font-size:.85rem;color:#8a8a9a;margin-bottom:1.5rem}
input[type=file]{display:block;width:100%;padding:.6rem;margin-bottom:1rem;
      background:#0f3460;border:1px solid #0f9b8e;border-radius:6px;color:#e0e0e0}
input[type=submit]{width:100%;padding:.7rem;background:#0f9b8e;color:#1a1a2e;
      border:none;border-radius:6px;font-size:1rem;font-weight:700;cursor:pointer}
input[type=submit]:hover{background:#12c4b3}
#prog{width:100%;height:20px;border-radius:6px;margin-top:1rem;display:none}
#msg{margin-top:1rem;font-size:.9rem;text-align:center}
.ok{color:#0f9b8e}.err{color:#e94560}
</style>
</head>
<body>
<div class="card">
<h1>OTA Firmware Update</h1>
<div class="info">Select a .bin firmware file to upload.</div>
<form method="POST" action="/update" enctype="multipart/form-data" id="fm">
<input type="file" name="firmware" accept=".bin" required>
<input type="submit" value="Upload &amp; Flash">
</form>
<progress id="prog" value="0" max="100"></progress>
<div id="msg"></div>
</div>
<script>
document.getElementById('fm').addEventListener('submit',function(e){
  e.preventDefault();
  var f=new FormData(this);
  var x=new XMLHttpRequest();
  var p=document.getElementById('prog');
  var m=document.getElementById('msg');
  p.style.display='block';
  m.textContent='Uploading...';m.className='';
  x.upload.addEventListener('progress',function(ev){
    if(ev.lengthComputable){p.value=Math.round(ev.loaded/ev.total*100);}
  });
  x.onreadystatechange=function(){
    if(x.readyState===4){
      if(x.status===200){m.textContent='Update OK! Rebooting...';m.className='ok';}
      else{m.textContent='Error: '+x.responseText;m.className='err';}
    }
  };
  x.open('POST','/update',true);
  x.send(f);
});
</script>
</body>
</html>
)rawhtml";

// ---------------------------------------------------------------------------
// Private helper methods
// ---------------------------------------------------------------------------
void OtaHAL::_setState(OtaState state, const char* msg) {
    _state = state;
    if (_stateCb) {
        _stateCb(state, msg ? msg : "");
    }
}

void OtaHAL::_setError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(_error, sizeof(_error), fmt, args);
    va_end(args);
    _setState(OtaState::ERROR, _error);
    DBG_ERROR(TAG, "%s", _error);
}

void OtaHAL::_reportProgress(size_t current, size_t total) {
    if (total > 0) {
        _progress = (uint8_t)((current * 100) / total);
    }
    if (_progressCb) {
        _progressCb(current, total);
    }
}

// ---------------------------------------------------------------------------
// HTTP server handlers (friend functions for private member access)
// ---------------------------------------------------------------------------
static void handleUpdatePage() {
    _server->sendHeader("Connection", "close");
    _server->send(200, "text/html", OTA_UPLOAD_PAGE);
}

void _otaHandleResult() {
    _server->sendHeader("Connection", "close");
    if (Update.hasError()) {
        _server->send(500, "text/plain", "Update failed");
    } else {
        _server->send(200, "text/plain", "OK");
        if (_instance) {
            _instance->_setState(OtaState::READY_REBOOT, "Update complete, ready to reboot");
            DBG_INFO(TAG, "Push OTA complete, ready to reboot");
        }
    }
}

void _otaHandleUpload() {
    HTTPUpload& upload = _server->upload();

    if (upload.status == UPLOAD_FILE_START) {
        DBG_INFO(TAG, "Push OTA start: %s", upload.filename.c_str());
        if (_instance) {
            _instance->_setState(OtaState::WRITING, "Receiving firmware");
            _instance->_progress = 0;
        }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            DBG_ERROR(TAG, "Update.begin failed: %s", Update.errorString());
            if (_instance) {
                _instance->_setError("Update.begin failed: %s", Update.errorString());
            }
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            DBG_ERROR(TAG, "Update.write failed: %s", Update.errorString());
            if (_instance) {
                _instance->_setError("Update.write failed: %s", Update.errorString());
            }
        } else if (_instance) {
            size_t written = Update.progress();
            size_t total = Update.size();
            _instance->_reportProgress(written, total > 0 ? total : written);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (_instance) {
            _instance->_setState(OtaState::VERIFYING, "Verifying firmware");
        }
        if (Update.end(true)) {
            DBG_INFO(TAG, "Push OTA success, %u bytes", upload.totalSize);
        } else {
            DBG_ERROR(TAG, "Update.end failed: %s", Update.errorString());
            if (_instance) {
                _instance->_setError("Update.end failed: %s", Update.errorString());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// OtaHAL implementation
// ---------------------------------------------------------------------------
bool OtaHAL::init() {
    _instance = this;
    _state = OtaState::IDLE;
    _progress = 0;
    _error[0] = '\0';

    // Read version from running app description
    const esp_app_desc_t* desc = esp_ota_get_app_description();
    if (desc) {
        snprintf(_version, sizeof(_version), "%s", desc->version);
    } else {
        strncpy(_version, "unknown", sizeof(_version));
    }

    DBG_INFO(TAG, "OTA HAL initialized, firmware version: %s", _version);
    return true;
}

bool OtaHAL::startServer(uint16_t port) {
    if (_serverRunning) {
        DBG_WARN(TAG, "OTA server already running");
        return true;
    }

    if (_server) {
        delete _server;
        _server = nullptr;
    }

    _server = new WebServer(port);
    _server->on("/update", HTTP_GET, handleUpdatePage);
    _server->on("/update", HTTP_POST, _otaHandleResult, _otaHandleUpload);
    _server->begin();
    _serverRunning = true;

    DBG_INFO(TAG, "OTA HTTP server started on port %u", port);
    return true;
}

void OtaHAL::stopServer() {
    if (_server) {
        _server->stop();
        delete _server;
        _server = nullptr;
    }
    _serverRunning = false;
    DBG_INFO(TAG, "OTA HTTP server stopped");
}

bool OtaHAL::isServerRunning() const {
    return _serverRunning;
}

bool OtaHAL::updateFromUrl(const char* url) {
    if (!url || strlen(url) == 0) {
        _setError("Invalid URL");
        return false;
    }

    DBG_INFO(TAG, "Pull OTA from URL: %s", url);
    _setState(OtaState::DOWNLOADING, "Downloading firmware");

    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        _setError("HTTP GET failed, code: %d", httpCode);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        _setError("Invalid content length: %d", contentLength);
        http.end();
        return false;
    }

    DBG_INFO(TAG, "Firmware size: %d bytes", contentLength);

    if (!Update.begin(contentLength)) {
        _setError("Not enough space for update: %s", Update.errorString());
        http.end();
        return false;
    }

    _setState(OtaState::WRITING, "Writing firmware to flash");

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    size_t written = 0;

    while (http.connected() && written < (size_t)contentLength) {
        size_t available = stream->available();
        if (available) {
            size_t toRead = (available < sizeof(buf)) ? available : sizeof(buf);
            size_t bytesRead = stream->readBytes(buf, toRead);
            size_t bytesWritten = Update.write(buf, bytesRead);
            if (bytesWritten != bytesRead) {
                _setError("Write failed at %u bytes: %s", written, Update.errorString());
                Update.abort();
                http.end();
                return false;
            }
            written += bytesWritten;
            _reportProgress(written, contentLength);
        }
        delay(1);
    }

    http.end();

    _setState(OtaState::VERIFYING, "Verifying firmware");

    if (!Update.end(true)) {
        _setError("Update verification failed: %s", Update.errorString());
        return false;
    }

    _setState(OtaState::READY_REBOOT, "Update complete, ready to reboot");
    DBG_INFO(TAG, "Pull OTA complete, %u bytes written", written);
    return true;
}

bool OtaHAL::updateFromSD(const char* path) {
    if (!path || strlen(path) == 0) {
        _setError("Invalid SD path");
        return false;
    }

    DBG_INFO(TAG, "SD OTA from: %s", path);
    _setState(OtaState::CHECKING, "Checking SD card");

    if (!SD_MMC.begin()) {
        _setError("SD card mount failed");
        return false;
    }

    File firmware = SD_MMC.open(path, FILE_READ);
    if (!firmware) {
        _setError("Cannot open %s", path);
        return false;
    }

    size_t fileSize = firmware.size();
    if (fileSize == 0) {
        _setError("Firmware file is empty");
        firmware.close();
        return false;
    }

    DBG_INFO(TAG, "Firmware file size: %u bytes", fileSize);

    if (!Update.begin(fileSize)) {
        _setError("Not enough space: %s", Update.errorString());
        firmware.close();
        return false;
    }

    _setState(OtaState::WRITING, "Writing firmware from SD");

    uint8_t buf[1024];
    size_t written = 0;

    while (firmware.available()) {
        size_t toRead = firmware.available();
        if (toRead > sizeof(buf)) toRead = sizeof(buf);
        size_t bytesRead = firmware.read(buf, toRead);
        size_t bytesWritten = Update.write(buf, bytesRead);
        if (bytesWritten != bytesRead) {
            _setError("Write failed at %u bytes: %s", written, Update.errorString());
            Update.abort();
            firmware.close();
            return false;
        }
        written += bytesWritten;
        _reportProgress(written, fileSize);
    }

    firmware.close();

    _setState(OtaState::VERIFYING, "Verifying firmware");

    if (!Update.end(true)) {
        _setError("Verification failed: %s", Update.errorString());
        return false;
    }

    // Rename firmware.bin to firmware.bin.bak
    char bakPath[128];
    snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
    SD_MMC.remove(bakPath);  // Remove old backup if present
    SD_MMC.rename(path, bakPath);
    DBG_INFO(TAG, "Renamed %s -> %s", path, bakPath);

    _setState(OtaState::READY_REBOOT, "SD update complete, ready to reboot");
    DBG_INFO(TAG, "SD OTA complete, %u bytes written", written);
    return true;
}

OtaState OtaHAL::getState() const {
    return _state;
}

uint8_t OtaHAL::getProgress() const {
    return _progress;
}

const char* OtaHAL::getLastError() const {
    return _error;
}

const char* OtaHAL::getCurrentVersion() const {
    return _version;
}

void OtaHAL::onProgress(OtaProgressCb cb) {
    _progressCb = cb;
}

void OtaHAL::onStateChange(OtaStateCb cb) {
    _stateCb = cb;
}

void OtaHAL::process() {
    if (_serverRunning && _server) {
        _server->handleClient();
    }
}

void OtaHAL::reboot() {
    DBG_INFO(TAG, "Rebooting...");
    delay(200);
    ESP.restart();
}

bool OtaHAL::rollback() {
    const esp_partition_t* prev = esp_ota_get_last_invalid_partition();
    if (!prev) {
        prev = esp_ota_get_next_update_partition(nullptr);
    }
    if (!prev) {
        _setError("No partition available for rollback");
        return false;
    }

    esp_err_t err = esp_ota_set_boot_partition(prev);
    if (err != ESP_OK) {
        _setError("Rollback failed: esp_err 0x%x", err);
        return false;
    }

    DBG_INFO(TAG, "Rollback set to partition: %s", prev->label);
    _setState(OtaState::READY_REBOOT, "Rollback ready, reboot to apply");
    return true;
}

bool OtaHAL::canRollback() const {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!running || !next) return false;
    // Can rollback if there is a different OTA partition available
    return (running != next);
}

const char* OtaHAL::getRunningPartition() const {
    const esp_partition_t* part = esp_ota_get_running_partition();
    return part ? part->label : "unknown";
}

size_t OtaHAL::getMaxFirmwareSize() const {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    return part ? part->size : 0;
}

OtaHAL::TestResult OtaHAL::runTest() {
    TestResult result = {};
    uint32_t startMs = millis();

    DBG_INFO(TAG, "--- OTA Test Begin ---");

    // Check dual OTA partitions
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    result.partition_ok = (running != nullptr && next != nullptr && running != next);
    result.running_partition = running ? running->label : "none";
    result.max_firmware_size = next ? next->size : 0;

    DBG_INFO(TAG, "Running partition: %s", result.running_partition);
    DBG_INFO(TAG, "Next OTA partition: %s (%u bytes)",
             next ? next->label : "none", result.max_firmware_size);
    DBG_INFO(TAG, "Dual OTA partitions: %s", result.partition_ok ? "YES" : "NO");

    // Test server start/stop
    bool wasRunning = _serverRunning;
    result.server_start_ok = startServer(8079);  // Use non-default port for test
    if (result.server_start_ok) {
        DBG_INFO(TAG, "Server start: OK");
        stopServer();
        result.server_stop_ok = !_serverRunning;
        DBG_INFO(TAG, "Server stop: %s", result.server_stop_ok ? "OK" : "FAIL");
    } else {
        DBG_ERROR(TAG, "Server start: FAIL");
        result.server_stop_ok = false;
    }

    // Restore previous server state if it was running
    if (wasRunning) {
        startServer();
    }

    // Test rollback API
    result.rollback_check_ok = (running != nullptr);
    DBG_INFO(TAG, "Rollback API: %s, canRollback: %s",
             result.rollback_check_ok ? "OK" : "FAIL",
             canRollback() ? "yes" : "no");

    // Test SD card access
    if (SD_MMC.begin()) {
        result.sd_detect_ok = true;
        DBG_INFO(TAG, "SD card: detected (%llu MB total)",
                 SD_MMC.totalBytes() / (1024 * 1024));
    } else {
        result.sd_detect_ok = false;
        DBG_INFO(TAG, "SD card: not detected");
    }

    result.test_duration_ms = millis() - startMs;
    DBG_INFO(TAG, "--- OTA Test End (%u ms) ---", result.test_duration_ms);

    return result;
}

// ============================================================================
// Platform: Desktop Simulator
// ============================================================================
#else // SIMULATOR

void OtaHAL::_setState(OtaState, const char*) {}
void OtaHAL::_setError(const char*, ...) {}
void OtaHAL::_reportProgress(size_t, size_t) {}

bool OtaHAL::init() {
    DBG_INFO(TAG, "OTA HAL init (simulator stub)");
    strncpy(_version, "sim-0.0.0", sizeof(_version));
    return false;
}

bool OtaHAL::startServer(uint16_t) { return false; }
void OtaHAL::stopServer() {}
bool OtaHAL::isServerRunning() const { return false; }
bool OtaHAL::updateFromUrl(const char*) { return false; }
bool OtaHAL::updateFromSD(const char*) { return false; }

OtaState OtaHAL::getState() const { return OtaState::IDLE; }
uint8_t OtaHAL::getProgress() const { return 0; }
const char* OtaHAL::getLastError() const { return _error; }
const char* OtaHAL::getCurrentVersion() const { return _version; }

void OtaHAL::onProgress(OtaProgressCb cb) { _progressCb = cb; }
void OtaHAL::onStateChange(OtaStateCb cb) { _stateCb = cb; }
void OtaHAL::process() {}
void OtaHAL::reboot() {}
bool OtaHAL::rollback() { return false; }
bool OtaHAL::canRollback() const { return false; }
const char* OtaHAL::getRunningPartition() const { return "simulator"; }
size_t OtaHAL::getMaxFirmwareSize() const { return 0; }

OtaHAL::TestResult OtaHAL::runTest() {
    TestResult r = {};
    r.running_partition = "simulator";
    return r;
}

#endif // SIMULATOR
