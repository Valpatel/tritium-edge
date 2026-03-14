/// @file wifi_probe_service.h
/// @brief WiFi probe request capture service — wraps hal_wifi_probe as a ServiceInterface.
/// @copyright 2026 Valpatel Software LLC
/// @license AGPL-3.0-or-later
///
/// Priority 55: init after WiFi manager (10) and MQTT (50).
/// Captures passive WiFi probe requests from nearby devices and publishes
/// them as wifi_probe sightings via MQTT for the unified target picture.
///
/// Serial commands: WIFI_PROBE_STATUS, WIFI_PROBE_LIST
#pragma once

#include "service.h"
#include "hal_wifi_probe.h"
#include <cstdio>
#include <cstring>

#if __has_include("hal_mqtt.h")
#include "hal_mqtt.h"
#define PROBE_HAS_MQTT 1
#else
#define PROBE_HAS_MQTT 0
#endif

class WifiProbeService : public ServiceInterface {
public:
    const char* name() const override { return "wifi_probe"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD | SVC_WEB_API; }
    int initPriority() const override { return 55; }

    bool init() override {
#ifndef SIMULATOR
        hal_wifi_probe::ProbeConfig cfg;
        cfg.enabled = true;
        cfg.channel_hop = false;  // Don't hop channels by default (keeps WiFi connection stable)
        cfg.report_interval_ms = 15000;

        if (hal_wifi_probe::init(cfg)) {
            Serial.printf("[tritium] WiFi probe capture: active\n");
            _initialized = true;
            return true;
        }
        Serial.printf("[tritium] WiFi probe capture: failed to start\n");
#endif
        return false;
    }

    void tick() override {
#ifndef SIMULATOR
        if (!_initialized) return;

        uint32_t now = millis();
        if ((now - _lastReport) < _reportInterval) return;
        _lastReport = now;

        // Report new/updated devices via MQTT
        _publishSightings();
#endif
    }

    void shutdown() override {
        hal_wifi_probe::shutdown();
        _initialized = false;
    }

    bool handleCommand(const char* cmd, const char*) override {
        if (strcmp(cmd, "WIFI_PROBE_STATUS") == 0) {
#ifndef SIMULATOR
            int count = hal_wifi_probe::get_active_count();
            Serial.printf("[wifi_probe] Active: %s, Devices: %d\n",
                          hal_wifi_probe::is_active() ? "yes" : "no", count);
#endif
            return true;
        }
        if (strcmp(cmd, "WIFI_PROBE_LIST") == 0) {
#ifndef SIMULATOR
            hal_wifi_probe::ProbeDevice devs[hal_wifi_probe::PROBE_MAX_DEVICES];
            int count = hal_wifi_probe::get_devices(devs, hal_wifi_probe::PROBE_MAX_DEVICES);
            Serial.printf("[wifi_probe] %d devices:\n", count);
            for (int i = 0; i < count; i++) {
                Serial.printf("  %02X:%02X:%02X:%02X:%02X:%02X  rssi=%d  probes=%d  ssids=%d  %s\n",
                    devs[i].mac[0], devs[i].mac[1], devs[i].mac[2],
                    devs[i].mac[3], devs[i].mac[4], devs[i].mac[5],
                    devs[i].rssi, devs[i].probe_count, devs[i].ssid_count,
                    devs[i].is_randomized ? "(random)" : "");
            }
#endif
            return true;
        }
        return false;
    }

    int toJson(char* buf, size_t size) override {
        return hal_wifi_probe::get_summary_json(buf, size);
    }

private:
    bool _initialized = false;
    uint32_t _lastReport = 0;
    uint32_t _reportInterval = 15000;

    void _publishSightings() {
#if PROBE_HAS_MQTT && !defined(SIMULATOR)
        // Get all active devices
        hal_wifi_probe::ProbeDevice devs[hal_wifi_probe::PROBE_MAX_DEVICES];
        int count = hal_wifi_probe::get_devices(devs, hal_wifi_probe::PROBE_MAX_DEVICES);

        // Publish each device as a wifi_probe sighting
        char buf[512];
        for (int i = 0; i < count; i++) {
            int len = hal_wifi_probe::get_sighting_json(devs[i], buf, sizeof(buf));
            if (len > 0) {
                // Publish to tritium/{device_id}/sighting topic
                // The MQTT service handles the actual publish
                hal_mqtt::publish_sighting(buf, len);
            }
        }
#endif
    }
};
