// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
//
// Power-Saving Mode Manager
// Automatically reduces scan intervals, display brightness, and disables
// non-essential HALs when battery drops below 20%.
// Resumes normal operation when charging is detected.
//
// Integrates with:
//   - PowerService (battery level, charge detection)
//   - BLE Scanner (scan interval adjustment)
//   - WiFi Scanner (scan interval adjustment)
//   - Heartbeat (heartbeat interval adjustment)
//   - Display (brightness reduction)
//
// Usage:
//   PowerSaver::instance().init();
//   // Called automatically from PowerService::tick()

#pragma once
#include <cstdint>

// Power-saver thresholds
static constexpr int POWER_SAVER_ENTER_PCT    = 20;   // Enter power-save below 20%
static constexpr int POWER_SAVER_EXIT_PCT     = 25;   // Exit power-save above 25% (hysteresis)
static constexpr int POWER_SAVER_CRITICAL_PCT = 10;   // Even more aggressive below 10%

// Normal scan intervals (milliseconds)
static constexpr uint32_t NORMAL_BLE_SCAN_INTERVAL_MS      = 10000;   // 10s
static constexpr uint32_t NORMAL_WIFI_SCAN_INTERVAL_MS     = 30000;   // 30s
static constexpr uint32_t NORMAL_HEARTBEAT_INTERVAL_MS     = 30000;   // 30s

// Power-save scan intervals (milliseconds)
static constexpr uint32_t SAVER_BLE_SCAN_INTERVAL_MS       = 30000;   // 30s
static constexpr uint32_t SAVER_WIFI_SCAN_INTERVAL_MS      = 120000;  // 120s
static constexpr uint32_t SAVER_HEARTBEAT_INTERVAL_MS      = 120000;  // 120s

// Critical power-save intervals (below 10%)
static constexpr uint32_t CRITICAL_BLE_SCAN_INTERVAL_MS    = 60000;   // 60s
static constexpr uint32_t CRITICAL_WIFI_SCAN_INTERVAL_MS   = 300000;  // 5min
static constexpr uint32_t CRITICAL_HEARTBEAT_INTERVAL_MS   = 300000;  // 5min

// Display brightness
static constexpr uint8_t NORMAL_BRIGHTNESS   = 180;   // Normal brightness
static constexpr uint8_t SAVER_BRIGHTNESS    = 25;    // 10% brightness in power-save
static constexpr uint8_t CRITICAL_BRIGHTNESS = 10;    // Minimal in critical

enum class PowerSaverState : uint8_t {
    NORMAL,      // Full power operation
    POWER_SAVE,  // Reduced intervals, dim display
    CRITICAL,    // Minimal operation, very dim
    CHARGING     // Charging detected, resume normal
};

class PowerSaver {
public:
    static PowerSaver& instance();

    // Initialize power saver (call after PowerService init)
    bool init();

    // Update power saver state based on battery level and charging status.
    // Call from PowerService::tick() or main loop.
    // Returns true if state changed.
    bool update(int battery_pct, bool is_charging, bool is_usb_powered);

    // Get current power saver state
    PowerSaverState getState() const { return _state; }

    // Check if power-saving is active
    bool isActive() const { return _state == PowerSaverState::POWER_SAVE || _state == PowerSaverState::CRITICAL; }

    // Get current adjusted intervals
    uint32_t getBleScanInterval() const { return _ble_interval; }
    uint32_t getWifiScanInterval() const { return _wifi_interval; }
    uint32_t getHeartbeatInterval() const { return _heartbeat_interval; }
    uint8_t getTargetBrightness() const { return _brightness; }

    // Get JSON status for serial/API output
    int toJson(char* buf, size_t size) const;

private:
    PowerSaver() = default;
    PowerSaver(const PowerSaver&) = delete;
    PowerSaver& operator=(const PowerSaver&) = delete;

    void enterPowerSave();
    void enterCritical();
    void exitPowerSave();
    void applyIntervals();

    PowerSaverState _state = PowerSaverState::NORMAL;
    bool _initialized = false;

    // Current adjusted intervals
    uint32_t _ble_interval = NORMAL_BLE_SCAN_INTERVAL_MS;
    uint32_t _wifi_interval = NORMAL_WIFI_SCAN_INTERVAL_MS;
    uint32_t _heartbeat_interval = NORMAL_HEARTBEAT_INTERVAL_MS;
    uint8_t _brightness = NORMAL_BRIGHTNESS;

    // Tracking
    int _last_battery_pct = 100;
    bool _last_charging = false;
};
