// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "led_service.h"
#include <Arduino.h>

namespace led_service {

static bool _initialized = false;
static hal_led::StatusColor _override_color = hal_led::StatusColor::OFF;
static bool _override_active = false;
static uint32_t _override_expire = 0;
static hal_led::StatusColor _last_auto_color = hal_led::StatusColor::OFF;

hal_led::LedConfig auto_detect() {
    hal_led::LedConfig cfg;
    cfg.enabled = false;

    // Check for board-specific NeoPixel defines
    // Common Waveshare ESP32-S3 boards with addressable LEDs
#if defined(HAS_NEOPIXEL) && HAS_NEOPIXEL
    cfg.gpio_pin = NEOPIXEL_PIN;
    cfg.is_neopixel = true;
    cfg.brightness = 32;
    cfg.enabled = true;
#elif defined(BOARD_LED_PIN)
    // Fallback: simple GPIO LED
    cfg.gpio_pin = BOARD_LED_PIN;
    cfg.is_neopixel = false;
    cfg.enabled = true;
#elif defined(LED_BUILTIN)
    // Arduino standard built-in LED
    cfg.gpio_pin = LED_BUILTIN;
    cfg.is_neopixel = false;
    cfg.enabled = true;
#endif

    return cfg;
}

bool init(hal_led::LedConfig config) {
    // Auto-detect if no GPIO specified
    if (config.gpio_pin < 0) {
        config = auto_detect();
    }

    if (!config.enabled || config.gpio_pin < 0) {
        _initialized = false;
        return false;
    }

    _initialized = hal_led::init(config);
    return _initialized;
}

void update(const SystemStatus& status) {
    if (!_initialized || _override_active) return;

    // Priority-based color selection (highest priority first)
    hal_led::StatusColor color;

    if (status.error_state) {
        color = hal_led::StatusColor::RED;
    } else if (status.ota_in_progress) {
        color = hal_led::StatusColor::YELLOW;
    } else if (status.motion_detected) {
        color = hal_led::StatusColor::PURPLE;
    } else if (status.ble_scanning || status.wifi_scanning) {
        color = hal_led::StatusColor::BLUE;
    } else if (status.mqtt_connected) {
        color = hal_led::StatusColor::CYAN;
    } else if (status.wifi_connected) {
        color = hal_led::StatusColor::GREEN;
    } else {
        color = hal_led::StatusColor::OFF;
    }

    // Only update if color changed (avoid unnecessary SPI traffic)
    if (color != _last_auto_color) {
        hal_led::set_status(color);
        _last_auto_color = color;
    }
}

void set_override(hal_led::StatusColor color, uint32_t duration_ms) {
    if (!_initialized) return;
    _override_active = true;
    _override_color = color;
    _override_expire = duration_ms > 0 ? millis() + duration_ms : 0;
    hal_led::set_status(color);
}

void clear_override() {
    _override_active = false;
    // Restore last automatic color
    if (_initialized) {
        hal_led::set_status(_last_auto_color);
    }
}

void tick() {
    if (!_initialized) return;

    // Check override expiry
    if (_override_active && _override_expire > 0 && millis() >= _override_expire) {
        clear_override();
    }
}

}  // namespace led_service
