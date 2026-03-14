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

}  // namespace hal_led
