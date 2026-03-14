// Tritium-OS MQTT OTA — remote firmware push from SC dashboard
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ota_mqtt.h"
#include "ota_manager.h"
#include "debug_log.h"
#include <cstring>
#include <cstdio>

static constexpr const char* TAG = "ota_mqtt";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace ota_mqtt {

bool init(const char*) { return false; }
void tick() {}
bool is_active() { return false; }
bool checkAutoRollback(uint8_t) { return false; }
void markBootSuccessful() {}

}  // namespace ota_mqtt

// ============================================================================
// ESP32 — real MQTT OTA bridge
// ============================================================================
#else

#include <Arduino.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_ota_ops.h>

#if __has_include("mqtt_sc_bridge.h")
#include "mqtt_sc_bridge.h"
#include "hal_mqtt.h"
#define HAS_MQTT_BRIDGE 1
#else
#define HAS_MQTT_BRIDGE 0
#endif

#if __has_include("hal_provision.h")
#include "hal_provision.h"
#define HAS_PROVISION 1
#else
#define HAS_PROVISION 0
#endif

namespace ota_mqtt {

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static bool _initialized = false;
static bool _ota_in_progress = false;
static char _device_id[64] = {};
static char _pending_url[256] = {};
static bool _has_pending_url = false;
static bool _pending_rollback = false;
static bool _pending_reboot = false;

// Progress publishing via a separate MQTT client instance
#if HAS_MQTT_BRIDGE
static MqttHAL _progress_mqtt;
static MqttHAL* _mqtt = &_progress_mqtt;
static bool _mqtt_init_attempted = false;
#else
static MqttHAL* _mqtt = nullptr;
#endif
static char _topic_progress[128] = {};

// NVS keys for boot counter
static constexpr const char* NVS_NAMESPACE = "ota_boot";
static constexpr const char* NVS_KEY_COUNT = "boot_cnt";
static constexpr const char* NVS_KEY_STABLE = "stable";

// Progress reporting interval
static uint32_t _last_progress_ms = 0;
static constexpr uint32_t PROGRESS_INTERVAL_MS = 1000;

// Boot stability timer
static uint32_t _boot_start_ms = 0;
static bool _boot_marked_stable = false;
static constexpr uint32_t BOOT_STABLE_DELAY_MS = 30000;  // 30s before marking stable

// Provisioning (for MQTT broker info)
#if HAS_PROVISION
static ProvisionHAL _prov;
static bool _prov_init = false;
#endif

// ---------------------------------------------------------------------------
// Progress callback — publishes OTA status to MQTT
// ---------------------------------------------------------------------------
static void publishProgress(const char* status, uint8_t pct,
                            uint32_t bytes_written, uint32_t total_bytes,
                            const char* error = nullptr) {
    if (!_mqtt || !_mqtt->isConnected()) return;

    char json[320];
    int pos = snprintf(json, sizeof(json),
        "{\"device_id\":\"%s\",\"status\":\"%s\","
        "\"progress\":%u,\"bytes_written\":%u,\"total_bytes\":%u",
        _device_id, status, pct, bytes_written, total_bytes);

    if (error && error[0]) {
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",\"error\":\"%s\"", error);
    }

    // Add version info
    const auto& ota_status = ota_manager::getStatus();
    if (ota_status.current_version[0]) {
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",\"current_version\":\"%s\"", ota_status.current_version);
    }
    if (ota_status.new_version[0]) {
        pos += snprintf(json + pos, sizeof(json) - pos,
            ",\"new_version\":\"%s\"", ota_status.new_version);
    }

    if (pos < (int)sizeof(json) - 1) {
        json[pos++] = '}';
        json[pos] = '\0';
    }

    _mqtt->publish(_topic_progress, json, false, 1);
}

// ---------------------------------------------------------------------------
// OTA progress callback from ota_manager
// ---------------------------------------------------------------------------
static void otaProgressCallback(const ota_manager::OtaStatus& status, void*) {
    const char* state_str = "unknown";
    switch (status.state) {
        case ota_manager::OTA_IDLE:         state_str = "idle"; break;
        case ota_manager::OTA_CHECKING:     state_str = "checking"; break;
        case ota_manager::OTA_DOWNLOADING:  state_str = "downloading"; break;
        case ota_manager::OTA_WRITING:      state_str = "writing"; break;
        case ota_manager::OTA_VERIFYING:    state_str = "verifying"; break;
        case ota_manager::OTA_READY_REBOOT: state_str = "ready_reboot"; break;
        case ota_manager::OTA_FAILED:       state_str = "failed"; break;
    }

    // Rate-limit progress updates to avoid flooding MQTT
    uint32_t now = millis();
    bool force = (status.state == ota_manager::OTA_READY_REBOOT ||
                  status.state == ota_manager::OTA_FAILED ||
                  status.state == ota_manager::OTA_DOWNLOADING ||
                  status.state == ota_manager::OTA_VERIFYING);

    if (!force && (now - _last_progress_ms) < PROGRESS_INTERVAL_MS) return;
    _last_progress_ms = now;

    publishProgress(state_str, status.progress_pct,
                    status.bytes_written, status.total_bytes,
                    status.state == ota_manager::OTA_FAILED ? status.error_msg : nullptr);
}

