/// @file wifi_fingerprint_service.h
/// @brief WiFi RSSI fingerprint collection service — wraps hal_wifi_fingerprint as a ServiceInterface.
/// @copyright 2026 Valpatel Software LLC
/// @license AGPL-3.0-or-later
///
/// Priority 56: init after WiFi manager (10) and MQTT (50).
/// Collects WiFi RSSI fingerprints at known positions for indoor
/// positioning. Published via MQTT to tritium/{device_id}/wifi_fingerprint.
///
/// Serial commands:
///   FINGERPRINT_STATUS — show collection status
///   FINGERPRINT_START — enable collection mode
///   FINGERPRINT_STOP — disable collection mode
///   FINGERPRINT_RECORD <lat> <lon> <floor> [room_id] — record at position
///   FINGERPRINT_LIST — show stored fingerprints
///   FINGERPRINT_PUBLISH — publish unpublished fingerprints via MQTT
///   FINGERPRINT_CLEAR — clear stored fingerprints
#pragma once

#include "service.h"
#include "hal_wifi_fingerprint.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if __has_include("hal_mqtt.h")
#include "hal_mqtt.h"
#define WFFP_HAS_MQTT 1
#else
#define WFFP_HAS_MQTT 0
#endif

class WifiFingerprintService : public ServiceInterface {
public:
    const char* name() const override { return "wifi_fingerprint"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD | SVC_WEB_API; }
    int initPriority() const override { return 56; }

    bool init() override {
        hal_wifi_fingerprint::FingerprintConfig cfg;
        cfg.enabled = false;  // Collection mode off by default
        cfg.scan_count = 3;
        cfg.scan_delay_ms = 500;

        if (hal_wifi_fingerprint::init(cfg)) {
            Serial.printf("[tritium] WiFi fingerprint: ready (collection off)\n");
            _initialized = true;
            return true;
        }
        Serial.printf("[tritium] WiFi fingerprint: init failed\n");
        return false;
    }

    void tick() override {
        if (!_initialized) return;

#if WFFP_HAS_MQTT && !defined(SIMULATOR)
        // Auto-publish unpublished fingerprints every 30s
        uint32_t now = millis();
        if ((now - _lastPublish) >= 30000) {
            _lastPublish = now;
            int unpub = hal_wifi_fingerprint::get_unpublished_count();
            if (unpub > 0) {
                _publishFingerprints();
            }
        }
#endif
    }

    void shutdown() override {
        hal_wifi_fingerprint::shutdown();
        _initialized = false;
    }

    bool handleCommand(const char* cmd, const char* args) override {
        if (strcmp(cmd, "FINGERPRINT_STATUS") == 0) {
            Serial.printf("[fingerprint] Collecting: %s, Stored: %d, Unpublished: %d\n",
                          hal_wifi_fingerprint::is_collecting() ? "yes" : "no",
                          hal_wifi_fingerprint::get_stored_count(),
                          hal_wifi_fingerprint::get_unpublished_count());
            return true;
        }
        if (strcmp(cmd, "FINGERPRINT_START") == 0) {
            hal_wifi_fingerprint::set_collecting(true);
            Serial.printf("[fingerprint] Collection mode: ON\n");
            return true;
        }
        if (strcmp(cmd, "FINGERPRINT_STOP") == 0) {
            hal_wifi_fingerprint::set_collecting(false);
            Serial.printf("[fingerprint] Collection mode: OFF\n");
            return true;
        }
        if (strcmp(cmd, "FINGERPRINT_RECORD") == 0) {
            return _handleRecord(args);
        }
        if (strcmp(cmd, "FINGERPRINT_LIST") == 0) {
            _handleList();
            return true;
        }
        if (strcmp(cmd, "FINGERPRINT_PUBLISH") == 0) {
            int count = _publishFingerprints();
            Serial.printf("[fingerprint] Published %d fingerprints\n", count);
            return true;
        }
        if (strcmp(cmd, "FINGERPRINT_CLEAR") == 0) {
            hal_wifi_fingerprint::clear();
            Serial.printf("[fingerprint] Cleared all fingerprints\n");
            return true;
        }
        return false;
    }

