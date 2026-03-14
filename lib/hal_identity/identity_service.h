// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once
// Device Identity Service — ServiceInterface wrapper for hal_identity.
// Initializes at priority 5 (before WiFi, MQTT, heartbeat) so the UUID
// is available to all downstream services.

#include "service.h"
#include "hal_identity.h"

class IdentityService : public ServiceInterface {
public:
    const char* name() const override { return "identity"; }
    uint8_t capabilities() const override { return SVC_SERIAL_CMD | SVC_WEB_API; }
    int initPriority() const override { return 5; }  // Very early — before networking

    bool init() override {
        return hal_identity::init();
    }

    bool handleCommand(const char* cmd, const char* args) override {
        if (strcmp(cmd, "UUID") == 0 || strcmp(cmd, "uuid") == 0) {
            Serial.printf("Device UUID: %s\n", hal_identity::get_uuid());
            Serial.printf("Short ID: %s\n", hal_identity::get_short_id());
            Serial.printf("Device Name: %s\n", hal_identity::get_device_name());
            return true;
        }
        if (strcmp(cmd, "SET_NAME") == 0 || strcmp(cmd, "set_name") == 0) {
            if (args && args[0]) {
                hal_identity::set_device_name(args);
                Serial.printf("Device name set to: %s\n", hal_identity::get_device_name());
            } else {
                Serial.printf("Usage: SET_NAME <name>\n");
            }
            return true;
        }
        if (strcmp(cmd, "REGEN_UUID") == 0 || strcmp(cmd, "regen_uuid") == 0) {
            hal_identity::regenerate();
            Serial.printf("New UUID: %s\n", hal_identity::get_uuid());
            return true;
        }
        return false;
    }

    int toJson(char* buf, size_t size) override {
        return snprintf(buf, size,
            "{\"uuid\":\"%s\",\"short_id\":\"%s\",\"name\":\"%s\"}",
            hal_identity::get_uuid(),
            hal_identity::get_short_id(),
            hal_identity::get_device_name());
    }
};
