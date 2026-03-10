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

#include "tritium_compat.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include <esp_partition.h>
#include <sys/stat.h>
#include <dirent.h>

static httpd_handle_t _httpd = nullptr;
static OtaHAL* _instance = nullptr;

// OTA session state for chunked upload
static esp_ota_handle_t _upload_handle = 0;
static const esp_partition_t* _upload_partition = nullptr;
static size_t _upload_written = 0;
static bool _upload_started = false;
static bool _upload_error = false;

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
// HTTP server handlers (ESP-IDF httpd)
// ---------------------------------------------------------------------------

static esp_err_t handleUpdatePageGet(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, OTA_UPLOAD_PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Multipart boundary parser helper
static bool findBoundaryEnd(const char* buf, int len, const char* boundary, int blen, int* dataStart) {
    // Search for \r\n\r\n after the boundary+headers to find where binary data starts
    for (int i = 0; i <= len - 4; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            *dataStart = i + 4;
            return true;
        }
    }
    return false;
}

esp_err_t handleUpdatePost(httpd_req_t* req) {
    esp_err_t err;
    char buf[1024];
    int received;
    int remaining = req->content_len;
    bool headersParsed = false;
    int dataStart = 0;

    // Reset upload state
    _upload_started = false;
    _upload_error = false;
    _upload_written = 0;
    _upload_handle = 0;
    _upload_partition = esp_ota_get_next_update_partition(NULL);

    if (!_upload_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    if (_instance) {
        _instance->_setState(OtaState::WRITING, "Receiving firmware");
        _instance->_progress = 0;
    }

    while (remaining > 0) {
        int toRead = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        received = httpd_req_recv(req, buf, toRead);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  // Retry on timeout
            }
            DBG_ERROR(TAG, "httpd_req_recv error: %d", received);
            if (_upload_started) {
                esp_ota_abort(_upload_handle);
            }
            if (_instance) {
                _instance->_setError("Connection lost during upload");
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        remaining -= received;

        const uint8_t* writePtr = (const uint8_t*)buf;
        size_t writeLen = (size_t)received;

        // Skip multipart headers on first chunk
        if (!headersParsed) {
            if (findBoundaryEnd(buf, received, nullptr, 0, &dataStart)) {
                headersParsed = true;
                writePtr = (const uint8_t*)buf + dataStart;
                writeLen = (size_t)(received - dataStart);
            } else {
                // Headers span multiple chunks — unlikely for typical form upload
                // but handle by skipping until we find \r\n\r\n
                continue;
            }
        }

        // Begin OTA on first data chunk
        if (!_upload_started && writeLen > 0) {
            err = esp_ota_begin(_upload_partition, OTA_SIZE_UNKNOWN, &_upload_handle);
            if (err != ESP_OK) {
                DBG_ERROR(TAG, "esp_ota_begin failed: 0x%x", err);
                if (_instance) {
                    _instance->_setError("esp_ota_begin failed: 0x%x", err);
                }
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
                return ESP_FAIL;
            }
            _upload_started = true;
        }

        // Strip trailing multipart boundary from last chunk
        // The boundary ends with \r\n--<boundary>--\r\n
        // We detect this by checking if remaining == 0 (last read)
        if (remaining == 0 && writeLen > 2) {
            // Search backwards for \r\n-- which starts the closing boundary
            for (int i = (int)writeLen - 1; i >= 3; i--) {
                if (writePtr[i-3] == '\r' && writePtr[i-2] == '\n' &&
                    writePtr[i-1] == '-' && writePtr[i] == '-') {
                    writeLen = (size_t)(i - 3);
                    break;
                }
            }
        }

        if (writeLen > 0 && _upload_started) {
            err = esp_ota_write(_upload_handle, writePtr, writeLen);
            if (err != ESP_OK) {
                DBG_ERROR(TAG, "esp_ota_write failed: 0x%x", err);
                esp_ota_abort(_upload_handle);
                _upload_started = false;
                if (_instance) {
                    _instance->_setError("esp_ota_write failed: 0x%x", err);
                }
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
                return ESP_FAIL;
            }
            _upload_written += writeLen;
            if (_instance) {
                _instance->_reportProgress(_upload_written, _upload_written);
            }
        }
    }

    if (!_upload_started) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware data received");
        return ESP_FAIL;
    }

    // Finalize OTA
    if (_instance) {
        _instance->_setState(OtaState::VERIFYING, "Verifying firmware");
    }

    err = esp_ota_end(_upload_handle);
    if (err != ESP_OK) {
        DBG_ERROR(TAG, "esp_ota_end failed: 0x%x", err);
        if (_instance) {
            _instance->_setError("esp_ota_end failed: 0x%x", err);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Verification failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(_upload_partition);
    if (err != ESP_OK) {
        DBG_ERROR(TAG, "esp_ota_set_boot_partition failed: 0x%x", err);
        if (_instance) {
            _instance->_setError("Set boot partition failed: 0x%x", err);
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "OK");

    if (_instance) {
        _instance->_setState(OtaState::READY_REBOOT, "Update complete, ready to reboot");
        DBG_INFO(TAG, "Push OTA complete, %u bytes, ready to reboot", (unsigned)_upload_written);
    }

    return ESP_OK;
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

    if (_httpd) {
        httpd_stop(_httpd);
        _httpd = nullptr;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&_httpd, &config);
    if (err != ESP_OK) {
        DBG_ERROR(TAG, "httpd_start failed: 0x%x", err);
        return false;
    }

    // Register GET /update — serve upload page
    httpd_uri_t get_uri = {
        .uri      = "/update",
        .method   = HTTP_GET,
        .handler  = handleUpdatePageGet,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(_httpd, &get_uri);

    // Register POST /update — receive firmware
    httpd_uri_t post_uri = {
        .uri      = "/update",
        .method   = HTTP_POST,
        .handler  = handleUpdatePost,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(_httpd, &post_uri);

    _serverRunning = true;
    DBG_INFO(TAG, "OTA HTTP server started on port %u", port);
    return true;
}

void OtaHAL::stopServer() {
    if (_httpd) {
        httpd_stop(_httpd);
        _httpd = nullptr;
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

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 30000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        _setError("HTTP client init failed");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        _setError("HTTP open failed: 0x%x", err);
        esp_http_client_cleanup(client);
        return false;
    }

    int contentLength = esp_http_client_fetch_headers(client);
    int statusCode = esp_http_client_get_status_code(client);

    if (statusCode != 200) {
        _setError("HTTP GET failed, code: %d", statusCode);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    if (contentLength <= 0) {
        _setError("Invalid content length: %d", contentLength);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    DBG_INFO(TAG, "Firmware size: %d bytes", contentLength);

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        _setError("No OTA partition available");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_ota_handle_t update_handle;
    err = esp_ota_begin(update_partition, (size_t)contentLength, &update_handle);
    if (err != ESP_OK) {
        _setError("esp_ota_begin failed: 0x%x", err);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    _setState(OtaState::WRITING, "Writing firmware to flash");

    uint8_t buf[1024];
    size_t written = 0;

    while (written < (size_t)contentLength) {
        int bytesRead = esp_http_client_read(client, (char*)buf, sizeof(buf));
        if (bytesRead < 0) {
            _setError("HTTP read failed at %u bytes", (unsigned)written);
            esp_ota_abort(update_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (bytesRead == 0) {
            // Connection closed prematurely
            if (written < (size_t)contentLength) {
                _setError("Connection closed at %u/%d bytes", (unsigned)written, contentLength);
                esp_ota_abort(update_handle);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return false;
            }
            break;
        }

        err = esp_ota_write(update_handle, buf, (size_t)bytesRead);
        if (err != ESP_OK) {
            _setError("esp_ota_write failed at %u bytes: 0x%x", (unsigned)written, err);
            esp_ota_abort(update_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        written += (size_t)bytesRead;
        _reportProgress(written, (size_t)contentLength);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    _setState(OtaState::VERIFYING, "Verifying firmware");

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        _setError("esp_ota_end failed: 0x%x", err);
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        _setError("Set boot partition failed: 0x%x", err);
        return false;
    }

    _setState(OtaState::READY_REBOOT, "Update complete, ready to reboot");
    DBG_INFO(TAG, "Pull OTA complete, %u bytes written", (unsigned)written);
    return true;
}

bool OtaHAL::updateFromSD(const char* path) {
    if (!path || strlen(path) == 0) {
        _setError("Invalid SD path");
        return false;
    }

    // Build full VFS path: /sdcard/<path>
    char fullPath[160];
    if (path[0] == '/') {
        snprintf(fullPath, sizeof(fullPath), "/sdcard%s", path);
    } else {
        snprintf(fullPath, sizeof(fullPath), "/sdcard/%s", path);
    }

    DBG_INFO(TAG, "SD OTA from: %s", fullPath);
    _setState(OtaState::CHECKING, "Checking SD card");

    // Check file exists and get size via stat
    struct stat st;
    if (stat(fullPath, &st) != 0) {
        _setError("Cannot stat %s", fullPath);
        return false;
    }

    size_t fileSize = (size_t)st.st_size;
    if (fileSize == 0) {
        _setError("Firmware file is empty");
        return false;
    }

    FILE* firmware = fopen(fullPath, "rb");
    if (!firmware) {
        _setError("Cannot open %s", fullPath);
        return false;
    }

    DBG_INFO(TAG, "Firmware file size: %u bytes", (unsigned)fileSize);

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        _setError("No OTA partition available");
        fclose(firmware);
        return false;
    }

    esp_ota_handle_t update_handle;
    esp_err_t err = esp_ota_begin(update_partition, fileSize, &update_handle);
    if (err != ESP_OK) {
        _setError("esp_ota_begin failed: 0x%x", err);
        fclose(firmware);
        return false;
    }

    _setState(OtaState::WRITING, "Writing firmware from SD");

    uint8_t buf[1024];
    size_t written = 0;

    while (written < fileSize) {
        size_t toRead = fileSize - written;
        if (toRead > sizeof(buf)) toRead = sizeof(buf);
        size_t bytesRead = fread(buf, 1, toRead, firmware);
        if (bytesRead == 0) {
            _setError("Read failed at %u bytes", (unsigned)written);
            esp_ota_abort(update_handle);
            fclose(firmware);
            return false;
        }

        err = esp_ota_write(update_handle, buf, bytesRead);
        if (err != ESP_OK) {
            _setError("esp_ota_write failed at %u bytes: 0x%x", (unsigned)written, err);
            esp_ota_abort(update_handle);
            fclose(firmware);
            return false;
        }
        written += bytesRead;
        _reportProgress(written, fileSize);
    }

    fclose(firmware);

    _setState(OtaState::VERIFYING, "Verifying firmware");

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        _setError("esp_ota_end failed: 0x%x", err);
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        _setError("Set boot partition failed: 0x%x", err);
        return false;
    }

    // Rename firmware.bin to firmware.bin.bak
    char bakPath[176];
    snprintf(bakPath, sizeof(bakPath), "%s.bak", fullPath);
    remove(bakPath);  // Remove old backup if present
    rename(fullPath, bakPath);
    DBG_INFO(TAG, "Renamed %s -> %s", fullPath, bakPath);

    _setState(OtaState::READY_REBOOT, "SD update complete, ready to reboot");
    DBG_INFO(TAG, "SD OTA complete, %u bytes written", (unsigned)written);
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
    // ESP-IDF httpd runs in its own task — no polling needed
    (void)_serverRunning;
}

void OtaHAL::reboot() {
    DBG_INFO(TAG, "Rebooting...");
    delay(200);
    esp_restart();
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
             next ? next->label : "none", (unsigned)result.max_firmware_size);
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

    // Test SD card access via VFS
    struct stat sd_stat;
    if (stat("/sdcard", &sd_stat) == 0) {
        result.sd_detect_ok = true;
        DBG_INFO(TAG, "SD card: detected (VFS mounted at /sdcard)");
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
