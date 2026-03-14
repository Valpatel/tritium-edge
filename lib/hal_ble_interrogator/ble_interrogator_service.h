// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once
// BLE Interrogator service adapter — wraps hal_ble_interrogator as a ServiceInterface.
// Priority 55 (after BLE scanner at 50).

#include "service.h"

#if defined(ENABLE_BLE_INTERROGATOR) && __has_include("hal_ble_interrogator.h")
#include "hal_ble_interrogator.h"
#endif

class BleInterrogatorService : public ServiceInterface {
public:
    const char* name() const override { return "ble_interrogator"; }
    uint8_t capabilities() const override { return SVC_SERIAL_CMD | SVC_WEB_API; }
    int initPriority() const override { return 55; }

    bool init() override {
#if defined(ENABLE_BLE_INTERROGATOR)
        hal_ble_interrogator::InterrogatorConfig cfg;
        cfg.cooldown_ms = 3600000;   // 1 hour cooldown
        cfg.timeout_ms = 2000;       // 2 second connection timeout
        cfg.auto_interrogate_unknown = true;
        if (hal_ble_interrogator::init(cfg)) {
            Serial.printf("[tritium] BLE Interrogator: active\n");
            _active = true;
            return true;
        } else {
            Serial.printf("[tritium] BLE Interrogator: failed to start\n");
            return false;
        }
#else
        return false;
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(ENABLE_BLE_INTERROGATOR)
        // BLE_QUERY AA:BB:CC:DD:EE:FF — queue a device for interrogation
        if (strcmp(cmd, "BLE_QUERY") == 0) {
            if (!args || args[0] == '\0') {
                Serial.printf("[ble_interrog] Usage: BLE_QUERY AA:BB:CC:DD:EE:FF\n");
                return true;
            }
            if (hal_ble_interrogator::queue_interrogation_by_mac(args)) {
                Serial.printf("[ble_interrog] Queued %s for interrogation\n", args);
            } else {
                Serial.printf("[ble_interrog] Failed to queue %s (full, cooldown, or duplicate)\n", args);
            }
            return true;
        }
        // BLE_PROFILE AA:BB:CC:DD:EE:FF — show cached interrogation result
        if (strcmp(cmd, "BLE_PROFILE") == 0) {
            if (!args || args[0] == '\0') {
                Serial.printf("[ble_interrog] Usage: BLE_PROFILE AA:BB:CC:DD:EE:FF\n");
                return true;
            }
            BleDeviceProfile profile;
            if (hal_ble_interrogator::get_result_by_mac(args, &profile)) {
                char json_buf[1024];
                hal_ble_interrogator::profile_to_json(&profile, json_buf, sizeof(json_buf));
                Serial.printf("[ble_interrog] Profile: %s\n", json_buf);
            } else {
                Serial.printf("[ble_interrog] No profile for %s\n", args);
            }
            return true;
        }
        // BLE_INTERROG_STATUS — show interrogator status
        if (strcmp(cmd, "BLE_INTERROG_STATUS") == 0) {
            char json_buf[256];
            hal_ble_interrogator::get_status_json(json_buf, sizeof(json_buf));
            Serial.printf("[ble_interrog] Status: %s\n", json_buf);
            return true;
        }
        // BLE_PROFILES — list all cached profiles
        if (strcmp(cmd, "BLE_PROFILES") == 0) {
            char json_buf[4096];
            hal_ble_interrogator::get_all_profiles_json(json_buf, sizeof(json_buf));
            Serial.printf("[ble_interrog] Profiles: %s\n", json_buf);
            return true;
        }
#endif
        return false;
    }

    int toJson(char* buf, size_t size) override {
#if defined(ENABLE_BLE_INTERROGATOR)
        if (!hal_ble_interrogator::is_active()) return 0;
        return hal_ble_interrogator::get_status_json(buf, size);
#else
        return 0;
#endif
    }

private:
    bool _active = false;
};
