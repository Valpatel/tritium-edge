#pragma once
// Sighting Logger service adapter — wraps hal_sighting_logger as a ServiceInterface.
// Priority 61 (after sighting buffer at 60, after scanners at 50).

#include "service.h"

#if defined(ENABLE_SIGHTING_LOGGER) && __has_include("hal_sighting_logger.h")
#include "hal_sighting_logger.h"
#endif

class SightingLoggerService : public ServiceInterface {
public:
    const char* name() const override { return "sighting_logger"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD; }
    int initPriority() const override { return 61; }

    bool init() override {
#if defined(ENABLE_SIGHTING_LOGGER)
        hal_sighting_logger::LoggerConfig cfg;
        cfg.db_path = "/sdcard/sightings.db";
        cfg.flush_interval_ms = 5000;
        cfg.max_batch = 64;
        if (hal_sighting_logger::init(cfg)) {
            Serial.printf("[tritium] Sighting Logger: active (SQLite)\n");
            return true;
        }
        Serial.printf("[tritium] Sighting Logger: failed to init\n");
        return false;
#else
        return false;
#endif
    }

    void tick() override {
#if defined(ENABLE_SIGHTING_LOGGER)
        hal_sighting_logger::tick();
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(ENABLE_SIGHTING_LOGGER)
        if (strcmp(cmd, "LOGGER_STATS") == 0) {
            auto s = hal_sighting_logger::get_stats();
            Serial.printf("[logger] BLE=%lu WiFi=%lu total=%lu db=%luB open=%s active=%s\n",
                (unsigned long)s.ble_logged, (unsigned long)s.wifi_logged,
                (unsigned long)s.total_rows, (unsigned long)s.db_size_bytes,
                s.db_open ? "yes" : "no",
                hal_sighting_logger::is_active() ? "yes" : "no");
            return true;
        }
        if (strcmp(cmd, "LOGGER_FLUSH") == 0) {
            int n = hal_sighting_logger::flush();
            Serial.printf("[logger] Flushed %d entries\n", n);
            return true;
        }
        if (strcmp(cmd, "LOGGER_ENABLE") == 0) {
            hal_sighting_logger::enable();
            Serial.printf("[logger] Enabled\n");
            return true;
        }
        if (strcmp(cmd, "LOGGER_DISABLE") == 0) {
            hal_sighting_logger::disable();
            Serial.printf("[logger] Disabled\n");
            return true;
        }
#endif
        return false;
    }
};
