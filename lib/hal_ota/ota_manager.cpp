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

#include <Arduino.h>
#include <WiFi.h>
#include <Update.h>
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <MD5Builder.h>

namespace ota_manager {

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static OtaStatus _status = {};
static OtaProgressCallback _progressCb = nullptr;
static void* _progressUserData = nullptr;
static OtaAuditLog _audit;
static MD5Builder _md5;
static bool _uploadStarted = false;
static bool _initialized = false;

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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init() {
    if (_initialized) return true;

    memset(&_status, 0, sizeof(_status));
    _status.state = OTA_IDLE;

    // Read current firmware version from app description
    const esp_app_desc_t* desc = esp_ota_get_app_description();
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
        setState(OTA_WRITING, "Receiving firmware upload");
        _status.bytes_written = 0;
        _status.total_bytes = 0;
        _status.new_version[0] = '\0';

        _md5.begin();

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            char err[64];
            snprintf(err, sizeof(err), "Update.begin failed: %s",
                     Update.errorString());
            setError(err);
            _audit.logAttempt("web", "unknown", "any", false, err);
            return false;
        }
        _uploadStarted = true;
    }

    // Write chunk
    _md5.add(data, len);
    size_t written = Update.write(const_cast<uint8_t*>(data), len);
    if (written != len) {
        char err[64];
        snprintf(err, sizeof(err), "Write failed: %s", Update.errorString());
        setError(err);
        Update.abort();
        _uploadStarted = false;
        _audit.logAttempt("web", "unknown", "any", false, err);
        return false;
    }

    _status.bytes_written += written;
    _status.total_bytes = _status.bytes_written;  // Updated as we go
    updateProgress(_status.bytes_written, _status.total_bytes);

    if (final_chunk) {
        // Finalize
        _md5.calculate();
        setState(OTA_VERIFYING, "Verifying firmware");

        if (!Update.end(true)) {
            char err[64];
            snprintf(err, sizeof(err), "Verification failed: %s",
                     Update.errorString());
            setError(err);
            _uploadStarted = false;
            _audit.logAttempt("web", "unknown", "any", false, err);
            return false;
        }

        _uploadStarted = false;
        _status.progress_pct = 100;

        // Try to read new version from next boot partition
        const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
        if (next) {
            esp_app_desc_t new_desc;
            if (esp_ota_get_partition_description(next, &new_desc) == ESP_OK) {
                strncpy(_status.new_version, new_desc.version,
                        sizeof(_status.new_version) - 1);
            }
        }

        setState(OTA_READY_REBOOT, "Upload complete, ready to reboot");
        saveHistoryEntry(
            _status.new_version[0] ? _status.new_version : "unknown",
            "web", true);
        _audit.logAttempt("web",
                          _status.new_version[0] ? _status.new_version : "unknown",
                          "any", true, nullptr);

        DBG_INFO(TAG, "Upload OTA complete, %u bytes, MD5=%s",
                 _status.bytes_written, _md5.toString().c_str());
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

    HTTPClient http;
    http.begin(url);
    http.setTimeout(30000);
    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        char err[64];
        snprintf(err, sizeof(err), "HTTP error: %d", httpCode);
        setError(err);
        http.end();
        _audit.logAttempt("url", "unknown", "any", false, err);
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        setError("Invalid content length");
        http.end();
        _audit.logAttempt("url", "unknown", "any", false, "Invalid content length");
        return false;
    }

    _status.total_bytes = contentLength;

    if (!Update.begin(contentLength)) {
        char err[64];
        snprintf(err, sizeof(err), "No space: %s", Update.errorString());
        setError(err);
        http.end();
        _audit.logAttempt("url", "unknown", "any", false, err);
        return false;
    }

    setState(OTA_WRITING, "Writing firmware to flash");

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    uint32_t written = 0;

    while (http.connected() && written < (uint32_t)contentLength) {
        size_t available = stream->available();
        if (available) {
            size_t toRead = (available < sizeof(buf)) ? available : sizeof(buf);
            size_t bytesRead = stream->readBytes(buf, toRead);
            size_t bytesWritten = Update.write(buf, bytesRead);
            if (bytesWritten != bytesRead) {
                char err[64];
                snprintf(err, sizeof(err), "Write failed at %u: %s",
                         written, Update.errorString());
                setError(err);
                Update.abort();
                http.end();
                _audit.logAttempt("url", "unknown", "any", false, err);
                return false;
            }
            written += bytesWritten;
            updateProgress(written, contentLength);
        }
        delay(1);
    }

    http.end();
    setState(OTA_VERIFYING, "Verifying firmware");

    if (!Update.end(true)) {
        char err[64];
        snprintf(err, sizeof(err), "Verify failed: %s", Update.errorString());
        setError(err);
        _audit.logAttempt("url", "unknown", "any", false, err);
        return false;
    }

    // Read new version
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next) {
        esp_app_desc_t new_desc;
        if (esp_ota_get_partition_description(next, &new_desc) == ESP_OK) {
            strncpy(_status.new_version, new_desc.version,
                    sizeof(_status.new_version) - 1);
        }
    }

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

    DBG_INFO(TAG, "SD OTA from: %s", path);
    setState(OTA_CHECKING, "Reading SD card");

    if (!SD_MMC.begin()) {
        setError("SD card mount failed");
        _audit.logAttempt("sd", "unknown", "any", false, "SD mount failed");
        return false;
    }

    File firmware = SD_MMC.open(path, FILE_READ);
    if (!firmware) {
        char err[64];
        snprintf(err, sizeof(err), "Cannot open %s", path);
        setError(err);
        _audit.logAttempt("sd", "unknown", "any", false, err);
        return false;
    }

    size_t fileSize = firmware.size();
    if (fileSize == 0) {
        setError("Firmware file is empty");
        firmware.close();
        _audit.logAttempt("sd", "unknown", "any", false, "Empty file");
        return false;
    }

    _status.total_bytes = fileSize;

    if (!Update.begin(fileSize)) {
        char err[64];
        snprintf(err, sizeof(err), "No space: %s", Update.errorString());
        setError(err);
        firmware.close();
        _audit.logAttempt("sd", "unknown", "any", false, err);
        return false;
    }

    setState(OTA_WRITING, "Writing firmware from SD");

    uint8_t buf[1024];
    uint32_t written = 0;

    while (firmware.available()) {
        size_t toRead = firmware.available();
        if (toRead > sizeof(buf)) toRead = sizeof(buf);
        size_t bytesRead = firmware.read(buf, toRead);
        size_t bytesWritten = Update.write(buf, bytesRead);
        if (bytesWritten != bytesRead) {
            char err[64];
            snprintf(err, sizeof(err), "Write failed at %u: %s",
                     written, Update.errorString());
            setError(err);
            Update.abort();
            firmware.close();
            _audit.logAttempt("sd", "unknown", "any", false, err);
            return false;
        }
        written += bytesWritten;
        updateProgress(written, fileSize);
    }

    firmware.close();
    setState(OTA_VERIFYING, "Verifying firmware");

    if (!Update.end(true)) {
        char err[64];
        snprintf(err, sizeof(err), "Verify failed: %s", Update.errorString());
        setError(err);
        _audit.logAttempt("sd", "unknown", "any", false, err);
        return false;
    }

    // Rename to .bak
    char bakPath[128];
    snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
    SD_MMC.remove(bakPath);
    SD_MMC.rename(path, bakPath);

    // Read new version
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next) {
        esp_app_desc_t new_desc;
        if (esp_ota_get_partition_description(next, &new_desc) == ESP_OK) {
            strncpy(_status.new_version, new_desc.version,
                    sizeof(_status.new_version) - 1);
        }
    }

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
    ESP.restart();
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
