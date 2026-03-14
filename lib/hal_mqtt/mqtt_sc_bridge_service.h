// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// MQTT SC Bridge service adapter — wraps mqtt_sc_bridge as a ServiceInterface.
// Priority 55 (after heartbeat at 50, before sighting_buffer at 60).

#include "service.h"

#if defined(ENABLE_MQTT_SC_BRIDGE) && __has_include("mqtt_sc_bridge.h")
#include "mqtt_sc_bridge.h"
#endif

class MqttScBridgeService : public ServiceInterface {
public:
    const char* name() const override { return "mqtt_sc_bridge"; }
    uint8_t capabilities() const override { return SVC_TICK; }
    int initPriority() const override { return 55; }

    bool init() override {
#if defined(ENABLE_MQTT_SC_BRIDGE)
        mqtt_sc_bridge::BridgeConfig cfg;
#if defined(DEFAULT_DEVICE_ID)
        cfg.device_id = DEFAULT_DEVICE_ID;
#endif
#if defined(DEFAULT_MQTT_BROKER)
        cfg.broker = DEFAULT_MQTT_BROKER;
#endif
#if defined(DEFAULT_MQTT_PORT)
        cfg.port = DEFAULT_MQTT_PORT;
#endif
        cfg.heartbeat_interval_ms = 30000;  // 30s heartbeats via MQTT
        cfg.sighting_interval_ms = 15000;   // 15s sighting updates
        if (mqtt_sc_bridge::init(cfg)) {
            Serial.printf("[tritium] MQTT SC Bridge: active\n");
            _active = true;
            return true;
        } else {
            Serial.printf("[tritium] MQTT SC Bridge: not configured\n");
            return false;
        }
#else
        return false;
#endif
    }

    void tick() override {
#if defined(ENABLE_MQTT_SC_BRIDGE)
        if (_active) mqtt_sc_bridge::tick();
#endif
    }

private:
    bool _active = false;
};
