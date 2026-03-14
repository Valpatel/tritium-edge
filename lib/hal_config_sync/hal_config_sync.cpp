// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_config_sync.h"
#include "debug_log.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

static constexpr const char* TAG = "config_sync";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace hal_config_sync {

static DeviceConfig _config;

bool init(const SyncConfig&) { return false; }
void on_mqtt_config(const char*, size_t) {}
const DeviceConfig& get_config() { return _config; }
uint32_t get_config_version() { return 0; }
bool is_synced() { return false; }
int get_config_json(char* buf, size_t buf_size) {
    return snprintf(buf, buf_size, "{\"version\":0,\"synced\":false}");
}

}  // namespace hal_config_sync

// ============================================================================
// ESP32 — real config sync via HTTP + MQTT
// ============================================================================
#else

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "hal_provision.h"

namespace hal_config_sync {

// ---------------------------------------------------------------------------
// Minimal JSON helpers (same pattern as hal_provision.cpp)
// ---------------------------------------------------------------------------

static bool jsonGetString(const char* json, const char* key, char* out, size_t outSize) {
    if (!json || !key || !out || outSize == 0) return false;
    out[0] = '\0';
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outSize - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool jsonGetInt(const char* json, const char* key, int* out) {
    if (!json || !key || !out) return false;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        *out = atoi(p);
    } else {
        *out = atoi(p);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static bool _initialized = false;
static bool _synced = false;
static DeviceConfig _config;
static char _server_url[256] = {};
static char _device_id[64] = {};

// Provisioning instance
static ProvisionHAL _provision;
static bool _provision_inited = false;

// NVS namespace for config sync
static constexpr const char* NVS_NAMESPACE = "cfg_sync";

// ---------------------------------------------------------------------------
// NVS persistence
// ---------------------------------------------------------------------------

static void save_config_to_nvs() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) {
        DBG_ERROR(TAG, "Failed to open NVS for write");
        return;
    }
    prefs.putUInt("version", _config.config_version);
    prefs.putUInt("hb_int_s", _config.heartbeat_interval_s);
    prefs.putUInt("sight_int_s", _config.sighting_interval_s);
    prefs.putUInt("scan_int_s", _config.scan_interval_s);
    prefs.putUChar("disp_bright", _config.display_brightness);
    prefs.putUInt("disp_tout_s", _config.display_timeout_s);
    prefs.putInt("rf_thresh", _config.rf_monitor_threshold_dbm);
    prefs.putUInt("rf_win_s", _config.rf_monitor_window_s);
    prefs.end();
    DBG_INFO(TAG, "Config v%u saved to NVS", _config.config_version);
}

static void load_config_from_nvs() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
        DBG_DEBUG(TAG, "No saved config in NVS");
        return;
    }
    _config.config_version = prefs.getUInt("version", 0);
    _config.heartbeat_interval_s = prefs.getUInt("hb_int_s", 30);
    _config.sighting_interval_s = prefs.getUInt("sight_int_s", 15);
    _config.scan_interval_s = prefs.getUInt("scan_int_s", 10);
    _config.display_brightness = prefs.getUChar("disp_bright", 255);
    _config.display_timeout_s = prefs.getUInt("disp_tout_s", 300);
    _config.rf_monitor_threshold_dbm = prefs.getInt("rf_thresh", -60);
    _config.rf_monitor_window_s = prefs.getUInt("rf_win_s", 10);
    prefs.end();

    if (_config.config_version > 0) {
        _synced = true;
        DBG_INFO(TAG, "Loaded config v%u from NVS", _config.config_version);
    }
}

// ---------------------------------------------------------------------------
// Parse config JSON and apply if version is newer
// ---------------------------------------------------------------------------

