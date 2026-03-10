#pragma once
// CoT/TAK service adapter — wraps hal_cot namespace as a ServiceInterface.
// Priority 60.

#include "service.h"
#include "service_registry.h"

#if defined(ENABLE_COT) && __has_include("hal_cot.h")
#include "hal_cot.h"
#endif

#if defined(ENABLE_WIFI) && __has_include("wifi_service.h")
#include "wifi_service.h"
#endif

class CotService : public ServiceInterface {
public:
    const char* name() const override { return "cot"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD; }
    int initPriority() const override { return 60; }

    bool init() override {
#if defined(ENABLE_COT)
        // Check WiFi via registry
#if defined(ENABLE_WIFI)
        auto* wifi_svc = ServiceRegistry::getAs<WifiService>("wifi");
        if (!wifi_svc || !wifi_svc->isConnected()) return false;
#else
        return false;
#endif
        hal_cot::CotConfig cot_cfg;
#if defined(DEFAULT_DEVICE_ID)
        cot_cfg.device_id = DEFAULT_DEVICE_ID;
#endif
        cot_cfg.interval_ms = 60000;  // CoT SA every 60s
#if HAS_CAMERA
        cot_cfg.cot_type = hal_cot::COT_TYPE_CAMERA;
#endif
        if (hal_cot::init(cot_cfg)) {
            Serial.printf("[tritium] CoT/TAK: active (uid=%s callsign=%s)\n",
                          cot_cfg.device_id ? cot_cfg.device_id : "auto",
                          cot_cfg.callsign ? cot_cfg.callsign : "auto");
            _active = true;
            return true;
        } else {
            Serial.printf("[tritium] CoT/TAK: init failed\n");
            return false;
        }
#else
        return false;
#endif
    }

    void tick() override {
#if defined(ENABLE_COT)
        if (_active) hal_cot::tick();
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(ENABLE_COT)
        if (strcmp(cmd, "COT_SEND") == 0) {
            if (!_active || !hal_cot::is_active()) {
                Serial.printf("[cot] Not active\n");
            } else {
                char xml[1024];
                int len = hal_cot::build_position_event(xml, sizeof(xml));
                if (len > 0) {
                    bool ok = hal_cot::send_multicast(xml, len);
                    Serial.printf("[cot] Sent SA event (%d bytes): %s\n", len, ok ? "OK" : "FAIL");
                } else {
                    Serial.printf("[cot] Failed to build CoT XML\n");
                }
            }
            return true;
        }
        if (strcmp(cmd, "COT_POS") == 0) {
            // COT_POS <lat> <lon> [hae]
            if (!args || args[0] == '\0') {
                Serial.printf("[cot] Usage: COT_POS <lat> <lon> [hae]\n");
                return true;
            }
            double lat = 0, lon = 0;
            float hae = 0;
            int n = sscanf(args, "%lf %lf %f", &lat, &lon, &hae);
            if (n >= 2) {
                hal_cot::set_position(lat, lon, hae);
                Serial.printf("[cot] Position set: %.6f, %.6f, %.1fm\n", lat, lon, hae);
            } else {
                Serial.printf("[cot] Usage: COT_POS <lat> <lon> [hae]\n");
            }
            return true;
        }
        if (strcmp(cmd, "COT_STATUS") == 0) {
            Serial.printf("[cot] Active: %s\n", hal_cot::is_active() ? "yes" : "no");
            return true;
        }
#endif
        return false;
    }

private:
    bool _active = false;
};
