// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// LED Service — high-level status indicator that integrates with system state.
//
// Automatically sets LED color based on system status:
//   Boot      -> WHITE
//   WiFi up   -> GREEN
//   Scanning  -> BLUE
//   MQTT up   -> CYAN
//   OTA       -> YELLOW
//   Motion    -> PURPLE
//   Error     -> RED
//
// Call led_service::tick() from the main loop to auto-update.
// Or set explicit overrides with set_override().

#include "hal_led.h"

namespace led_service {

struct SystemStatus {
    bool wifi_connected  = false;
    bool mqtt_connected  = false;
    bool ble_scanning    = false;
    bool wifi_scanning   = false;
    bool ota_in_progress = false;
    bool motion_detected = false;
    bool error_state     = false;
};

/// Initialize the LED service with a hardware config.
/// Detects board type and configures the LED GPIO automatically
/// if gpio_pin is -1 (auto-detect).
bool init(hal_led::LedConfig config = {});

/// Auto-detect if the current board has an addressable LED.
/// Checks HAS_NEOPIXEL define and known board pin assignments.
/// Returns a populated LedConfig or one with gpio_pin=-1 if no LED found.
hal_led::LedConfig auto_detect();

/// Update the LED based on current system status.
/// Call from main loop. Priority order (highest first):
///   error > ota > motion > scanning > mqtt > wifi > idle
void update(const SystemStatus& status);

/// Set an explicit override color. Clears after duration_ms (0 = permanent).
void set_override(hal_led::StatusColor color, uint32_t duration_ms = 0);

/// Clear any override, return to automatic status-based color.
void clear_override();

/// Tick function for main loop (handles override expiry).
void tick();

}  // namespace led_service