static bool apply_config_json(const char* json) {
    if (!json) return false;

    int version = 0;
    if (!jsonGetInt(json, "config_version", &version)) {
        DBG_WARN(TAG, "Config JSON missing config_version");
        return false;
    }

    if ((uint32_t)version <= _config.config_version) {
        DBG_DEBUG(TAG, "Config v%d not newer than local v%u, skipping",
                  version, _config.config_version);
        return false;
    }

    DBG_INFO(TAG, "Applying config v%d (was v%u)", version, _config.config_version);

    _config.config_version = (uint32_t)version;

    int val;
    if (jsonGetInt(json, "heartbeat_interval_s", &val) && val > 0 && val <= 3600) {
        _config.heartbeat_interval_s = (uint32_t)val;
    }
    if (jsonGetInt(json, "sighting_interval_s", &val) && val > 0 && val <= 3600) {
        _config.sighting_interval_s = (uint32_t)val;
    }
    if (jsonGetInt(json, "scan_interval_s", &val) && val > 0 && val <= 3600) {
        _config.scan_interval_s = (uint32_t)val;
    }
    if (jsonGetInt(json, "display_brightness", &val) && val >= 0 && val <= 255) {
        _config.display_brightness = (uint8_t)val;
    }
    if (jsonGetInt(json, "display_timeout_s", &val) && val >= 0 && val <= 86400) {
        _config.display_timeout_s = (uint32_t)val;
    }
    if (jsonGetInt(json, "rf_monitor_threshold_dbm", &val) && val >= -100 && val <= 0) {
        _config.rf_monitor_threshold_dbm = val;
    }
    if (jsonGetInt(json, "rf_monitor_window_s", &val) && val > 0 && val <= 3600) {
        _config.rf_monitor_window_s = (uint32_t)val;
    }

    // Persist to NVS
    save_config_to_nvs();
    _synced = true;

    return true;
}

// ---------------------------------------------------------------------------
// HTTP fetch config from fleet server
// ---------------------------------------------------------------------------

static bool fetch_config_from_server() {
    if (_server_url[0] == '\0' || _device_id[0] == '\0') {
        return false;
    }
    if (!WiFi.isConnected()) {
        return false;
    }

    char url[384];
    snprintf(url, sizeof(url), "%s/api/devices/%s/config", _server_url, _device_id);

    DBG_INFO(TAG, "Fetching config from %s", url);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(5000);

    int code = http.GET();
    if (code != 200) {
        DBG_WARN(TAG, "Config fetch failed: HTTP %d", code);
        http.end();
        return false;
    }

    String response = http.getString();
    http.end();

    if (response.length() == 0) {
        DBG_WARN(TAG, "Empty config response");
        return false;
    }

    DBG_DEBUG(TAG, "Config response: %s", response.c_str());

    bool applied = apply_config_json(response.c_str());
    if (applied) {
        DBG_INFO(TAG, "Remote config applied: v%u", _config.config_version);
    } else {
        DBG_DEBUG(TAG, "Remote config not newer than local");
    }

    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init(const SyncConfig& config) {
    if (_initialized) return _synced;
    _initialized = true;

    // Resolve server_url and device_id
    if (config.server_url && config.server_url[0] != '\0') {
        strncpy(_server_url, config.server_url, sizeof(_server_url) - 1);
    }
    if (config.device_id && config.device_id[0] != '\0') {
        strncpy(_device_id, config.device_id, sizeof(_device_id) - 1);
    }

    // Try provisioning if not provided
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

    // Load last known config from NVS (used as fallback if server unreachable)
    load_config_from_nvs();

    // Attempt HTTP fetch from fleet server
    if (_server_url[0] != '\0' && _device_id[0] != '\0') {
        fetch_config_from_server();
        DBG_INFO(TAG, "Initialized: server=%s device=%s config_v=%u",
                 _server_url, _device_id, _config.config_version);
    } else {
        DBG_INFO(TAG, "No server/device_id configured, using NVS/defaults");
    }

    return true;
}

void on_mqtt_config(const char* payload, size_t len) {
    if (!payload || len == 0) return;

    // Ensure null-terminated copy (payload may not be)
    static char buf[1024];
    size_t copy_len = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
    memcpy(buf, payload, copy_len);
    buf[copy_len] = '\0';

    DBG_INFO(TAG, "MQTT config push received (%zu bytes)", len);

    if (apply_config_json(buf)) {
        DBG_INFO(TAG, "MQTT config applied: v%u", _config.config_version);
    }
}

const DeviceConfig& get_config() {
    return _config;
}

uint32_t get_config_version() {
    return _config.config_version;
}

bool is_synced() {
    return _synced;
}

int get_config_json(char* buf, size_t buf_size) {
    return snprintf(buf, buf_size,
        "{\"version\":%u,\"synced\":%s,"
        "\"heartbeat_interval_s\":%u,"
        "\"sighting_interval_s\":%u,"
        "\"scan_interval_s\":%u,"
        "\"display_brightness\":%u,"
        "\"display_timeout_s\":%u,"
        "\"rf_monitor_threshold_dbm\":%d,"
        "\"rf_monitor_window_s\":%u}",
        _config.config_version,
        _synced ? "true" : "false",
        _config.heartbeat_interval_s,
        _config.sighting_interval_s,
        _config.scan_interval_s,
        (unsigned)_config.display_brightness,
        _config.display_timeout_s,
        _config.rf_monitor_threshold_dbm,
        _config.rf_monitor_window_s);
}

}  // namespace hal_config_sync

#endif  // SIMULATOR
