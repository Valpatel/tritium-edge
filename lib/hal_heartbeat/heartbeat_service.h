#pragma once
// Heartbeat service adapter — wraps hal_heartbeat namespace as a ServiceInterface.
// Priority 50.

#include "service.h"

#if defined(ENABLE_HEARTBEAT) && __has_include("hal_heartbeat.h")
#include "hal_heartbeat.h"
#endif

class HeartbeatService : public ServiceInterface {
public:
    const char* name() const override { return "heartbeat"; }
    uint8_t capabilities() const override { return SVC_TICK; }
    int initPriority() const override { return 50; }

    bool init() override {
#if defined(ENABLE_HEARTBEAT)
        hal_heartbeat::HeartbeatConfig hb_cfg;
#if defined(DEFAULT_SERVER_URL)
        hb_cfg.server_url = DEFAULT_SERVER_URL;
#endif
#if defined(DEFAULT_DEVICE_ID)
        hb_cfg.device_id = DEFAULT_DEVICE_ID;
#endif
        hb_cfg.interval_ms = 30000;  // 30s heartbeats
        if (hal_heartbeat::init(hb_cfg)) {
            Serial.printf("[tritium] Heartbeat: active\n");
            _active = true;
            return true;
        } else {
            Serial.printf("[tritium] Heartbeat: not configured\n");
            return false;
        }
#else
        return false;
#endif
    }

    void tick() override {
#if defined(ENABLE_HEARTBEAT)
        if (_active) hal_heartbeat::tick();
#endif
    }

private:
    bool _active = false;
};
