// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_radio_scheduler.h"
#include <cstdio>
#include <cstring>

#ifdef SIMULATOR

namespace hal_radio_scheduler {
bool init(const Config&) { return false; }
void shutdown() {}
RadioMode get_mode() { return RadioMode::IDLE; }
uint32_t get_slot_remaining_ms() { return 0; }
void force_mode(RadioMode) {}
bool is_wifi_available() { return true; }
bool is_ble_available() { return false; }
bool is_active() { return false; }
int get_status_json(char* buf, size_t s) { return snprintf(buf, s, "{}"); }
}

#else

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if __has_include("debug_log.h")
#include "debug_log.h"
#endif

#if __has_include("os_events.h")
#include "os_events.h"
#define HAS_OS_EVENTS 1
#else
#define HAS_OS_EVENTS 0
#endif

static constexpr const char* TAG = "radio_sched";

namespace hal_radio_scheduler {

// --- Internal state ---
static Config _config;
static volatile RadioMode _mode = RadioMode::IDLE;
static volatile bool _running = false;
static TaskHandle_t _task = nullptr;
static uint32_t _slot_start_ms = 0;
static uint32_t _wifi_cycles = 0;
static uint32_t _ble_cycles = 0;
static volatile RadioMode _force_mode = RadioMode::IDLE;
static volatile bool _force_pending = false;

// Forward declarations for radio stack management
static bool start_wifi();
static void stop_wifi();
static bool start_ble_scan();
static void stop_ble_scan();

// --- WiFi stack management ---

static bool start_wifi() {
    // WiFi is managed by WifiManager — we just signal readiness.
    // The WifiManager auto-reconnects when WiFi hardware is available.
    // In a full implementation, this would call esp_wifi_start().
    DBG_INFO(TAG, "WiFi slot: starting");
    return true;
}

static void stop_wifi() {
    // Signal WiFi to disconnect gracefully before BLE takes over.
    // In a full implementation: esp_wifi_disconnect() + esp_wifi_stop()
    // For now, WiFi stays connected — the BLE slot just won't init BLE
    // until the coexistence issue is fixed in ESP-IDF 5.5.2+.
    DBG_INFO(TAG, "WiFi slot: pausing");
}

static bool start_ble_scan() {
    // BLE scan start — requires NimBLE init.
    // Currently blocked by coexistence issue (esp_bt.h not found / memory).
    // When ESP-IDF 5.5.2 fixes this, call hal_ble_scanner::init() here.
    DBG_INFO(TAG, "BLE slot: starting (stub — coex fix pending)");
    return true;
}

static void stop_ble_scan() {
    // Stop BLE scanning and deinit the stack to free memory for WiFi.
    // When implemented: hal_ble_scanner::shutdown() + NimBLEDevice::deinit(true)
    DBG_INFO(TAG, "BLE slot: stopping");
}

// --- Scheduler task ---

static void scheduler_task(void* param) {
    RadioMode next = _config.wifi_first ? RadioMode::WIFI_ACTIVE : RadioMode::BLE_SCANNING;

    while (_running) {
        // Check for forced mode override
        if (_force_pending) {
            next = _force_mode;
            _force_pending = false;
        }

        // Transition phase
        _mode = RadioMode::TRANSITIONING;
#if HAS_OS_EVENTS
        os_events::publish("radio_mode", "transitioning");
#endif

        // Tear down current stack
        stop_wifi();
        stop_ble_scan();
        vTaskDelay(pdMS_TO_TICKS(_config.transition_ms));

        // Start next stack
        _slot_start_ms = millis();

        if (next == RadioMode::WIFI_ACTIVE && _config.enable_wifi) {
            start_wifi();
            _mode = RadioMode::WIFI_ACTIVE;
            _wifi_cycles++;
#if HAS_OS_EVENTS
            os_events::publish("radio_mode", "wifi");
#endif
            DBG_INFO(TAG, "WiFi slot active (cycle %lu)", (unsigned long)_wifi_cycles);
            vTaskDelay(pdMS_TO_TICKS(_config.wifi_slot_ms));
            next = _config.enable_ble ? RadioMode::BLE_SCANNING : RadioMode::WIFI_ACTIVE;

        } else if (next == RadioMode::BLE_SCANNING && _config.enable_ble) {
            start_ble_scan();
            _mode = RadioMode::BLE_SCANNING;
            _ble_cycles++;
#if HAS_OS_EVENTS
            os_events::publish("radio_mode", "ble");
#endif
            DBG_INFO(TAG, "BLE slot active (cycle %lu)", (unsigned long)_ble_cycles);
            vTaskDelay(pdMS_TO_TICKS(_config.ble_slot_ms));
            next = _config.enable_wifi ? RadioMode::WIFI_ACTIVE : RadioMode::BLE_SCANNING;

        } else {
            // Neither enabled — just idle
            _mode = RadioMode::IDLE;
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    _mode = RadioMode::IDLE;
    vTaskDelete(nullptr);
}

// --- Public API ---

bool init(const Config& config) {
    if (_running) return true;

    _config = config;
    _wifi_cycles = 0;
    _ble_cycles = 0;
    _force_pending = false;
    _running = true;

    xTaskCreatePinnedToCore(scheduler_task, "radio_sched", 4096, nullptr, 2, &_task, 1);

    DBG_INFO(TAG, "Started: wifi=%ums ble=%ums transition=%ums",
        config.wifi_slot_ms, config.ble_slot_ms, config.transition_ms);
    return true;
}

void shutdown() {
    _running = false;
    if (_task) {
        vTaskDelay(pdMS_TO_TICKS(500));
        _task = nullptr;
    }
    stop_wifi();
    stop_ble_scan();
    _mode = RadioMode::IDLE;
}

RadioMode get_mode() { return _mode; }

uint32_t get_slot_remaining_ms() {
    if (_mode == RadioMode::IDLE || _mode == RadioMode::TRANSITIONING) return 0;
    uint32_t elapsed = millis() - _slot_start_ms;
    uint32_t slot_dur = (_mode == RadioMode::WIFI_ACTIVE)
        ? _config.wifi_slot_ms : _config.ble_slot_ms;
    return (elapsed < slot_dur) ? (slot_dur - elapsed) : 0;
}

void force_mode(RadioMode mode) {
    _force_mode = mode;
    _force_pending = true;
}

bool is_wifi_available() {
    return _mode == RadioMode::WIFI_ACTIVE || !_config.enable_ble;
}

bool is_ble_available() {
    return _mode == RadioMode::BLE_SCANNING || !_config.enable_wifi;
}

bool is_active() { return _running; }

int get_status_json(char* buf, size_t s) {
    const char* mode_str = "idle";
    switch (_mode) {
        case RadioMode::WIFI_ACTIVE:   mode_str = "wifi"; break;
        case RadioMode::BLE_SCANNING:  mode_str = "ble"; break;
        case RadioMode::TRANSITIONING: mode_str = "transition"; break;
        default: break;
    }
    return snprintf(buf, s,
        "{\"mode\":\"%s\",\"wifi_cycles\":%lu,\"ble_cycles\":%lu,"
        "\"slot_remaining_ms\":%lu}",
        mode_str,
        (unsigned long)_wifi_cycles, (unsigned long)_ble_cycles,
        (unsigned long)get_slot_remaining_ms());
}

}  // namespace hal_radio_scheduler

#endif  // SIMULATOR
