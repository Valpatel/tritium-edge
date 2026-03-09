#pragma once
// Diagnostics service adapter — wraps hal_diag namespace as a ServiceInterface.
// Priority 150 (after hardware HALs).
// Includes power/camera/touch/NTP provider wiring.

#include "service.h"

// Only pull in Wire when touch diagnostics are actually needed.
// Wire (old I2C driver) conflicts with ESP-IDF 5.x i2c_master on builds
// that don't gate it out via lib_ignore (e.g. fleet builds without touch).
#if defined(ENABLE_DIAG) && __has_include("hal_touch.h") && defined(TOUCH_SDA)
#include <Wire.h>
#endif

#if defined(ENABLE_DIAG) && __has_include("hal_diag.h")
#include "hal_diag.h"
#endif

#if defined(ENABLE_DIAG)
#if HAS_PMIC && __has_include("hal_power.h")
#include "hal_power.h"
#endif
#if HAS_CAMERA && __has_include("hal_camera.h")
#include "hal_camera.h"
#endif
#if __has_include("hal_touch.h")
#include "hal_touch.h"
#endif
#if __has_include("hal_ntp.h")
#include "hal_ntp.h"
#endif
#endif

class DiagService : public ServiceInterface {
public:
    const char* name() const override { return "diag"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD | SVC_WEB_API; }
    int initPriority() const override { return 150; }

    bool init() override {
#if defined(ENABLE_DIAG)
        hal_diag::DiagConfig diag_cfg;
        diag_cfg.health_interval_ms = 30000;
        diag_cfg.log_to_serial = true;
        diag_cfg.anomaly_detection = true;
        if (hal_diag::init(diag_cfg)) {
            Serial.printf("[tritium] Diagnostics: active\n");
            hal_diag::log(hal_diag::Severity::INFO, "system", "Tritium-Edge boot complete");

            // Wire power HAL into diagnostics on boards with PMIC
#if HAS_PMIC && __has_include("hal_power.h")
            {
                static PowerHAL _diag_power;
                _diag_power.initLgfx(0, 0x34);
                hal_diag::set_power_provider([](hal_diag::PowerInfo& out) -> bool {
                    auto info = _diag_power.getInfo();
                    out.battery_voltage = info.voltage;
                    out.battery_percent = (info.percentage >= 0) ? (float)info.percentage : 0.0f;
                    out.charge_current_ma = 0.0f;  // Not exposed by PowerHAL yet
                    out.power_source = info.is_usb_powered ? 1 : (info.has_battery ? 2 : 0);
                    out.pmic_temp_c = 0.0f;  // TODO: add PMIC temp read to PowerHAL
                    return true;
                });
                Serial.printf("[tritium] Diagnostics: power provider wired\n");
            }
#endif

            // Wire camera diagnostics on boards with camera hardware
#if HAS_CAMERA && __has_include("hal_camera.h")
            {
                static CameraHAL _diag_camera;
                if (_diag_camera.init(CamResolution::QVGA_320x240, CamPixelFormat::RGB565)) {
                    hal_diag::set_camera_provider([](hal_diag::CameraInfo& out) -> bool {
                        out.available    = _diag_camera.available();
                        out.frame_count  = _diag_camera.getFrameCount();
                        out.fail_count   = _diag_camera.getFailCount();
                        out.last_capture_us = _diag_camera.getLastCaptureUs();
                        out.max_capture_us  = _diag_camera.getMaxCaptureUs();
                        out.avg_fps      = _diag_camera.getAvgFps();
                        return true;
                    });
                    Serial.printf("[tritium] Diagnostics: camera provider wired\n");
                }
            }
#endif

            // Touch diagnostics: use the global TouchHAL from main.cpp
            // (initialized after services, so we wire a lazy provider)
#if __has_include("hal_touch.h") && defined(TOUCH_SDA)
            {
                extern TouchHAL touch;
                hal_diag::set_touch_provider([](bool& available) -> bool {
                    available = touch.available();
                    return true;
                });
                Serial.printf("[tritium] Diagnostics: touch provider wired (lazy)\n");
            }
#endif

            // Wire NTP diagnostics when WiFi is available
#if __has_include("hal_ntp.h") && defined(ENABLE_WIFI)
            {
                static NtpHAL _diag_ntp;
                _diag_ntp.init();
                hal_diag::set_ntp_provider([](hal_diag::NtpInfo& out) -> bool {
                    out.synced = _diag_ntp.isSynced();
                    uint32_t last = _diag_ntp.getLastSyncEpoch();
                    if (last > 0) {
                        uint32_t now = _diag_ntp.getEpoch();
                        out.last_sync_age_s = (now > last) ? (now - last) : 0;
                    } else {
                        out.last_sync_age_s = 0;
                    }
                    return true;
                });
                Serial.printf("[tritium] Diagnostics: NTP provider wired\n");
            }
#endif
            _active = true;
            return true;
        } else {
            Serial.printf("[tritium] Diagnostics: init failed\n");
            return false;
        }
#else
        return false;
#endif
    }

    void tick() override {
#if defined(ENABLE_DIAG)
        if (_active) hal_diag::tick();
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(ENABLE_DIAG)
        static char _cmd_json[2048];  // shared across diag serial commands
        if (strcmp(cmd, "DIAG") == 0) {
            int len = hal_diag::full_report_json(_cmd_json, sizeof(_cmd_json));
            if (len > 0) Serial.println(_cmd_json);
            return true;
        }
        if (strcmp(cmd, "HEALTH") == 0) {
            int len = hal_diag::health_to_json(_cmd_json, sizeof(_cmd_json));
            if (len > 0) Serial.println(_cmd_json);
            return true;
        }
        if (strcmp(cmd, "ANOMALIES") == 0) {
            int len = hal_diag::anomalies_to_json(_cmd_json, sizeof(_cmd_json));
            if (len > 0) Serial.println(_cmd_json);
            return true;
        }
#endif
        return false;
    }

    int toJson(char* buf, size_t size) override {
#if defined(ENABLE_DIAG)
        return hal_diag::full_report_json(buf, size);
#else
        return 0;
#endif
    }

private:
    bool _active = false;
};
