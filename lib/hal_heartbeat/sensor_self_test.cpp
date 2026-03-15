// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "sensor_self_test.h"
#include "debug_log.h"
#include <cstdio>
#include <cstring>

static constexpr const char* TAG = "self-test";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace sensor_self_test {

static SelfTestResult _result = {};
static uint32_t _interval_ms = 3600000;

void init(uint32_t interval_ms) {
    _interval_ms = interval_ms;
    _result.overall_pass = true;
    _result.wifi_scan_ok = true;
    _result.ble_scan_ok = true;
    _result.heap_ok = true;
    _result.ntp_sync_ok = true;
    _result.free_heap = 200000;
    _result.min_free_heap = 180000;
    _result.largest_block = 180000;
    _result.fragmentation_pct = 10.0f;
    _result.ntp_age_s = 0;
    DBG_INFO(TAG, "Self-test init (simulator stub)");
}

bool tick() { return false; }
void run_now() {}
const SelfTestResult& get_result() { return _result; }
int to_json(char* buf, size_t buf_size) {
    return snprintf(buf, buf_size,
        "{\"pass\":true,\"wifi_ok\":true,\"ble_ok\":true,"
        "\"heap_ok\":true,\"free_heap\":200000,\"runs\":0}");
}

}  // namespace sensor_self_test

// ============================================================================
// ESP32 — real self-test
// ============================================================================
#else

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

// Optional BLE scanner
#if __has_include("hal_ble_scanner.h")
#include "hal_ble_scanner.h"
#define HAS_BLE_FOR_TEST 1
#else
#define HAS_BLE_FOR_TEST 0
#endif

// Optional WiFi scanner
#if __has_include("hal_wifi_scanner.h")
#include "hal_wifi_scanner.h"
#define HAS_WIFI_SCANNER_FOR_TEST 1
#else
#define HAS_WIFI_SCANNER_FOR_TEST 0
#endif

namespace sensor_self_test {

static SelfTestResult _result = {};
static uint32_t _interval_ms = 3600000;
static uint32_t _last_run_ms = 0;
static bool _initialized = false;

// Minimum heap threshold — below this is unhealthy
static constexpr uint32_t MIN_HEAP_THRESHOLD = 30000;  // 30KB

void init(uint32_t interval_ms) {
    _interval_ms = interval_ms;
    _initialized = true;
    memset(&_result, 0, sizeof(_result));

    // Run first test shortly after boot (30s delay to let things settle)
    _last_run_ms = millis();  // Will run after interval
    DBG_INFO(TAG, "Sensor self-test initialized (interval=%lus)",
             (unsigned long)(_interval_ms / 1000));
}

static void _run_test() {
    DBG_INFO(TAG, "Running sensor self-test...");
    _result.last_run_ms = millis();
    _result.run_count++;
    _result.overall_pass = true;

    // --- WiFi scan test ---
    _result.wifi_scan_ok = false;
    _result.wifi_scan_count = 0;
    if (WiFi.isConnected()) {
        // WiFi is connected, basic connectivity check passes
        _result.wifi_scan_ok = true;
        _result.wifi_scan_count = 1;  // At least connected to one AP

#if HAS_WIFI_SCANNER_FOR_TEST
        if (hal_wifi_scanner::is_active()) {
            int count = hal_wifi_scanner::get_visible_count();
            _result.wifi_scan_count = count;
            _result.wifi_scan_ok = (count > 0);
        }
#endif
    } else {
        _result.overall_pass = false;
        DBG_WARN(TAG, "WiFi scan FAIL: not connected");
    }

    // --- BLE scan test ---
#if HAS_BLE_FOR_TEST
    _result.ble_scan_ok = hal_ble_scanner::is_active();
    if (!_result.ble_scan_ok) {
        DBG_DEBUG(TAG, "BLE scanner not active (may be disabled for WiFi coex)");
        // BLE not active is not necessarily a failure — coexistence issue
        // Don't fail overall for this
    }
#else
    _result.ble_scan_ok = true;  // N/A — not compiled in, not a failure
#endif

    // --- Heap health test ---
    _result.free_heap = (uint32_t)ESP.getFreeHeap();
    _result.min_free_heap = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    _result.heap_ok = (_result.free_heap >= MIN_HEAP_THRESHOLD);
    if (!_result.heap_ok) {
        _result.overall_pass = false;
        DBG_WARN(TAG, "Heap FAIL: free=%u min=%u threshold=%u",
                 _result.free_heap, _result.min_free_heap, MIN_HEAP_THRESHOLD);
    }

    DBG_INFO(TAG, "Self-test #%lu: %s (wifi=%s[%d] ble=%s heap=%u/%u)",
             (unsigned long)_result.run_count,
             _result.overall_pass ? "PASS" : "FAIL",
             _result.wifi_scan_ok ? "ok" : "FAIL",
             _result.wifi_scan_count,
             _result.ble_scan_ok ? "ok" : "n/a",
             _result.free_heap, _result.min_free_heap);
}

bool tick() {
    if (!_initialized) return false;

    uint32_t now = millis();
    if (_last_run_ms != 0 && (now - _last_run_ms) < _interval_ms) {
        return false;
    }
    _last_run_ms = now;
    _run_test();
    return true;
}

void run_now() {
    _run_test();
}

const SelfTestResult& get_result() {
    return _result;
}

int to_json(char* buf, size_t buf_size) {
    return snprintf(buf, buf_size,
        "{\"pass\":%s,\"wifi_ok\":%s,\"wifi_count\":%d,"
        "\"ble_ok\":%s,\"heap_ok\":%s,"
        "\"free_heap\":%u,\"min_free_heap\":%u,\"runs\":%u}",
        _result.overall_pass ? "true" : "false",
        _result.wifi_scan_ok ? "true" : "false",
        _result.wifi_scan_count,
        _result.ble_scan_ok ? "true" : "false",
        _result.heap_ok ? "true" : "false",
        _result.free_heap,
        _result.min_free_heap,
        _result.run_count);
}

}  // namespace sensor_self_test

#endif  // SIMULATOR
