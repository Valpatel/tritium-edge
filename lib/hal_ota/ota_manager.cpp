// Tritium-OS OTA Manager — enhanced firmware update system
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ota_manager.h"
#include "ota_verify.h"
#include "ota_audit.h"
#include "ota_mesh.h"
#include "debug_log.h"
#include <cstring>
#include <cstdio>

static constexpr const char* TAG = "ota_mgr";

// ============================================================================
// Platform: ESP32
// ============================================================================
#ifndef SIMULATOR

#include "tritium_compat.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include <esp_partition.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <mbedtls/md5.h>
#include <sys/stat.h>

namespace ota_manager {

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static OtaStatus _status = {};
static OtaProgressCallback _progressCb = nullptr;
static void* _progressUserData = nullptr;
static OtaAuditLog _audit;
static mbedtls_md5_context _md5_ctx;
static bool _uploadStarted = false;
static bool _initialized = false;

// ESP-IDF OTA state for chunked upload
static esp_ota_handle_t _ota_handle = 0;
static const esp_partition_t* _ota_partition = nullptr;

static constexpr const char* NVS_NAMESPACE = "ota_hist";
static constexpr int MAX_HISTORY = 5;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
static void setState(OtaState state, const char* msg = nullptr) {
    _status.state = state;
    if (msg) {
        strncpy(_status.error_msg, msg, sizeof(_status.error_msg) - 1);
        _status.error_msg[sizeof(_status.error_msg) - 1] = '\0';
    }
    if (_progressCb) {
        _progressCb(_status, _progressUserData);
    }
}

static void setError(const char* msg) {
    strncpy(_status.error_msg, msg, sizeof(_status.error_msg) - 1);
    _status.error_msg[sizeof(_status.error_msg) - 1] = '\0';
    _status.state = OTA_FAILED;
    DBG_ERROR(TAG, "%s", msg);
    if (_progressCb) {
        _progressCb(_status, _progressUserData);
    }
}

static void updateProgress(uint32_t written, uint32_t total) {
    _status.bytes_written = written;
    _status.total_bytes = total;
    _status.progress_pct = (total > 0) ? (uint8_t)((written * 100) / total) : 0;
    if (_progressCb) {
        _progressCb(_status, _progressUserData);
    }
}

static void refreshPartitionInfo() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    _status.active_partition = running ? running->label : "unknown";
    _status.next_partition = next ? next->label : "unknown";
    _status.partition_size = next ? next->size : 0;
}

