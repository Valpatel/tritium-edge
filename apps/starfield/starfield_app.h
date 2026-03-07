#pragma once
#include "app.h"
#include "StarField.h"
#include "hal_power.h"
#include "hal_imu.h"
#include "battery_monitor.h"
#include "battery_widget.h"

class StarfieldApp : public App {
public:
    const char* name() override { return "Starfield"; }
    void setup(LGFX& display) override;
    void loop(LGFX& display) override;

private:
    static uint16_t tintedColor(float brightness, StarTint tint);
    void drawSplash(LGFX& display);
    float currentSpeed() const;

    LGFX_Sprite* _canvas = nullptr;
    LGFX_Sprite* _overlay = nullptr;
    uint16_t* _line_buf = nullptr;
    StarField* _starfield = nullptr;
    int _render_scale = 1;  // 1=native, 2=half-res scaled up

    // Profiling (serial only, no on-screen FPS)
    uint32_t _frame_count = 0;
    uint32_t _fps_timer = 0;
    float _fps = 0.0f;
    uint32_t _render_ms = 0;
    uint32_t _push_ms = 0;

    // Splash screen
    uint32_t _start_time = 0;
    bool _splash_done = false;

    // Warp
    uint32_t _warp_timer = 0;
    bool _warping = false;

    // Battery + IMU
    PowerHAL _power;
    IMUHAL _imu;
    BatteryMonitor _battery;
    BatteryWidget _battWidget;
    bool _power_ok = false;
    bool _imu_ok = false;
    uint32_t _bat_timer = 0;

    // UI visibility — show on touch/motion, hide after timeout
    bool _ui_visible = false;
    uint32_t _ui_show_time = 0;
    static constexpr uint32_t UI_TIMEOUT_MS = 4000;
};
