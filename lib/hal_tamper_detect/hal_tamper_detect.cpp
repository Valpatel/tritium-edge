// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_tamper_detect.h"
#include "debug_log.h"

#include <cstdio>
#include <cstring>

static constexpr const char* TAG = "tamper";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace hal_tamper_detect {

static TamperAlert _alert = {};

bool init(const TamperConfig&) { return false; }
bool tick() { return false; }
TamperType get_status() { return TamperType::NONE; }
bool is_alert_active() { return false; }
const TamperAlert& get_alert() { return _alert; }
void clear_alert() {}
int to_json(char* buf, size_t size) {
    return snprintf(buf, size, "{\"status\":\"ok\"}");
}

}  // namespace hal_tamper_detect

// ============================================================================
// ESP32 — real tamper detection
// ============================================================================
#else

#include <Arduino.h>
#include <esp_heap_caps.h>

// Optional WiFi scanner
#if __has_include("hal_wifi_scanner.h")
#include "hal_wifi_scanner.h"
#define HAS_WIFI_SCANNER 1
#else
#define HAS_WIFI_SCANNER 0
#endif

// Optional BLE scanner
#if __has_include("hal_ble_scanner.h")
#include "hal_ble_scanner.h"
#define HAS_BLE_SCANNER 1
#else
#define HAS_BLE_SCANNER 0
#endif

// MQTT SC bridge — used to force-publish heartbeat on tamper alert
#if __has_include("mqtt_sc_bridge.h")
#include "mqtt_sc_bridge.h"
#define HAS_MQTT_BRIDGE 1
#else
#define HAS_MQTT_BRIDGE 0
#endif

