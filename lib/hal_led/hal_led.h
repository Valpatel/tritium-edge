// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// LED Status Indicator HAL
//
// Drives an addressable LED (NeoPixel/WS2812) or simple GPIO LED
// for visual system status feedback:
//   GREEN   = WiFi connected, system idle
//   BLUE    = Scanning (BLE/WiFi)
//   RED     = Error state
//   YELLOW  = OTA update in progress
//   PURPLE  = RF motion detected
//   CYAN    = MQTT connected
//   WHITE   = Boot / initializing
//   OFF     = Deep sleep or disabled
//
// Boards without an addressable LED get a simple on/off GPIO fallback.

#include <cstdint>

namespace hal_led {

// Status codes that map to LED colors
enum class StatusColor : uint8_t {
    OFF      = 0,
    GREEN    = 1,   // Connected / idle
    BLUE     = 2,   // Scanning
    RED      = 3,   // Error
    YELLOW   = 4,   // OTA in progress
    PURPLE   = 5,   // Motion detected
    CYAN     = 6,   // MQTT connected
    WHITE    = 7,   // Boot / init
};

struct LedConfig {
    int8_t  gpio_pin       = -1;    // GPIO for NeoPixel data or simple LED
    bool    is_neopixel    = false; // true = WS2812/NeoPixel, false = simple GPIO
    uint8_t brightness     = 32;    // 0-255 (NeoPixel only)
    bool    enabled        = true;
};

// RGB color for NeoPixel
struct RGBColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

// Map status to RGB color
inline RGBColor status_to_rgb(StatusColor status) {
    switch (status) {
        case StatusColor::GREEN:  return {0, 255, 0};
        case StatusColor::BLUE:   return {0, 0, 255};
        case StatusColor::RED:    return {255, 0, 0};
        case StatusColor::YELLOW: return {255, 200, 0};
        case StatusColor::PURPLE: return {128, 0, 255};
        case StatusColor::CYAN:   return {0, 240, 255};
        case StatusColor::WHITE:  return {255, 255, 255};
        case StatusColor::OFF:
        default:                  return {0, 0, 0};
    }
}

/// Initialize the LED subsystem.
/// Call once at boot after GPIO is available.
/// Returns true if a usable LED was found.
bool init(const LedConfig& config = {});

/// Set the LED to a status color.
/// For NeoPixel: sets the color with configured brightness.
/// For GPIO LED: turns on/off (any non-OFF color = on).
void set_status(StatusColor status);

/// Set a custom RGB color (NeoPixel only).
void set_rgb(uint8_t r, uint8_t g, uint8_t b);

/// Pulse the LED briefly (50ms flash).
/// Useful for indicating events like sightings.
void pulse(StatusColor status);

/// Turn off the LED.
void off();

/// Set brightness (NeoPixel only, 0-255).
void set_brightness(uint8_t brightness);

/// Check if LED subsystem is initialized and available.
bool is_available();

/// Get current status color.
StatusColor get_current_status();

}  // namespace hal_led