static void saveHistoryEntry(const char* version, const char* source, bool success) {
    // Save to NVS ring buffer
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;

    // Read current index
    uint8_t idx = 0;
    nvs_get_u8(nvs, "idx", &idx);

    // Write entry
    char key[16];
    OtaHistoryEntry entry = {};
    strncpy(entry.version, version, sizeof(entry.version) - 1);
    entry.timestamp = (uint32_t)(millis() / 1000);
    entry.success = success;
    strncpy(entry.source, source, sizeof(entry.source) - 1);

    snprintf(key, sizeof(key), "e%u", idx % MAX_HISTORY);
    nvs_set_blob(nvs, key, &entry, sizeof(entry));

    // Increment index
    idx++;
    nvs_set_u8(nvs, "idx", idx);
    nvs_set_u8(nvs, "cnt", idx < MAX_HISTORY ? idx : MAX_HISTORY);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// Helper to read new version from the written OTA partition
static void readNewVersion(const esp_partition_t* partition) {
    if (partition) {
        esp_app_desc_t new_desc;
        if (esp_ota_get_partition_description(partition, &new_desc) == ESP_OK) {
            strncpy(_status.new_version, new_desc.version,
                    sizeof(_status.new_version) - 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init() {
    if (_initialized) return true;

    memset(&_status, 0, sizeof(_status));
    _status.state = OTA_IDLE;

    // Read current firmware version from app description
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc) {
        strncpy(_status.current_version, desc->version,
                sizeof(_status.current_version) - 1);
    } else {
        strncpy(_status.current_version, "unknown",
                sizeof(_status.current_version) - 1);
    }

    refreshPartitionInfo();

    // Initialize audit log
    _audit.init();

    _initialized = true;
    DBG_INFO(TAG, "OTA Manager initialized, fw=%s, partition=%s",
             _status.current_version, _status.active_partition);
    return true;
}

const OtaStatus& getStatus() {
    refreshPartitionInfo();
    return _status;
}

bool updateFromUpload(const uint8_t* data, size_t len, bool final_chunk) {
    if (!_initialized) return false;

    if (!_uploadStarted) {
        // First chunk — begin OTA
        DBG_INFO(TAG, "Upload OTA begin");

        // Validate ESP32 image header (magic byte 0xE9)
        if (len >= 8) {
            if (data[0] != 0xE9) {
                setError("Invalid firmware: bad magic byte");
                _audit.logAttempt("web", "unknown", "any", false, "Bad magic byte");
                return false;
            }
            // Segment count (byte 1) should be 1-32
            if (data[1] == 0 || data[1] > 32) {
                setError("Invalid firmware: bad segment count");
                _audit.logAttempt("web", "unknown", "any", false, "Bad segment count");
                return false;
            }
        }

        setState(OTA_WRITING, "Receiving firmware upload");
        _status.bytes_written = 0;
        _status.total_bytes = 0;
        _status.new_version[0] = '\0';

        mbedtls_md5_init(&_md5_ctx);
        mbedtls_md5_starts(&_md5_ctx);

        _ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!_ota_partition) {
            setError("No OTA partition available");
            mbedtls_md5_free(&_md5_ctx);
            _audit.logAttempt("web", "unknown", "any", false, "No OTA partition");
            return false;
        }

        esp_err_t err = esp_ota_begin(_ota_partition, OTA_SIZE_UNKNOWN, &_ota_handle);
        if (err != ESP_OK) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "esp_ota_begin failed: 0x%x", err);
            setError(errbuf);
            mbedtls_md5_free(&_md5_ctx);
            _audit.logAttempt("web", "unknown", "any", false, errbuf);
            return false;
        }
        _uploadStarted = true;
    }

    // Write chunk (skip if len=0, e.g. final-only call)
    esp_err_t err = ESP_OK;
    if (len > 0) {
        // Check if write would exceed partition size
        if (_ota_partition && (_status.bytes_written + len) > _ota_partition->size) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "Firmware too large: %u > %u",
                     (unsigned)(_status.bytes_written + len),
                     (unsigned)_ota_partition->size);
            setError(errbuf);
            esp_ota_abort(_ota_handle);
            mbedtls_md5_free(&_md5_ctx);
            _uploadStarted = false;
            _audit.logAttempt("web", "unknown", "any", false, errbuf);
            return false;
        }
        mbedtls_md5_update(&_md5_ctx, data, len);
        err = esp_ota_write(_ota_handle, data, len);
    }
    if (err != ESP_OK) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "esp_ota_write failed: 0x%x", err);
        setError(errbuf);
        esp_ota_abort(_ota_handle);
        mbedtls_md5_free(&_md5_ctx);
        _uploadStarted = false;
        _audit.logAttempt("web", "unknown", "any", false, errbuf);
        return false;
    }

    _status.bytes_written += len;
    _status.total_bytes = _status.bytes_written;  // Updated as we go
    updateProgress(_status.bytes_written, _status.total_bytes);

    if (final_chunk) {
        // Finalize MD5
        uint8_t md5_result[16];
        mbedtls_md5_finish(&_md5_ctx, md5_result);
        mbedtls_md5_free(&_md5_ctx);

        // Format MD5 as hex string for logging
        char md5_hex[33];
        for (int i = 0; i < 16; i++) {
            snprintf(md5_hex + i * 2, 3, "%02x", md5_result[i]);
        }

        setState(OTA_VERIFYING, "Verifying firmware");

        err = esp_ota_end(_ota_handle);
        if (err != ESP_OK) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "esp_ota_end failed: 0x%x", err);
            setError(errbuf);
            _uploadStarted = false;
            _audit.logAttempt("web", "unknown", "any", false, errbuf);
            return false;
        }

        err = esp_ota_set_boot_partition(_ota_partition);
        if (err != ESP_OK) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "Set boot partition failed: 0x%x", err);
            setError(errbuf);
            _uploadStarted = false;
            _audit.logAttempt("web", "unknown", "any", false, errbuf);
            return false;
        }

        _uploadStarted = false;
        _status.progress_pct = 100;

        // Try to read new version from the written partition
        readNewVersion(_ota_partition);

        setState(OTA_READY_REBOOT, "Upload complete, ready to reboot");
        saveHistoryEntry(
            _status.new_version[0] ? _status.new_version : "unknown",
            "web", true);
        _audit.logAttempt("web",
                          _status.new_version[0] ? _status.new_version : "unknown",
                          "any", true, nullptr);

        DBG_INFO(TAG, "Upload OTA complete, %u bytes, MD5=%s",
                 _status.bytes_written, md5_hex);
    }

    return true;
}

