// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_heartbeat.h"
#include "debug_log.h"

#include <cstring>
#include <cstdio>

static constexpr const char* TAG = "heartbeat";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace hal_heartbeat {

static bool _active = false;
static uint32_t _interval_ms = 60000;

bool init(const HeartbeatConfig& config) {
    (void)config;
    DBG_INFO(TAG, "Heartbeat init (simulator stub)");
    return false;
}

bool tick() { return false; }
bool send_now() { return false; }
bool is_active() { return false; }
uint32_t get_interval_ms() { return _interval_ms; }

}  // namespace hal_heartbeat

// ============================================================================
// ESP32 — real HTTP heartbeat
// ============================================================================
#else

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include "hal_provision.h"

namespace hal_heartbeat {

// Internal state
static bool _initialized = false;
static bool _active = false;
static uint32_t _interval_ms = 60000;
static uint32_t _last_send_ms = 0;
static char _server_url[256] = {};
static char _device_id[64] = {};

// Cached firmware info (computed once at init)
static char _fw_version[32] = {};
static char _fw_hash[65] = {};
static const char* _board_name = nullptr;

// Provisioning instance (shared, lightweight)
static ProvisionHAL _provision;
static bool _provision_inited = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void computeFirmwareHash() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        _fw_hash[0] = '\0';
        return;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    uint8_t buf[4096];
    size_t offset = 0;
    size_t fwSize = running->size;
    while (offset < fwSize) {
        size_t toRead = (fwSize - offset < sizeof(buf)) ? fwSize - offset : sizeof(buf);
        if (esp_partition_read(running, offset, buf, toRead) != ESP_OK) {
            mbedtls_sha256_free(&ctx);
            _fw_hash[0] = '\0';
            return;
        }
        mbedtls_sha256_update(&ctx, buf, toRead);
        offset += toRead;
    }

    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    for (int i = 0; i < 32; i++) {
        snprintf(&_fw_hash[i * 2], 3, "%02x", hash[i]);
    }
}

