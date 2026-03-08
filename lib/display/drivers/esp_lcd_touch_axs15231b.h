/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esp_lcd_touch_axs15231b.h
 * @brief Touch data structures for AXS15231B integrated touch controller.
 *
 * Copied from Waveshare ESP32-S3-Touch-LCD-3.49 reference code
 * (references/ESP32-S3-Touch-LCD-3.49/Examples/Arduino/09_LVGL_V8_Test/src/touch/esp_lcd_touch.h).
 *
 * This is a standalone touch struct definition that does NOT depend on the
 * Espressif esp_lcd_touch component from the component registry. The AXS15231B
 * touch driver uses a custom I2C handshake protocol and is self-contained.
 */

#pragma once

#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Max touch points and buttons -- hardcoded since we don't use Kconfig */
#ifndef CONFIG_ESP_LCD_TOUCH_MAX_POINTS
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 5
#endif
#ifndef CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS
#define CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Touch controller type
 */
typedef struct esp_lcd_touch_s esp_lcd_touch_t;
typedef esp_lcd_touch_t *esp_lcd_touch_handle_t;

/**
 * @brief Touch controller interrupt callback type
 */
typedef void (*esp_lcd_touch_interrupt_callback_t)(esp_lcd_touch_handle_t tp);

/**
 * @brief Touch Configuration Type
 */
typedef struct {
    uint16_t x_max; /*!< X coordinates max (for mirroring) */
    uint16_t y_max; /*!< Y coordinates max (for mirroring) */

    gpio_num_t rst_gpio_num;    /*!< GPIO number of reset pin */
    gpio_num_t int_gpio_num;    /*!< GPIO number of interrupt pin */

    struct {
        unsigned int reset: 1;    /*!< Level of reset pin in reset */
        unsigned int interrupt: 1;/*!< Active Level of interrupt pin */
    } levels;

    struct {
        unsigned int swap_xy: 1;  /*!< Swap X and Y after read coordinates */
        unsigned int mirror_x: 1; /*!< Mirror X after read coordinates */
        unsigned int mirror_y: 1; /*!< Mirror Y after read coordinates */
    } flags;

    /*!< User callback called after get coordinates from touch controller for apply user adjusting */
    void (*process_coordinates)(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
    /*!< User callback called after the touch interrupt occurred */
    esp_lcd_touch_interrupt_callback_t interrupt_callback;
    /*!< User data passed to callback */
    void *user_data;
    /*!< User data passed to driver */
    void *driver_data;
} esp_lcd_touch_config_t;

typedef struct {
    uint8_t points; /*!< Count of touch points saved */

    struct {
        uint16_t x; /*!< X coordinate */
        uint16_t y; /*!< Y coordinate */
        uint16_t strength; /*!< Strength */
    } coords[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];

#if (CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS > 0)
    uint8_t buttons; /*!< Count of buttons states saved */

    struct {
        uint8_t status; /*!< Status of button */
    } button[CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS];
#endif

    portMUX_TYPE lock; /*!< Lock for read/write */
} esp_lcd_touch_data_t;

/**
 * @brief Declare of Touch Type
 */
struct esp_lcd_touch_s {
    esp_err_t (*enter_sleep)(esp_lcd_touch_handle_t tp);
    esp_err_t (*exit_sleep)(esp_lcd_touch_handle_t tp);
    esp_err_t (*read_data)(esp_lcd_touch_handle_t tp);
    bool (*get_xy)(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
#if (CONFIG_ESP_LCD_TOUCH_MAX_BUTTONS > 0)
    esp_err_t (*get_button_state)(esp_lcd_touch_handle_t tp, uint8_t n, uint8_t *state);
#endif
    esp_err_t (*set_swap_xy)(esp_lcd_touch_handle_t tp, bool swap);
    esp_err_t (*get_swap_xy)(esp_lcd_touch_handle_t tp, bool *swap);
    esp_err_t (*set_mirror_x)(esp_lcd_touch_handle_t tp, bool mirror);
    esp_err_t (*get_mirror_x)(esp_lcd_touch_handle_t tp, bool *mirror);
    esp_err_t (*set_mirror_y)(esp_lcd_touch_handle_t tp, bool mirror);
    esp_err_t (*get_mirror_y)(esp_lcd_touch_handle_t tp, bool *mirror);
    esp_err_t (*del)(esp_lcd_touch_handle_t tp);

    esp_lcd_touch_config_t config;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_touch_data_t data;
};

/* Convenience inline functions */

static inline esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp) {
    if (tp && tp->read_data) return tp->read_data(tp);
    return ESP_ERR_INVALID_ARG;
}

static inline bool esp_lcd_touch_get_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num) {
    if (tp && tp->get_xy) return tp->get_xy(tp, x, y, strength, point_num, max_point_num);
    return false;
}

static inline esp_err_t esp_lcd_touch_del(esp_lcd_touch_handle_t tp) {
    if (tp && tp->del) return tp->del(tp);
    return ESP_ERR_INVALID_ARG;
}

static inline esp_err_t esp_lcd_touch_register_interrupt_callback(esp_lcd_touch_handle_t tp, esp_lcd_touch_interrupt_callback_t callback) {
    if (tp) {
        tp->config.interrupt_callback = callback;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

#ifdef __cplusplus
}
#endif
