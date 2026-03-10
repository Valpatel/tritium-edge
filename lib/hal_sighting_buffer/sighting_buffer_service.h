#pragma once
// Sighting Buffer service adapter — wraps hal_sighting_buffer namespace as a ServiceInterface.
// Priority 60 (after scanners).

#include "service.h"

#if defined(ENABLE_SIGHTING_BUFFER) && __has_include("hal_sighting_buffer.h")
#include "hal_sighting_buffer.h"
#endif

class SightingBufferService : public ServiceInterface {
public:
    const char* name() const override { return "sighting_buffer"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD; }
    int initPriority() const override { return 60; }

    bool init() override {
#if defined(ENABLE_SIGHTING_BUFFER) && defined(HAS_SDCARD) && HAS_SDCARD
        {
            hal_sighting_buffer::BufferConfig buf_cfg;
            buf_cfg.flush_interval_ms = 10000;
            if (hal_sighting_buffer::init(buf_cfg)) {
                Serial.printf("[tritium] Sighting Buffer: active (SD)\n");
                _active = true;
                return true;
            }
            return false;
        }
#elif defined(ENABLE_SIGHTING_BUFFER)
        {
            hal_sighting_buffer::BufferConfig buf_cfg;
            buf_cfg.flush_interval_ms = 10000;
            if (hal_sighting_buffer::init(buf_cfg)) {
                Serial.printf("[tritium] Sighting Buffer: active (memory-only)\n");
                _active = true;
                return true;
            }
            return false;
        }
#else
        return false;
#endif
    }

    void tick() override {
#if defined(ENABLE_SIGHTING_BUFFER)
        if (_active) hal_sighting_buffer::tick();
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(ENABLE_SIGHTING_BUFFER)
        if (strcmp(cmd, "SIGHTING_STATS") == 0) {
            auto s = hal_sighting_buffer::get_stats();
            Serial.printf("[sighting] BLE=%lu WiFi=%lu flushed=%lu synced=%lu "
                "pending=%lu sd=%s\n",
                (unsigned long)s.ble_sightings_buffered,
                (unsigned long)s.wifi_sightings_buffered,
                (unsigned long)s.total_flushed_to_sd,
                (unsigned long)s.total_synced_to_server,
                (unsigned long)s.pending_on_sd,
                s.sd_available ? "yes" : "no");
            return true;
        }
        if (strcmp(cmd, "SIGHTING_FLUSH") == 0) {
            int n = hal_sighting_buffer::flush_to_sd();
            Serial.printf("[sighting] Flushed %d entries\n", n);
            return true;
        }
        if (strcmp(cmd, "SIGHTING_SYNC") == 0) {
#if defined(DEFAULT_SERVER_URL) && defined(DEFAULT_DEVICE_ID)
            int n = hal_sighting_buffer::sync_to_server(
                DEFAULT_SERVER_URL, DEFAULT_DEVICE_ID);
            Serial.printf("[sighting] Synced %d entries\n", n);
#else
            Serial.printf("[sighting] No server URL/device ID configured\n");
#endif
            return true;
        }
#endif
        return false;
    }

private:
    bool _active = false;
};
