/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * tritium_compat.h — ESP-IDF native replacements for Arduino primitives.
 *
 * Drop-in replacement for <Arduino.h>. Provides millis(), delay(), Serial,
 * GPIO, and other essentials using pure ESP-IDF APIs. No Arduino framework.
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

// ESP-IDF core headers
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Time ─────────────────────────────────────────────────────────────────────

static inline uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline uint32_t micros(void) {
    return (uint32_t)esp_timer_get_time();
}

static inline void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static inline void delayMicroseconds(uint32_t us) {
    esp_rom_delay_us(us);
}

// ── GPIO ─────────────────────────────────────────────────────────────────────

// Arduino compatibility defines
#ifndef INPUT
#define INPUT    GPIO_MODE_INPUT
#define OUTPUT   GPIO_MODE_OUTPUT
#define HIGH     1
#define LOW      0
#endif

#ifndef PROGMEM
#define PROGMEM  // No-op on ESP32 (flash is memory-mapped)
#endif

#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif

static inline void pinMode(int pin, int mode) {
    if (pin < 0) return;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << pin);
    cfg.mode = (gpio_mode_t)mode;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
}

static inline void digitalWrite(int pin, int level) {
    if (pin < 0) return;
    gpio_set_level((gpio_num_t)pin, level);
}

static inline int digitalRead(int pin) {
    if (pin < 0) return 0;
    return gpio_get_level((gpio_num_t)pin);
}

// ── Serial ───────────────────────────────────────────────────────────────────
//
// Provides a Serial-like interface using USB CDC (stdout) on ESP32-S3.
// printf() goes to USB CDC when CONFIG_ESP_CONSOLE_USB_CDC=y or
// CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y in sdkconfig.

#ifdef __cplusplus
}  // extern "C"

#include "driver/usb_serial_jtag.h"

class SerialPort {
public:
    void begin(unsigned long baud) {
        (void)baud;
        // USB CDC is already initialized by ESP-IDF
        _initialized = true;
    }

    void end() { _initialized = false; }

    int available() {
        poll();
        return _rx_write - _rx_read;
    }

    int read() {
        poll();
        if (_rx_read < _rx_write) {
            return _rx_buf[_rx_read++];
        }
        if (_rx_read == _rx_write) {
            _rx_read = _rx_write = 0;
        }
        return -1;
    }

    size_t write(uint8_t c) {
        return fputc(c, stdout) != EOF ? 1 : 0;
    }

    size_t write(const uint8_t* buf, size_t len) {
        return fwrite(buf, 1, len, stdout);
    }

    int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list args;
        va_start(args, fmt);
        int ret = vprintf(fmt, args);
        va_end(args);
        return ret;
    }

    void flush() { fflush(stdout); }

    // Poll for available data (call from main loop)
    void poll() {
        uint8_t tmp[64];
        int n = usb_serial_jtag_read_bytes(tmp, sizeof(tmp), 0);
        if (n > 0) {
            for (int i = 0; i < n && _rx_write < (int)sizeof(_rx_buf); i++) {
                _rx_buf[_rx_write++] = tmp[i];
            }
            _rx_available = _rx_write - _rx_read;
        }
    }

    int readFromBuffer() {
        if (_rx_read < _rx_write) {
            return _rx_buf[_rx_read++];
        }
        if (_rx_read == _rx_write) {
            _rx_read = _rx_write = 0;
            _rx_available = 0;
        }
        return -1;
    }

    int availableFromBuffer() const {
        return _rx_write - _rx_read;
    }

private:
    bool _initialized = false;
    int _rx_available = 0;
    uint8_t _rx_buf[512] = {};
    int _rx_read = 0;
    int _rx_write = 0;
};

extern SerialPort Serial;

#endif  // __cplusplus