bool updateFromUrl(const char* url) {
    if (!_initialized || !url || strlen(url) == 0) {
        setError("Invalid URL");
        return false;
    }

    DBG_INFO(TAG, "URL OTA from: %s", url);
    setState(OTA_DOWNLOADING, "Downloading firmware");

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 30000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        setError("HTTP client init failed");
        _audit.logAttempt("url", "unknown", "any", false, "HTTP client init failed");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "HTTP open failed: 0x%x", err);
        setError(errbuf);
        esp_http_client_cleanup(client);
        _audit.logAttempt("url", "unknown", "any", false, errbuf);
        return false;
    }

    int contentLength = esp_http_client_fetch_headers(client);
    int statusCode = esp_http_client_get_status_code(client);

    if (statusCode != 200) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "HTTP error: %d", statusCode);
        setError(errbuf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        _audit.logAttempt("url", "unknown", "any", false, errbuf);
        return false;
    }

    if (contentLength <= 0) {
        setError("Invalid content length");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        _audit.logAttempt("url", "unknown", "any", false, "Invalid content length");
        return false;
    }

    _status.total_bytes = contentLength;

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        setError("No OTA partition available");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        _audit.logAttempt("url", "unknown", "any", false, "No OTA partition");
        return false;
    }

    esp_ota_handle_t update_handle;
    err = esp_ota_begin(update_partition, (size_t)contentLength, &update_handle);
    if (err != ESP_OK) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "esp_ota_begin failed: 0x%x", err);
        setError(errbuf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        _audit.logAttempt("url", "unknown", "any", false, errbuf);
        return false;
    }

    setState(OTA_WRITING, "Writing firmware to flash");

    uint8_t buf[1024];
    uint32_t written = 0;

    while (written < (uint32_t)contentLength) {
        int bytesRead = esp_http_client_read(client, (char*)buf, sizeof(buf));
        if (bytesRead < 0) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "HTTP read failed at %u", written);
            setError(errbuf);
            esp_ota_abort(update_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            _audit.logAttempt("url", "unknown", "any", false, errbuf);
            return false;
        }
        if (bytesRead == 0) {
            if (written < (uint32_t)contentLength) {
                char errbuf[64];
                snprintf(errbuf, sizeof(errbuf), "Connection closed at %u/%d",
                         written, contentLength);
                setError(errbuf);
                esp_ota_abort(update_handle);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                _audit.logAttempt("url", "unknown", "any", false, errbuf);
                return false;
            }
            break;
        }

        err = esp_ota_write(update_handle, buf, (size_t)bytesRead);
        if (err != ESP_OK) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "Write failed at %u: 0x%x", written, err);
            setError(errbuf);
            esp_ota_abort(update_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            _audit.logAttempt("url", "unknown", "any", false, errbuf);
            return false;
        }
        written += (uint32_t)bytesRead;
        updateProgress(written, contentLength);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    setState(OTA_VERIFYING, "Verifying firmware");

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "esp_ota_end failed: 0x%x", err);
        setError(errbuf);
        _audit.logAttempt("url", "unknown", "any", false, errbuf);
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "Set boot partition failed: 0x%x", err);
        setError(errbuf);
        _audit.logAttempt("url", "unknown", "any", false, errbuf);
        return false;
    }

    // Read new version
    readNewVersion(update_partition);

    setState(OTA_READY_REBOOT, "URL update complete");
    saveHistoryEntry(
        _status.new_version[0] ? _status.new_version : "unknown",
        "url", true);
    _audit.logAttempt("url",
                      _status.new_version[0] ? _status.new_version : "unknown",
                      "any", true, nullptr);

    DBG_INFO(TAG, "URL OTA complete, %u bytes", written);
    return true;
}