// ---------------------------------------------------------------------------
// Command handler — called by mqtt_sc_bridge when cmd/ota is received
// ---------------------------------------------------------------------------
static void handleOtaCommand(const char* payload, size_t len) {
    DBG_INFO(TAG, "MQTT OTA command received: %.*s", (int)len, payload);

    // Check for action-only commands (rollback, reboot) first
    const char* action = strstr(payload, "\"action\"");
    if (action) {
        if (strstr(action, "\"rollback\"")) {
            DBG_INFO(TAG, "MQTT OTA rollback requested");
            _pending_rollback = true;
            return;
        }
        if (strstr(action, "\"reboot\"")) {
            DBG_INFO(TAG, "MQTT reboot requested");
            _pending_reboot = true;
            return;
        }
    }

    // Parse JSON payload for "url" field
    const char* url_key = strstr(payload, "\"url\"");
    if (!url_key) {
        url_key = strstr(payload, "\"firmware_url\"");
    }
    if (!url_key) {
        DBG_ERROR(TAG, "No URL in OTA command payload");
        publishProgress("error", 0, 0, 0, "No firmware URL in command");
        return;
    }

    // Find the colon after the key
    const char* colon = strchr(url_key, ':');
    if (!colon) return;

    // Find opening quote of value
    const char* quote_start = strchr(colon + 1, '"');
    if (!quote_start) return;
    quote_start++;  // Skip opening quote

    // Find closing quote
    const char* quote_end = strchr(quote_start, '"');
    if (!quote_end) return;

    size_t url_len = (size_t)(quote_end - quote_start);
    if (url_len == 0 || url_len >= sizeof(_pending_url)) {
        DBG_ERROR(TAG, "Invalid URL length: %u", (unsigned)url_len);
        publishProgress("error", 0, 0, 0, "Invalid firmware URL");
        return;
    }

    if (_ota_in_progress) {
        DBG_ERROR(TAG, "OTA already in progress, ignoring");
        publishProgress("error", 0, 0, 0, "OTA already in progress");
        return;
    }

    // Queue the URL for processing in tick() (don't block the MQTT callback)
    memcpy(_pending_url, quote_start, url_len);
    _pending_url[url_len] = '\0';
    _has_pending_url = true;

    DBG_INFO(TAG, "OTA URL queued: %s", _pending_url);
    publishProgress("queued", 0, 0, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init(const char* device_id) {
    if (_initialized) return true;
    if (!device_id || device_id[0] == '\0') return false;

    strncpy(_device_id, device_id, sizeof(_device_id) - 1);

    // Build progress topic
    snprintf(_topic_progress, sizeof(_topic_progress),
             "tritium/%s/ota/progress", _device_id);

    // Register command handler with MQTT SC bridge
#if HAS_MQTT_BRIDGE
    mqtt_sc_bridge::on_command([](const char* cmd, const char* payload, size_t len) {
        if (strcmp(cmd, "ota") == 0) {
            handleOtaCommand(payload, len);
        }
    });
#endif

    _boot_start_ms = millis();
    _initialized = true;

    DBG_INFO(TAG, "MQTT OTA initialized, device=%s, topic=%s",
             _device_id, _topic_progress);
    return true;
}

void tick() {
    if (!_initialized) return;

#if HAS_MQTT_BRIDGE
    // Lazy-init progress MQTT client using provisioning data
    if (!_mqtt_init_attempted && mqtt_sc_bridge::is_connected()) {
        _mqtt_init_attempted = true;
#if HAS_PROVISION
        if (!_prov_init) {
            _prov_init = _prov.init();
        }
        if (_prov_init && _prov.isProvisioned()) {
            const DeviceIdentity& id = _prov.getIdentity();
            char client_id[48];
            snprintf(client_id, sizeof(client_id), "%s-ota", _device_id);
            if (_mqtt->init(id.mqtt_broker, id.mqtt_port, client_id)) {
                DBG_INFO(TAG, "Progress MQTT client initialized");
            }
        }
#endif
    }

    // Process progress MQTT
    if (_mqtt) {
        _mqtt->process();
    }
#endif

    // Handle pending rollback
    if (_pending_rollback) {
        _pending_rollback = false;
        publishProgress("rolling_back", 0, 0, 0, nullptr);
        ota_manager::init();
        if (ota_manager::rollback()) {
            publishProgress("rollback_ready", 100, 0, 0, nullptr);
            delay(500);
            ota_manager::reboot();
        } else {
            publishProgress("error", 0, 0, 0, "Rollback failed");
        }
        return;
    }

    // Handle pending reboot
    if (_pending_reboot) {
        _pending_reboot = false;
        publishProgress("rebooting", 0, 0, 0, nullptr);
        delay(500);
        esp_restart();
        return;
    }

    // Process pending OTA URL
    if (_has_pending_url && !_ota_in_progress) {
        _has_pending_url = false;
        _ota_in_progress = true;

        DBG_INFO(TAG, "Starting MQTT OTA from: %s", _pending_url);
        publishProgress("starting", 0, 0, 0, nullptr);

        // Initialize OTA manager and register progress callback
        ota_manager::init();
        ota_manager::onProgress(otaProgressCallback, nullptr);

        // Start the URL download (this blocks until complete)
        bool ok = ota_manager::updateFromUrl(_pending_url);
        _ota_in_progress = false;

        if (ok) {
            DBG_INFO(TAG, "MQTT OTA complete, rebooting in 2s");
            publishProgress("ready_reboot", 100, 0, 0, nullptr);
            delay(2000);
            ota_manager::reboot();
        } else {
            const auto& status = ota_manager::getStatus();
            DBG_ERROR(TAG, "MQTT OTA failed: %s", status.error_msg);
            publishProgress("failed", 0, 0, 0, status.error_msg);
        }
    }

    // Auto-mark boot as stable after BOOT_STABLE_DELAY_MS
    if (!_boot_marked_stable && (millis() - _boot_start_ms) >= BOOT_STABLE_DELAY_MS) {
        markBootSuccessful();
    }
}

bool is_active() {
    return _ota_in_progress;
}

bool checkAutoRollback(uint8_t max_failures) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        DBG_ERROR(TAG, "NVS open failed for boot counter: 0x%x", err);
        return false;
    }

    // Read current boot count
    uint8_t boot_count = 0;
    nvs_get_u8(nvs, NVS_KEY_COUNT, &boot_count);

    // Check if previous boot was marked stable
    uint8_t was_stable = 0;
    nvs_get_u8(nvs, NVS_KEY_STABLE, &was_stable);

    if (was_stable) {
        // Previous boot was successful, reset counter
        boot_count = 0;
    }

    // Increment boot count
    boot_count++;
    nvs_set_u8(nvs, NVS_KEY_COUNT, boot_count);
    nvs_set_u8(nvs, NVS_KEY_STABLE, 0);  // Not yet stable this boot
    nvs_commit(nvs);
    nvs_close(nvs);

    DBG_INFO(TAG, "Boot count: %u/%u", boot_count, max_failures);

    if (boot_count >= max_failures) {
        DBG_ERROR(TAG, "Boot failed %u times, triggering auto-rollback!", boot_count);

        // Reset counter before rollback to avoid rollback loop
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
        if (err == ESP_OK) {
            nvs_set_u8(nvs, NVS_KEY_COUNT, 0);
            nvs_set_u8(nvs, NVS_KEY_STABLE, 1);
            nvs_commit(nvs);
            nvs_close(nvs);
        }

        // Attempt rollback
        const esp_partition_t* prev = esp_ota_get_next_update_partition(nullptr);
        if (prev) {
            err = esp_ota_set_boot_partition(prev);
            if (err == ESP_OK) {
                DBG_ERROR(TAG, "Auto-rollback to %s, rebooting...", prev->label);
                delay(200);
                esp_restart();
                return true;  // Never reached
            }
        }

        DBG_ERROR(TAG, "Auto-rollback failed, no valid partition");
    }

    return false;
}

void markBootSuccessful() {
    if (_boot_marked_stable) return;
    _boot_marked_stable = true;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return;

    nvs_set_u8(nvs, NVS_KEY_COUNT, 0);
    nvs_set_u8(nvs, NVS_KEY_STABLE, 1);
    nvs_commit(nvs);
    nvs_close(nvs);

    // Also tell esp_ota the app is valid
    esp_ota_mark_app_valid_cancel_rollback();

    DBG_INFO(TAG, "Boot marked successful, rollback counter reset");
}

}  // namespace ota_mqtt

#endif  // SIMULATOR
