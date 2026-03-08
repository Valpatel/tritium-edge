/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file display.h
 * @brief Public display API for the esp32-hardware project.
 *
 * This is the ONLY display interface code in the project. It wraps Espressif's
 * esp_lcd framework, dispatching to the correct panel driver and init sequence
 * based on compile-time board selection (BOARD_* defines).
 *
 * Supported boards:
 *   - BOARD_TOUCH_LCD_349    -> AXS15231B QSPI (172x640)
 *   - BOARD_TOUCH_LCD_35BC   -> AXS15231B QSPI (320x480)
 *   - BOARD_TOUCH_AMOLED_241B -> RM690B0 QSPI (450x600)
 *   - BOARD_TOUCH_AMOLED_18  -> SH8601Z QSPI (368x448)
 *   - BOARD_AMOLED_191M      -> RM67162 QSPI (240x536)
 *   - BOARD_TOUCH_LCD_43C_BOX -> ST7262 RGB (800x480)
 *
 * Usage:
 *   esp_err_t ret = display_init();
 *   if (ret == ESP_OK) {
 *       esp_lcd_panel_handle_t panel = display_get_panel();
 *       // Use esp_lcd_panel_draw_bitmap() etc.
 *   }
 */

#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize display for the current board (selected by build flags).
 *
 * This sets up the SPI/RGB bus, panel IO, panel driver, and backlight.
 * For boards with TCA9554 reset (3.5B-C, 1.8), the I2C bus must already
 * be initialized before calling this function.
 *
 * @return ESP_OK on success
 */
esp_err_t display_init(void);

/**
 * @brief Get the esp_lcd panel handle for direct rendering.
 *
 * Use with esp_lcd_panel_draw_bitmap(), esp_lcd_panel_disp_on_off(), etc.
 *
 * @return Panel handle, or NULL if display_init() has not been called.
 */
esp_lcd_panel_handle_t display_get_panel(void);

/**
 * @brief Get the esp_lcd panel IO handle.
 *
 * Useful for sending custom commands to the panel.
 *
 * @return Panel IO handle, or NULL if display_init() has not been called.
 */
esp_lcd_panel_io_handle_t display_get_panel_io(void);

/**
 * @brief Get display width in pixels.
 */
int display_get_width(void);

/**
 * @brief Get display height in pixels.
 */
int display_get_height(void);

/**
 * @brief Set display backlight brightness.
 *
 * For GPIO-backlight boards (3.49, 3.5B-C): uses LEDC PWM.
 * For AMOLED boards: sends 0x51 brightness command to panel.
 * For 4.3C-BOX: sends command to IO extension chip.
 *
 * @param brightness 0-255 (0=off, 255=full brightness)
 */
void display_set_brightness(uint8_t brightness);

/**
 * @brief Get the DMA flush-done semaphore.
 *
 * Signaled from ISR when an esp_lcd_panel_draw_bitmap() DMA transfer completes.
 * Use with xSemaphoreTake() for proper DMA synchronization in flush callbacks.
 *
 * @return Binary semaphore handle, or NULL if display not initialized.
 */
SemaphoreHandle_t display_get_flush_semaphore(void);

#ifdef __cplusplus
}
#endif