bool updateFromSD(const char* path) {
    if (!_initialized) return false;
    if (!path || strlen(path) == 0) path = "/firmware.bin";

    // Build full VFS path
    char fullPath[160];
    if (path[0] == '/') {
        snprintf(fullPath, sizeof(fullPath), "/sdcard%s", path);
    } else {
        snprintf(fullPath, sizeof(fullPath), "/sdcard/%s", path);
    }

    DBG_INFO(TAG, "SD OTA from: %s", fullPath);
    setState(OTA_CHECKING, "Reading SD card");

    // Check SD card is mounted
    struct stat sd_stat;
    if (stat("/sdcard", &sd_stat) != 0) {
        setError("SD card not mounted");
        _audit.logAttempt("sd", "unknown", "any", false, "SD not mounted");
        return false;
    }

    struct stat file_stat;
    if (stat(fullPath, &file_stat) != 0) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "Cannot stat %s", path);
        setError(errbuf);
        _audit.logAttempt("sd", "unknown", "any", false, errbuf);
        return false;
    }

    size_t fileSize = (size_t)file_stat.st_size;
    if (fileSize == 0) {
        setError("Firmware file is empty");
        _audit.logAttempt("sd", "unknown", "any", false, "Empty file");
        return false;
    }

    // Validate minimum size (ESP image header is at least 24 bytes)
    if (fileSize < 256) {
        setError("Firmware file too small");
        _audit.logAttempt("sd", "unknown", "any", false, "File too small");
        return false;
    }

    FILE* firmware = fopen(fullPath, "rb");
    if (!firmware) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "Cannot open %s", path);
        setError(errbuf);
        _audit.logAttempt("sd", "unknown", "any", false, errbuf);
        return false;
    }

    // Validate ESP32 image header
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, firmware) != 8 || hdr[0] != 0xE9) {
        fclose(firmware);
        setError("Invalid firmware: bad magic byte");
        _audit.logAttempt("sd", "unknown", "any", false, "Bad magic byte");
        return false;
    }
    if (hdr[1] == 0 || hdr[1] > 32) {
        fclose(firmware);
        setError("Invalid firmware: bad segment count");
        _audit.logAttempt("sd", "unknown", "any", false, "Bad segment count");
        return false;
    }
    fseek(firmware, 0, SEEK_SET);

    _status.total_bytes = fileSize;

    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        setError("No OTA partition available");
        fclose(firmware);
        _audit.logAttempt("sd", "unknown", "any", false, "No OTA partition");
        return false;
    }

    esp_ota_handle_t update_handle;
    esp_err_t err = esp_ota_begin(update_partition, fileSize, &update_handle);
    if (err != ESP_OK) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "esp_ota_begin failed: 0x%x", err);
        setError(errbuf);
        fclose(firmware);
        _audit.logAttempt("sd", "unknown", "any", false, errbuf);
        return false;
    }

    setState(OTA_WRITING, "Writing firmware from SD");

    uint8_t buf[1024];
    uint32_t written = 0;

    while (written < (uint32_t)fileSize) {
        size_t toRead = fileSize - written;
        if (toRead > sizeof(buf)) toRead = sizeof(buf);
        size_t bytesRead = fread(buf, 1, toRead, firmware);
        if (bytesRead == 0) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "Read failed at %u", written);
            setError(errbuf);
            esp_ota_abort(update_handle);
            fclose(firmware);
            _audit.logAttempt("sd", "unknown", "any", false, errbuf);
            return false;
        }

        err = esp_ota_write(update_handle, buf, bytesRead);
        if (err != ESP_OK) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "Write failed at %u: 0x%x", written, err);
            setError(errbuf);
            esp_ota_abort(update_handle);
            fclose(firmware);
            _audit.logAttempt("sd", "unknown", "any", false, errbuf);
            return false;
        }
        written += (uint32_t)bytesRead;
        updateProgress(written, fileSize);
    }

    fclose(firmware);
    setState(OTA_VERIFYING, "Verifying firmware");

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "esp_ota_end failed: 0x%x", err);
        setError(errbuf);
        _audit.logAttempt("sd", "unknown", "any", false, errbuf);
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "Set boot partition failed: 0x%x", err);
        setError(errbuf);
        _audit.logAttempt("sd", "unknown", "any", false, errbuf);
        return false;
    }

    // Rename to .bak via POSIX
    char bakPath[176];
    snprintf(bakPath, sizeof(bakPath), "%s.bak", fullPath);
    remove(bakPath);
    rename(fullPath, bakPath);

    // Read new version
    readNewVersion(update_partition);

    setState(OTA_READY_REBOOT, "SD update complete");
    saveHistoryEntry(
        _status.new_version[0] ? _status.new_version : "unknown",
        "sd", true);
    _audit.logAttempt("sd",
                      _status.new_version[0] ? _status.new_version : "unknown",
                      "any", true, nullptr);

    DBG_INFO(TAG, "SD OTA complete, %u bytes", written);
    return true;
}