    int toJson(char* buf, size_t size) override {
        return snprintf(buf, size,
            "{\"collecting\":%s,\"stored\":%d,\"unpublished\":%d}",
            hal_wifi_fingerprint::is_collecting() ? "true" : "false",
            hal_wifi_fingerprint::get_stored_count(),
            hal_wifi_fingerprint::get_unpublished_count());
    }

private:
    bool _initialized = false;
    uint32_t _lastPublish = 0;

    bool _handleRecord(const char* args) {
        if (!args || !*args) {
            Serial.printf("[fingerprint] Usage: FINGERPRINT_RECORD <lat> <lon> <floor> [room_id]\n");
            return true;
        }

        // Parse: lat lon floor [room_id]
        double lat = 0, lon = 0;
        int floor_level = 0;
        char room_id[33] = {};

        // Simple space-separated parse
        char buf[128];
        strncpy(buf, args, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;

        char* tok = strtok(buf, " ");
        if (tok) lat = atof(tok);
        tok = strtok(nullptr, " ");
        if (tok) lon = atof(tok);
        tok = strtok(nullptr, " ");
        if (tok) floor_level = atoi(tok);
        tok = strtok(nullptr, " ");
        if (tok) { strncpy(room_id, tok, 32); room_id[32] = 0; }

        if (lat == 0 && lon == 0) {
            Serial.printf("[fingerprint] Invalid coordinates\n");
            return true;
        }

        Serial.printf("[fingerprint] Recording at (%.6f, %.6f) floor=%d...\n",
                       lat, lon, floor_level);

        uint32_t id = hal_wifi_fingerprint::record_fingerprint(
            lat, lon, (int8_t)floor_level,
            room_id[0] ? room_id : nullptr);

        if (id > 0) {
            Serial.printf("[fingerprint] Recorded fingerprint #%u\n", id);
        } else {
            Serial.printf("[fingerprint] Failed to record (not collecting or storage full)\n");
        }
        return true;
    }

    void _handleList() {
        int count = hal_wifi_fingerprint::get_stored_count();
        Serial.printf("[fingerprint] %d stored fingerprints:\n", count);

        hal_wifi_fingerprint::Fingerprint fps[hal_wifi_fingerprint::FP_MAX_STORED];
        int n = hal_wifi_fingerprint::get_fingerprints(fps, hal_wifi_fingerprint::FP_MAX_STORED);

        for (int i = 0; i < n; i++) {
            Serial.printf("  #%u (%.6f, %.6f) floor=%d room=%s aps=%d %s\n",
                fps[i].id, fps[i].lat, fps[i].lon, fps[i].floor_level,
                fps[i].room_id[0] ? fps[i].room_id : "-",
                fps[i].ap_count,
                fps[i].published ? "[published]" : "[pending]");
        }
    }

    int _publishFingerprints() {
#if WFFP_HAS_MQTT && !defined(SIMULATOR)
        hal_wifi_fingerprint::Fingerprint fps[hal_wifi_fingerprint::FP_MAX_STORED];
        int count = hal_wifi_fingerprint::get_fingerprints(fps, hal_wifi_fingerprint::FP_MAX_STORED);
        int published = 0;

        char buf[1024];
        for (int i = 0; i < count; i++) {
            if (fps[i].published) continue;

            int len = hal_wifi_fingerprint::get_fingerprint_json(fps[i], buf, sizeof(buf));
            if (len > 0) {
                // Publish to wifi_fingerprint topic
                hal_mqtt::publish("wifi_fingerprint", buf, len);
                hal_wifi_fingerprint::mark_published(fps[i].id);
                published++;
            }
        }
        return published;
#else
        return 0;
#endif
    }
};