namespace hal_tamper_detect {

static bool _initialized = false;
static TamperConfig _config;
static TamperAlert _alert = {};
static bool _alert_active = false;
static uint32_t _last_check_ms = 0;

// Track when we last saw non-zero sighting counts
static uint32_t _wifi_last_nonzero_ms = 0;
static uint32_t _ble_last_nonzero_ms = 0;
static bool _wifi_was_zero = false;
static bool _ble_was_zero = false;

// Prevent duplicate MQTT alerts within a session
static bool _wifi_alert_sent = false;
static bool _ble_alert_sent = false;

static const char* tamper_type_str(TamperType t) {
    switch (t) {
        case TamperType::WIFI_SILENCE: return "wifi_silence";
        case TamperType::BLE_SILENCE:  return "ble_silence";
        case TamperType::BOTH_SILENCE: return "both_silence";
        default:                       return "none";
    }
}

static void publish_tamper_alert(TamperType type, uint32_t silence_s) {
    // Log the tamper alert — MQTT publishing happens via heartbeat integration
    // (hal_heartbeat.cpp includes tamper JSON in heartbeat payload)
    DBG_WARN(TAG, "TAMPER ALERT: %s — zero sightings for %lu seconds. "
             "Possible RF jamming or physical obstruction.",
             tamper_type_str(type), (unsigned long)silence_s);

    // Force a heartbeat to push the tamper status immediately
#if HAS_MQTT_BRIDGE
    if (mqtt_sc_bridge::is_connected()) {
        mqtt_sc_bridge::publish_heartbeat();
        DBG_INFO(TAG, "Forced heartbeat publish with tamper alert");
    }
#endif
}

bool init(const TamperConfig& config) {
    if (_initialized) return true;
    _config = config;
    _initialized = true;
    _alert_active = false;
    memset(&_alert, 0, sizeof(_alert));

    uint32_t now = millis();
    _wifi_last_nonzero_ms = now;
    _ble_last_nonzero_ms = now;

    DBG_INFO(TAG, "Tamper detection initialized, threshold=%lus, check_interval=%lus",
             (unsigned long)(_config.silence_threshold_ms / 1000),
             (unsigned long)(_config.check_interval_ms / 1000));
    return true;
}

bool tick() {
    if (!_initialized) return false;

    uint32_t now = millis();
    if (_last_check_ms != 0 && (now - _last_check_ms) < _config.check_interval_ms) {
        return false;
    }
    _last_check_ms = now;

    // Don't check during first 2 minutes of boot (scanners still initializing)
    if (now < 120000) return false;

    bool wifi_silent = false;
    bool ble_silent = false;
    uint32_t wifi_silence_s = 0;
    uint32_t ble_silence_s = 0;

    // --- Check WiFi scanner ---
#if HAS_WIFI_SCANNER
    if (_config.alert_on_wifi && hal_wifi_scanner::is_active()) {
        int wifi_count = hal_wifi_scanner::get_visible_count();
        if (wifi_count > 0) {
            _wifi_last_nonzero_ms = now;
            _wifi_was_zero = false;
            _wifi_alert_sent = false;  // Reset so we can alert again if it goes silent later
        } else {
            _wifi_was_zero = true;
            uint32_t silence_ms = now - _wifi_last_nonzero_ms;
            wifi_silence_s = silence_ms / 1000;
            if (silence_ms >= _config.silence_threshold_ms) {
                wifi_silent = true;
            }
        }
    }
#endif

    // --- Check BLE scanner ---
#if HAS_BLE_SCANNER
    if (_config.alert_on_ble && hal_ble_scanner::is_active()) {
        int ble_count = hal_ble_scanner::get_device_count();
        if (ble_count > 0) {
            _ble_last_nonzero_ms = now;
            _ble_was_zero = false;
            _ble_alert_sent = false;
        } else {
            _ble_was_zero = true;
            uint32_t silence_ms = now - _ble_last_nonzero_ms;
            ble_silence_s = silence_ms / 1000;
            if (silence_ms >= _config.silence_threshold_ms) {
                ble_silent = true;
            }
        }
    }
#endif

    // --- Determine tamper type ---
    TamperType new_type = TamperType::NONE;
    uint32_t max_silence = 0;

    if (wifi_silent && ble_silent) {
        new_type = TamperType::BOTH_SILENCE;
        max_silence = (wifi_silence_s > ble_silence_s) ? wifi_silence_s : ble_silence_s;
    } else if (wifi_silent) {
        new_type = TamperType::WIFI_SILENCE;
        max_silence = wifi_silence_s;
    } else if (ble_silent) {
        new_type = TamperType::BLE_SILENCE;
        max_silence = ble_silence_s;
    }

    // --- Transition to alert state ---
    if (new_type != TamperType::NONE) {
        bool should_publish = false;

        if (new_type == TamperType::WIFI_SILENCE && !_wifi_alert_sent) {
            _wifi_alert_sent = true;
            should_publish = true;
        } else if (new_type == TamperType::BLE_SILENCE && !_ble_alert_sent) {
            _ble_alert_sent = true;
            should_publish = true;
        } else if (new_type == TamperType::BOTH_SILENCE &&
                   (!_wifi_alert_sent || !_ble_alert_sent)) {
            _wifi_alert_sent = true;
            _ble_alert_sent = true;
            should_publish = true;
        }

        // Update alert state
        _alert.type = new_type;
        _alert.silence_duration_s = max_silence;
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        _alert.timestamp_epoch = (uint32_t)tv.tv_sec;
        _alert_active = true;

        if (should_publish) {
            publish_tamper_alert(new_type, max_silence);
            _alert.published = true;
            return true;
        }
    } else if (_alert_active) {
        // Condition cleared — sensors are seeing devices again
        DBG_INFO(TAG, "Tamper condition cleared — sensors reporting normally");
        _alert_active = false;
        _alert.type = TamperType::NONE;
    }

    return false;
}

TamperType get_status() {
    return _alert_active ? _alert.type : TamperType::NONE;
}

bool is_alert_active() {
    return _alert_active;
}

const TamperAlert& get_alert() {
    return _alert;
}

void clear_alert() {
    _alert_active = false;
    _alert.type = TamperType::NONE;
    _wifi_alert_sent = false;
    _ble_alert_sent = false;
    DBG_INFO(TAG, "Tamper alert cleared by operator");
}

int to_json(char* buf, size_t size) {
    if (!_alert_active) {
        return snprintf(buf, size, "{\"status\":\"ok\"}");
    }

    return snprintf(buf, size,
        "{\"status\":\"alert\",\"type\":\"%s\","
        "\"silence_s\":%lu,\"epoch\":%lu,\"published\":%s}",
        tamper_type_str(_alert.type),
        (unsigned long)_alert.silence_duration_s,
        (unsigned long)_alert.timestamp_epoch,
        _alert.published ? "true" : "false");
}

}  // namespace hal_tamper_detect

#endif  // SIMULATOR
