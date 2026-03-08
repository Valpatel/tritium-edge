/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file backlight.h
 * @brief Backlight PWM helpers for display boards with direct GPIO backlight control.
 *
 * For AMOLED boards (no backlight GPIO), brightness is controlled via the
 * panel's 0x51 register. For the 4.3C-BOX, backlight is via the IO extension chip.
 * This header only handles direct GPIO PWM backlights (3.49, 3.5B-C).
 */

#pragma once

#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKLIGHT_LEDC_TIMER        LEDC_TIMER_2        /* Avoid TIMER_0 (camera XCLK) */
#define BACKLIGHT_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_FREQ_HZ      5000
#define BACKLIGHT_LEDC_DUTY_RES     LEDC_TIMER_8_BIT    /* 0-255 range */

/**
 * @brief Initialize backlight PWM on the given GPIO.
 *
 * @param gpio_num  GPIO number for backlight
 * @param channel   LEDC channel to use (0-7)
 * @param active_high  true if high = on, false if low = on
 * @return ESP_OK on success
 */
static inline esp_err_t backlight_init(int gpio_num, int channel, bool active_high)
{
    if (gpio_num < 0) return ESP_OK;  /* No backlight GPIO */

    ledc_timer_config_t timer_conf = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .duty_resolution = BACKLIGHT_LEDC_DUTY_RES,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .freq_hz = BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_conf);
    if (ret != ESP_OK) return ret;

    ledc_channel_config_t ch_conf = {
        .gpio_num = gpio_num,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel = (ledc_channel_t)channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ret = ledc_channel_config(&ch_conf);
    if (ret != ESP_OK) return ret;

    /* Also set as plain GPIO output as fallback (some boards need this) */
    gpio_set_direction((gpio_num_t)gpio_num, GPIO_MODE_OUTPUT);

    return ESP_OK;
}

/**
 * @brief Set backlight brightness.
 *
 * @param gpio_num      GPIO number for backlight
 * @param channel       LEDC channel used in backlight_init()
 * @param brightness    0-255 (0=off, 255=full)
 * @param active_high   true if high = on
 */
static inline void backlight_set_brightness(int gpio_num, int channel, uint8_t brightness, bool active_high)
{
    if (gpio_num < 0) return;

    uint32_t duty = active_high ? brightness : (255 - brightness);
    ledc_set_duty(BACKLIGHT_LEDC_MODE, (ledc_channel_t)channel, duty);
    ledc_update_duty(BACKLIGHT_LEDC_MODE, (ledc_channel_t)channel);

    /* Fallback: set GPIO high/low for full on/off */
    if (brightness > 0) {
        gpio_set_level((gpio_num_t)gpio_num, active_high ? 1 : 0);
    }
}

#ifdef __cplusplus
}
#endif