static void cacheFirmwareInfo() {
    // Version from app description
    const esp_app_desc_t* desc = esp_ota_get_app_description();
    if (desc) {
        strncpy(_fw_version, desc->version, sizeof(_fw_version) - 1);
        _fw_version[sizeof(_fw_version) - 1] = '\0';
    } else {
        strncpy(_fw_version, "unknown", sizeof(_fw_version) - 1);
    }

    // Board name
#ifdef DISPLAY_DRIVER
    _board_name = DISPLAY_DRIVER;
#else
    _board_name = "unknown";
#endif

    // Firmware hash (expensive, do once)
    computeFirmwareHash();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init(const HeartbeatConfig& config) {
    if (_initialized) return _active;

    _initialized = true;
    _interval_ms = config.interval_ms;

    // Resolve server_url and device_id
    if (config.server_url && config.server_url[0] != '\0') {
        strncpy(_server_url, config.server_url, sizeof(_server_url) - 1);
    }
    if (config.device_id && config.device_id[0] != '\0') {
        strncpy(_device_id, config.device_id, sizeof(_device_id) - 1);
    }

    // If not provided, try provisioning
    if (_server_url[0] == '\0' || _device_id[0] == '\0') {
        if (!_provision_inited) {
            _provision_inited = _provision.init();
        }
        if (_provision_inited && _provision.isProvisioned()) {
            const DeviceIdentity& id = _provision.getIdentity();
            if (_server_url[0] == '\0' && id.server_url[0] != '\0') {
                strncpy(_server_url, id.server_url, sizeof(_server_url) - 1);
            }
            if (_device_id[0] == '\0' && id.device_id[0] != '\0') {
                strncpy(_device_id, id.device_id, sizeof(_device_id) - 1);
            }
        }
    }

    // Check if we have enough config to send heartbeats
    if (_server_url[0] == '\0' || _device_id[0] == '\0') {
        DBG_INFO(TAG, "Not configured, skipping");
        _active = false;
        return false;
    }

    // Cache firmware info (version, hash, board)
    cacheFirmwareInfo();

    _active = true;
    _last_send_ms = 0;  // Send first heartbeat on next tick()
    DBG_INFO(TAG, "Initialized: server=%s device=%s interval=%lums",
             _server_url, _device_id, (unsigned long)_interval_ms);
    return true;
}

bool tick() {
    if (!_active) return false;
    if (!WiFi.isConnected()) return false;

    uint32_t now = millis();
    if (_last_send_ms != 0 && (now - _last_send_ms) < _interval_ms) {
        return false;
    }
    _last_send_ms = now;

    return send_now();
}

bool send_now() {
    if (!_active) return false;
    if (!WiFi.isConnected()) return false;

    char url[320];
    snprintf(url, sizeof(url), "%s/api/devices/%s/status", _server_url, _device_id);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    // Build JSON payload — matches the format used by OtaApp::sendHeartbeat()
    const char* partition = "unknown";
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) partition = running->label;

    char body[768];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"board\":\"%s\",\"partition\":\"%s\","
             "\"ip\":\"%s\",\"mac\":\"%s\",\"uptime_s\":%lu,\"free_heap\":%u,"
             "\"rssi\":%d,\"fw_hash\":\"%s\","
             "\"reported_config\":{\"heartbeat_interval_s\":%lu}}",
             _fw_version, _board_name, partition,
             WiFi.localIP().toString().c_str(),
             WiFi.macAddress().c_str(),
             (unsigned long)(millis() / 1000),
             (unsigned)ESP.getFreeHeap(),
             WiFi.RSSI(),
             _fw_hash,
             (unsigned long)(_interval_ms / 1000));

    int code = http.POST(body);
    if (code == 200) {
        DBG_DEBUG(TAG, "Sent to %s - %d", _server_url, code);

        // Parse response for server-directed interval or OTA
        String response = http.getString();

        // Check for heartbeat_interval_s adjustment
        int idx = response.indexOf("\"heartbeat_interval_s\":");
        if (idx >= 0) {
            int valStart = idx + 23;  // length of "heartbeat_interval_s":
            int newInterval = response.substring(valStart).toInt();
            if (newInterval > 0 && newInterval <= 3600) {
                uint32_t newMs = (uint32_t)newInterval * 1000;
                if (newMs != _interval_ms) {
                    DBG_INFO(TAG, "Server adjusted interval: %lus -> %ds",
                             (unsigned long)(_interval_ms / 1000), newInterval);
                    _interval_ms = newMs;
                }
            }
        }

        // Check for desired_config — apply server-pushed settings
        if (response.indexOf("\"desired_config\"") >= 0) {
            // Parse heartbeat_interval_s from desired_config
            int cfgIdx = response.indexOf("\"desired_config\"");
            if (cfgIdx >= 0) {
                int intervalIdx = response.indexOf("\"heartbeat_interval_s\":", cfgIdx);
                if (intervalIdx >= 0) {
                    int valStart = intervalIdx + 23;
                    int newInterval = response.substring(valStart).toInt();
                    if (newInterval > 0 && newInterval <= 3600) {
                        uint32_t newMs = (uint32_t)newInterval * 1000;
                        if (newMs != _interval_ms) {
                            DBG_INFO(TAG, "Config: heartbeat_interval=%ds", newInterval);
                            _interval_ms = newMs;
                        }
                    }
                }
                DBG_DEBUG(TAG, "Received desired_config from server");
            }
        }

        // Check for OTA directive — store URL in NVS for hal_ota to pick up
        if (response.indexOf("\"ota\"") >= 0 && response.indexOf("\"url\"") >= 0) {
            int urlStart = response.indexOf("\"url\":\"") + 7;
            int urlEnd = response.indexOf("\"", urlStart);
            if (urlStart > 6 && urlEnd > urlStart) {
                String otaUrl = String(_server_url) + response.substring(urlStart, urlEnd);
                DBG_INFO(TAG, "Server scheduled OTA: %s", otaUrl.c_str());
                // Note: OTA execution is left to apps that include hal_ota.
                // The heartbeat library only logs the directive.
            }
        }
    } else {
        DBG_DEBUG(TAG, "Sent to %s - HTTP %d", _server_url, code);
    }

    http.end();
    return (code == 200);
}

bool is_active() {
    return _active;
}

uint32_t get_interval_ms() {
    return _interval_ms;
}

}  // namespace hal_heartbeat

#endif  // SIMULATOR
