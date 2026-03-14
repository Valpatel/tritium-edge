// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once
// BLE Feature Extraction service adapter — wraps hal_ble_features as a ServiceInterface.
// Depends on BLE scanner being active. Priority 55 (after BLE scanner at 50).

#include "service.h"

#if defined(ENABLE_BLE_SCANNER) && __has_include("hal_ble_features.h")
#include "hal_ble_features.h"
#include "hal_ble_scanner.h"
#endif

class BleFeatureService : public ServiceInterface {
public:
    const char* name() const override { return "ble_features"; }
    uint8_t capabilities() const override { return SVC_SERIAL_CMD; }
    int initPriority() const override { return 55; }

    bool init() override {
#if defined(ENABLE_BLE_SCANNER)
        // Feature extraction doesn't need separate init — it operates
        // on BleDevice structs from the scanner.
        _active = hal_ble_scanner::is_active();
        if (_active) {
            Serial.printf("[tritium] BLE Features: active (v%d)\n",
                          hal_ble_features::get_version());
        }
        return _active;
#else
        return false;
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(ENABLE_BLE_SCANNER)
        if (strcmp(cmd, "BLE_FEATURES") == 0) {
            BleDevice devs[8];
            int n = hal_ble_scanner::get_devices(devs, 8);
            char buf[2048];
            int len = hal_ble_features::to_json_array(devs, n, buf, sizeof(buf));
            if (len > 0) {
                Serial.printf("[ble_features] %s\n", buf);
            } else {
                Serial.printf("[ble_features] No devices\n");
            }
            return true;
        }
        if (strcmp(cmd, "BLE_FEEDBACK_COUNT") == 0) {
            Serial.printf("[ble_features] Cached classifications: %d\n",
                          hal_ble_features::get_feedback_count());
            return true;
        }
#endif
        return false;
    }

    int toJson(char* buf, size_t size) override {
#if defined(ENABLE_BLE_SCANNER)
        if (!_active) return 0;
        return snprintf(buf, size,
            "{\"active\":true,\"version\":%d,\"feedback_cached\":%d}",
            hal_ble_features::get_version(),
            hal_ble_features::get_feedback_count());
#else
        return 0;
#endif
    }

private:
    bool _active = false;
};
