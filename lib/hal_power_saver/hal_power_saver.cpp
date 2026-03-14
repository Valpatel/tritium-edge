// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_power_saver.h"
#include "debug_log.h"

#include <cstdio>

static constexpr const char* TAG = "power_saver";

PowerSaver& PowerSaver::instance() {
    static PowerSaver s;
    return s;
}

bool PowerSaver::init() {
    _state = PowerSaverState::NORMAL;
    _ble_interval = NORMAL_BLE_SCAN_INTERVAL_MS;
    _wifi_interval = NORMAL_WIFI_SCAN_INTERVAL_MS;
    _heartbeat_interval = NORMAL_HEARTBEAT_INTERVAL_MS;
    _brightness = NORMAL_BRIGHTNESS;
    _initialized = true;

    DBG_INFO(TAG, "Power saver initialized (enter=%d%%, exit=%d%%, critical=%d%%)",
             POWER_SAVER_ENTER_PCT, POWER_SAVER_EXIT_PCT, POWER_SAVER_CRITICAL_PCT);
    return true;
}

bool PowerSaver::update(int battery_pct, bool is_charging, bool is_usb_powered) {
    if (!_initialized) return false;

    PowerSaverState prev_state = _state;
    _last_battery_pct = battery_pct;
    _last_charging = is_charging;

    // If charging or on USB, exit power save
    if (is_charging || is_usb_powered) {
        if (_state != PowerSaverState::CHARGING && _state != PowerSaverState::NORMAL) {
            DBG_INFO(TAG, "Charging detected, exiting power-save mode");
            _state = PowerSaverState::CHARGING;
            exitPowerSave();
        } else if (_state == PowerSaverState::CHARGING) {
            // Stay in charging state (same as normal)
        } else {
            _state = PowerSaverState::NORMAL;
        }
        return _state != prev_state;
    }

    // On battery — check thresholds
    if (battery_pct < 0) {
        // Unknown battery level, stay in current state
        return false;
    }

    if (battery_pct <= POWER_SAVER_CRITICAL_PCT) {
        if (_state != PowerSaverState::CRITICAL) {
            DBG_WARN(TAG, "Battery CRITICAL (%d%%), entering critical power-save", battery_pct);
            enterCritical();
        }
    } else if (battery_pct <= POWER_SAVER_ENTER_PCT) {
        if (_state == PowerSaverState::NORMAL || _state == PowerSaverState::CHARGING) {
            DBG_INFO(TAG, "Battery low (%d%%), entering power-save mode", battery_pct);
            enterPowerSave();
        }
    } else if (battery_pct >= POWER_SAVER_EXIT_PCT) {
        if (_state == PowerSaverState::POWER_SAVE || _state == PowerSaverState::CRITICAL) {
            DBG_INFO(TAG, "Battery recovered (%d%%), exiting power-save mode", battery_pct);
            exitPowerSave();
        }
    }

    return _state != prev_state;
}

void PowerSaver::enterPowerSave() {
    _state = PowerSaverState::POWER_SAVE;
    _ble_interval = SAVER_BLE_SCAN_INTERVAL_MS;
    _wifi_interval = SAVER_WIFI_SCAN_INTERVAL_MS;
    _heartbeat_interval = SAVER_HEARTBEAT_INTERVAL_MS;
    _brightness = SAVER_BRIGHTNESS;
    applyIntervals();

    DBG_INFO(TAG, "Power-save mode: BLE=%lums, WiFi=%lums, HB=%lums, bright=%u",
             (unsigned long)_ble_interval, (unsigned long)_wifi_interval,
             (unsigned long)_heartbeat_interval, _brightness);
}

void PowerSaver::enterCritical() {
    _state = PowerSaverState::CRITICAL;
    _ble_interval = CRITICAL_BLE_SCAN_INTERVAL_MS;
    _wifi_interval = CRITICAL_WIFI_SCAN_INTERVAL_MS;
    _heartbeat_interval = CRITICAL_HEARTBEAT_INTERVAL_MS;
    _brightness = CRITICAL_BRIGHTNESS;
    applyIntervals();

    DBG_WARN(TAG, "CRITICAL power-save: BLE=%lums, WiFi=%lums, HB=%lums, bright=%u",
             (unsigned long)_ble_interval, (unsigned long)_wifi_interval,
             (unsigned long)_heartbeat_interval, _brightness);
}

void PowerSaver::exitPowerSave() {
    _state = PowerSaverState::NORMAL;
    _ble_interval = NORMAL_BLE_SCAN_INTERVAL_MS;
    _wifi_interval = NORMAL_WIFI_SCAN_INTERVAL_MS;
    _heartbeat_interval = NORMAL_HEARTBEAT_INTERVAL_MS;
    _brightness = NORMAL_BRIGHTNESS;
    applyIntervals();

    DBG_INFO(TAG, "Normal power mode restored");
}

void PowerSaver::applyIntervals() {
    // Notify other HALs of interval changes via weak-linked functions.
    // Each HAL can optionally implement these to adjust its scan rate.
    //
    // The HALs check PowerSaver::instance().getXxxInterval() on each
    // tick to adapt, so explicit notification is not strictly necessary,
    // but we log for diagnostics.

    // Display brightness is applied by the PowerService which already
    // manages screen state. We just expose the target brightness.
}

int PowerSaver::toJson(char* buf, size_t size) const {
    const char* state_str = "normal";
    switch (_state) {
        case PowerSaverState::NORMAL:     state_str = "normal"; break;
        case PowerSaverState::POWER_SAVE: state_str = "power_save"; break;
        case PowerSaverState::CRITICAL:   state_str = "critical"; break;
        case PowerSaverState::CHARGING:   state_str = "charging"; break;
    }

    int n = snprintf(buf, size,
        "{"
        "\"state\":\"%s\","
        "\"active\":%s,"
        "\"battery_pct\":%d,"
        "\"charging\":%s,"
        "\"ble_interval_ms\":%lu,"
        "\"wifi_interval_ms\":%lu,"
        "\"heartbeat_interval_ms\":%lu,"
        "\"brightness\":%u"
        "}",
        state_str,
        isActive() ? "true" : "false",
        _last_battery_pct,
        _last_charging ? "true" : "false",
        (unsigned long)_ble_interval,
        (unsigned long)_wifi_interval,
        (unsigned long)_heartbeat_interval,
        _brightness
    );

    if (n < 0 || (size_t)n >= size) {
        if (size > 0) buf[0] = '\0';
        return -1;
    }
    return n;
}
