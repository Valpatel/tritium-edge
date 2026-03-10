// Tritium-OS Power Management Service
// ServiceInterface wrapper for PowerHAL — manages power profiles, screen
// timeouts, auto-sleep, and battery-aware profile switching.
//
// Priority 25: after settings (5) and WiFi (10), before display-dependent services.
//
// Serial commands:
//   POWER                  — show current power state as JSON
//   POWER_PROFILE <0-3>    — set profile (0=performance, 1=balanced, 2=saver, 3=auto)
//   POWER_WAKE             — force wake from any dim/off/sleep state
//
// Settings (domain: system):
//   pwr_profile  (int)  — active profile or 3=auto (default: 3)
//   pwr_dim_s    (int)  — seconds until screen dim (default: 60)
//   pwr_off_s    (int)  — seconds until screen off (default: 120)
//   pwr_sleep_s  (int)  — seconds until light sleep (default: 300)
//   pwr_wake_touch (bool) — wake on touch (default: true)
//   pwr_wake_mesh  (bool) — wake on mesh message (default: true)
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "service.h"

#if defined(ENABLE_POWER_MGMT) && __has_include("hal_power.h")
#include "hal_power.h"
#include "hal_sleep.h"
#include "os_settings.h"
#include "os_events.h"
#endif

// Power profiles — like Linux power governors
enum class PowerProfile : uint8_t {
    PERFORMANCE  = 0,   // Wall power: max brightness, no sleep
    BALANCED     = 1,   // Battery: medium brightness, moderate timeouts
    POWER_SAVER  = 2,   // Low battery: low brightness, aggressive timeouts
    AUTO         = 3    // Automatic selection based on power source + level
};

// Screen state machine
enum class ScreenState : uint8_t {
    ON,         // Full brightness for current profile
    DIMMED,     // Brightness reduced by 50%
    OFF         // Brightness 0, display sleeping
};

class PowerService : public ServiceInterface {
public:
    static PowerService& instance();

    const char* name() const override { return "power"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD | SVC_WEB_API; }
    int initPriority() const override { return 25; }

    bool init() override;
    void tick() override;
    bool handleCommand(const char* cmd, const char* args) override;
    int toJson(char* buf, size_t size) override;

    // Activity tracking — call from touch, serial, mesh handlers
    void resetActivity();

    // Force wake from dim/off/sleep
    void wake();

    // Get current state
    PowerProfile getProfile() const;
    ScreenState getScreenState() const;
    uint32_t getIdleSeconds() const;

    // Battery info for status bar
    int getBatteryPercent() const;
    bool isBatteryCharging() const;
    bool hasBattery() const;

private:
    PowerService() = default;
    ~PowerService() = default;
    PowerService(const PowerService&) = delete;
    PowerService& operator=(const PowerService&) = delete;

#if defined(ENABLE_POWER_MGMT) && __has_include("hal_power.h")
    // Profile management
    void applyProfile(PowerProfile profile);
    PowerProfile resolveAutoProfile(const PowerInfo& info);
    void loadSettings();

    // Screen state machine
    void updateScreenState(uint32_t idle_s);
    void setScreenBrightness(uint8_t brightness);

    // Event handlers
    static void onTouchEvent(const TritiumEvent& event, void* user_data);
    static void onMeshEvent(const TritiumEvent& event, void* user_data);
    static void onPowerEvent(const TritiumEvent& event, void* user_data);
    static void onSettingsChanged(const char* domain, const char* key, void* user_data);

    // Hardware
    PowerHAL _power;
    SleepHAL _sleep;

    // State
    bool _active = false;
    PowerProfile _activeProfile = PowerProfile::AUTO;
    PowerProfile _resolvedProfile = PowerProfile::BALANCED;
    bool _userOverride = false;        // True if user manually set a profile
    ScreenState _screenState = ScreenState::ON;
    uint32_t _lastActivityMs = 0;
    uint32_t _lastPollMs = 0;
    PowerInfo _lastInfo = {};

    // Profile parameters (loaded from settings or profile defaults)
    uint8_t _targetBrightness = 255;   // Full brightness for current profile
    uint32_t _dimTimeoutS = 60;
    uint32_t _offTimeoutS = 120;
    uint32_t _sleepTimeoutS = 300;

    // Settings
    bool _wakeOnTouch = true;
    bool _wakeOnMesh = true;

    // Event subscriber IDs
    int _touchSubId = -1;
    int _meshSubId = -1;
    int _powerSubId = -1;
    int _settingsObsId = -1;

    // Polling interval
    static constexpr uint32_t POLL_INTERVAL_MS = 1000;

    // Emergency threshold
    static constexpr int EMERGENCY_BATTERY_PCT = 5;
    static constexpr int LOW_BATTERY_PCT = 20;
#endif
};