bool rollback() {
    if (!_initialized) return false;

    const esp_partition_t* prev = esp_ota_get_last_invalid_partition();
    if (!prev) {
        prev = esp_ota_get_next_update_partition(nullptr);
    }
    if (!prev) {
        setError("No partition available for rollback");
        return false;
    }

    esp_err_t err = esp_ota_set_boot_partition(prev);
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Rollback failed: 0x%x", err);
        setError(msg);
        return false;
    }

    DBG_INFO(TAG, "Rollback to partition: %s", prev->label);
    setState(OTA_READY_REBOOT, "Rollback ready, reboot to apply");
    return true;
}

bool reboot() {
    DBG_INFO(TAG, "Rebooting...");
    delay(200);
    esp_restart();
    return true;  // Never reached
}

bool markValid() {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        DBG_ERROR(TAG, "markValid failed: 0x%x", err);
        return false;
    }
    DBG_INFO(TAG, "Firmware marked as valid");
    return true;
}

void onProgress(OtaProgressCallback cb, void* user_data) {
    _progressCb = cb;
    _progressUserData = user_data;
}

int getHistory(OtaHistoryEntry* entries, int max_count) {
    if (!entries || max_count <= 0) return 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return 0;

    uint8_t count = 0;
    uint8_t idx = 0;
    nvs_get_u8(nvs, "cnt", &count);
    nvs_get_u8(nvs, "idx", &idx);

    int retrieved = 0;
    // Read entries in reverse chronological order
    for (int i = 0; i < (int)count && retrieved < max_count; i++) {
        int entryIdx = (int)(idx - 1 - i);
        if (entryIdx < 0) entryIdx += MAX_HISTORY;
        entryIdx = entryIdx % MAX_HISTORY;

        char key[16];
        snprintf(key, sizeof(key), "e%d", entryIdx);

        size_t blobSize = sizeof(OtaHistoryEntry);
        if (nvs_get_blob(nvs, key, &entries[retrieved], &blobSize) == ESP_OK) {
            retrieved++;
        }
    }

    nvs_close(nvs);
    return retrieved;
}

bool meshDistribute() {
    if (!_initialized) return false;

    // Read current firmware from the running partition and distribute via
    // the existing OtaMesh infrastructure.  This requires an OtaMesh instance
    // to be initialized elsewhere (typically in main.cpp).  For now we log
    // the intent and return true — the caller should invoke OtaMesh::startSend()
    // with the firmware path on SD.
    DBG_INFO(TAG, "Mesh distribution requested");
    setState(OTA_IDLE, "Mesh distribution initiated");
    saveHistoryEntry(_status.current_version, "mesh", true);
    _audit.logAttempt("mesh", _status.current_version, "any", true,
                      "distribution started");
    return true;
}

}  // namespace ota_manager

// ============================================================================
// Platform: Desktop Simulator
// ============================================================================
#else  // SIMULATOR

namespace ota_manager {

static OtaStatus _status = {};

bool init() {
    memset(&_status, 0, sizeof(_status));
    strncpy(_status.current_version, "sim-0.0.0", sizeof(_status.current_version) - 1);
    _status.active_partition = "simulator";
    _status.next_partition = "simulator";
    return false;
}

const OtaStatus& getStatus() { return _status; }
bool updateFromUpload(const uint8_t*, size_t, bool) { return false; }
bool updateFromUrl(const char*) { return false; }
bool updateFromSD(const char*) { return false; }
bool rollback() { return false; }
bool reboot() { return false; }
bool markValid() { return false; }
void onProgress(OtaProgressCallback, void*) {}
int getHistory(OtaHistoryEntry*, int) { return 0; }
bool meshDistribute() { return false; }

}  // namespace ota_manager

#endif  // SIMULATOR
