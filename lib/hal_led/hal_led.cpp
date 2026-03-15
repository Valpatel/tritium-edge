// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_led.h"
#include <Arduino.h>

// NeoPixel bit-bang timing for WS2812 on ESP32-S3.
// Uses RMT peripheral for precise timing.

namespace hal_led {

static LedConfig _config;
static bool _initialized = false;
static StatusColor _current = StatusColor::OFF;

#if defined(ESP32)
#include "driver/rmt_tx.h"

// RMT-based NeoPixel driver state
static rmt_channel_handle_t _rmt_channel = nullptr;
static rmt_encoder_handle_t _rmt_encoder = nullptr;
static bool _rmt_ready = false;

// WS2812 timing (in RMT ticks at 10MHz resolution)
static const uint16_t WS2812_T0H = 3;   // 300ns
static const uint16_t WS2812_T0L = 9;   // 900ns
static const uint16_t WS2812_T1H = 9;   // 900ns
static const uint16_t WS2812_T1L = 3;   // 300ns

static bool _init_rmt_neopixel(int gpio) {
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = (gpio_num_t)gpio;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = 10000000;  // 10MHz -> 100ns per tick
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 1;

    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &_rmt_channel);
    if (err != ESP_OK) {
        return false;
    }

    // Use bytes encoder for WS2812 protocol
    rmt_bytes_encoder_config_t bytes_cfg = {};
    bytes_cfg.bit0.level0 = 1;
    bytes_cfg.bit0.duration0 = WS2812_T0H;
    bytes_cfg.bit0.level1 = 0;
    bytes_cfg.bit0.duration1 = WS2812_T0L;
    bytes_cfg.bit1.level0 = 1;
    bytes_cfg.bit1.duration0 = WS2812_T1H;
    bytes_cfg.bit1.level1 = 0;
    bytes_cfg.bit1.duration1 = WS2812_T1L;
    bytes_cfg.flags.msb_first = 1;

    err = rmt_new_bytes_encoder(&bytes_cfg, &_rmt_encoder);
    if (err != ESP_OK) {
        rmt_del_channel(_rmt_channel);
        _rmt_channel = nullptr;
        return false;
    }

    err = rmt_enable(_rmt_channel);
    if (err != ESP_OK) {
        rmt_del_encoder(_rmt_encoder);
        rmt_del_channel(_rmt_channel);
        _rmt_encoder = nullptr;
        _rmt_channel = nullptr;
        return false;
    }

    _rmt_ready = true;
    return true;
}

static void _send_neopixel_color(uint8_t r, uint8_t g, uint8_t b) {
    if (!_rmt_ready) return;

    // Apply brightness
    uint8_t bright = _config.brightness;
    r = (uint16_t)r * bright / 255;
    g = (uint16_t)g * bright / 255;
    b = (uint16_t)b * bright / 255;

    // WS2812 expects GRB order
    uint8_t data[3] = {g, r, b};

    rmt_transmit_config_t tx_cfg = {};
    tx_cfg.loop_count = 0;

    rmt_transmit(_rmt_channel, _rmt_encoder, data, sizeof(data), &tx_cfg);
    rmt_tx_wait_all_done(_rmt_channel, 100);
}

#else
// Non-ESP32: stub implementation
static bool _init_rmt_neopixel(int gpio) { return false; }
static void _send_neopixel_color(uint8_t r, uint8_t g, uint8_t b) {}
#endif

bool init(const LedConfig& config) {
    _config = config;

    if (!_config.enabled || _config.gpio_pin < 0) {
        _initialized = false;
        return false;
    }

    if (_config.is_neopixel) {
        _initialized = _init_rmt_neopixel(_config.gpio_pin);
        if (_initialized) {
            // Boot flash: brief white
            set_status(StatusColor::WHITE);
        }
    } else {
        // Simple GPIO LED
        pinMode(_config.gpio_pin, OUTPUT);
        digitalWrite(_config.gpio_pin, LOW);
        _initialized = true;
    }

    return _initialized;
}

void set_status(StatusColor status) {
    if (!_initialized) return;
    _current = status;

    if (_config.is_neopixel) {
        RGBColor c = status_to_rgb(status);
        _send_neopixel_color(c.r, c.g, c.b);
    } else {
        // Simple LED: on for any status, off for OFF
        digitalWrite(_config.gpio_pin, status != StatusColor::OFF ? HIGH : LOW);
    }
}

void set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (!_initialized || !_config.is_neopixel) return;
    _send_neopixel_color(r, g, b);
}

void pulse(StatusColor status) {
    if (!_initialized) return;
    StatusColor prev = _current;
    set_status(status);
    delay(50);
    set_status(prev);
}

void off() {
    set_status(StatusColor::OFF);
}

void set_brightness(uint8_t brightness) {
    _config.brightness = brightness;
    // Re-apply current color with new brightness
    if (_initialized && _config.is_neopixel) {
        set_status(_current);
    }
}

bool is_available() {
    return _initialized;
}

StatusColor get_current_status() {
    return _current;
}

// ---------------------------------------------------------------------------
// Blink pattern engine
// ---------------------------------------------------------------------------

static BlinkPattern _active_pattern = BlinkPattern::SOLID;
static StatusColor  _pattern_color  = StatusColor::OFF;
static bool         _pattern_running = false;
static unsigned long _pattern_start_ms = 0;

// Timer-driven pattern update — call from loop() or a Ticker
static void _update_pattern() {
    if (!_pattern_running || !_initialized) return;

    unsigned long elapsed = millis() - _pattern_start_ms;
    RGBColor base = status_to_rgb(_pattern_color);
    float brightness_factor = 1.0f;

    switch (_active_pattern) {
        case BlinkPattern::SOLID:
            // Always on — no modulation
            brightness_factor = 1.0f;
            break;

        case BlinkPattern::SLOW_BLINK: {
            // 1 Hz: 500ms on / 500ms off
            uint32_t phase = elapsed % 1000;
            brightness_factor = (phase < 500) ? 1.0f : 0.0f;
            break;
        }

        case BlinkPattern::FAST_BLINK: {
            // 4 Hz: 125ms on / 125ms off
            uint32_t phase = elapsed % 250;
            brightness_factor = (phase < 125) ? 1.0f : 0.0f;
            break;
        }

        case BlinkPattern::PULSE: {
            // Smooth sine-wave pulse, ~2s cycle
            float t = (float)(elapsed % 2000) / 2000.0f;
            // sin goes 0->1->0 over half period
            brightness_factor = (sinf(t * 2.0f * 3.14159f) + 1.0f) / 2.0f;
            break;
        }

        case BlinkPattern::DOUBLE_FLASH: {
            // Two 80ms flashes separated by 80ms gap, then 760ms off
            // Total cycle: 1000ms
            uint32_t phase = elapsed % 1000;
            if (phase < 80 || (phase >= 160 && phase < 240)) {
                brightness_factor = 1.0f;
            } else {
                brightness_factor = 0.0f;
            }
            break;
        }

        case BlinkPattern::BREATHE: {
            // Slow sine-wave, ~4s cycle, never fully off (min 5%)
            float t = (float)(elapsed % 4000) / 4000.0f;
            brightness_factor = (sinf(t * 2.0f * 3.14159f) + 1.0f) / 2.0f;
            brightness_factor = 0.05f + brightness_factor * 0.95f;
            break;
        }
    }

    if (_config.is_neopixel) {
        uint8_t r = (uint8_t)(base.r * brightness_factor);
        uint8_t g = (uint8_t)(base.g * brightness_factor);
        uint8_t b = (uint8_t)(base.b * brightness_factor);
        _send_neopixel_color(r, g, b);
    } else {
        // Simple GPIO: on if brightness > 50%
        digitalWrite(_config.gpio_pin, brightness_factor > 0.5f ? HIGH : LOW);
    }
}

void start_pattern(StatusColor color, BlinkPattern pattern) {
    if (!_initialized) return;

    _pattern_color = color;
    _active_pattern = pattern;
    _pattern_start_ms = millis();
    _pattern_running = true;

    // Immediate first update
    _update_pattern();
}

void stop_pattern() {
    _pattern_running = false;
    // Leave LED in whatever state it's in
}

void set_operational_status(StatusColor status) {
    switch (status) {
        case StatusColor::GREEN:
            start_pattern(StatusColor::GREEN, BlinkPattern::SOLID);
            break;
        case StatusColor::BLUE:
            start_pattern(StatusColor::BLUE, BlinkPattern::SLOW_BLINK);
            break;
        case StatusColor::YELLOW:
            start_pattern(StatusColor::YELLOW, BlinkPattern::FAST_BLINK);
            break;
        case StatusColor::RED:
            start_pattern(StatusColor::RED, BlinkPattern::SOLID);
            break;
        case StatusColor::PURPLE:
            start_pattern(StatusColor::PURPLE, BlinkPattern::PULSE);
            break;
        case StatusColor::CYAN:
            start_pattern(StatusColor::CYAN, BlinkPattern::DOUBLE_FLASH);
            break;
        case StatusColor::WHITE:
            start_pattern(StatusColor::WHITE, BlinkPattern::BREATHE);
            break;
        case StatusColor::OFF:
        default:
            stop_pattern();
            off();
            break;
    }
    _current = status;
}

bool pattern_active() {
    return _pattern_running;
}

}  // namespace hal_led
