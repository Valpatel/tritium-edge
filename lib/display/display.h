/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file display.h
 * @brief Public display API for the tritium-edge project.
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
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Display health information for remote diagnostics.
 *
 * Populated by display_init() and display_verify(). Exposed via
 * display_get_health() so APIs can include display status in responses.
 */
typedef struct {
    bool initialized;       /**< display_init() returned ESP_OK */
    bool verified;          /**< Panel ID readback matched expected value */
    const char* board_name; /**< Compile-time board identifier string */
    const char* driver;     /**< Display driver name (e.g. "AXS15231B") */
    int width;              /**< Expected display width in pixels */
    int height;             /**< Expected display height in pixels */
    uint32_t expected_id;   /**< Expected panel chip ID */
    uint32_t actual_id;     /**< Actual panel chip ID from readback (0 = read failed) */
    uint32_t frame_count;   /**< Number of successful draw_bitmap calls */
} display_health_t;

/**
 * @brief Initialize display for the current board (selected by build flags).
 *
 * This sets up the SPI/RGB bus, panel IO, panel driver, and backlight.
 * After hardware init, calls display_verify() to read back the panel chip ID
 * and confirm the correct display is connected.
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
 * For QSPI panels: signaled by the SPI DMA completion ISR.
 * For RGB panels: signaled by the on_color_trans_done callback when LVGL's
 * draw buffer has been copied into the panel's internal framebuffer.
 *
 * @return Binary semaphore handle, or NULL if display not initialized.
 */
SemaphoreHandle_t display_get_flush_semaphore(void);

/**
 * @brief Get the "GUI ready" semaphore for the vsync handshake (RGB panels only).
 *
 * LVGL gives this semaphore when rendering is complete and it's ready for the
 * FB swap. The on_vsync ISR takes this to confirm LVGL is waiting, then
 * gives sem_vsync_end to unblock LVGL.
 *
 * @return Binary semaphore handle, or NULL for QSPI panels.
 */
SemaphoreHandle_t display_get_sem_gui_ready(void);

/**
 * @brief Get the "vsync end" semaphore for the vsync handshake (RGB panels only).
 *
 * The on_vsync ISR gives this semaphore when vsync fires AND LVGL was waiting
 * (gui_ready was taken). LVGL takes this to proceed with the FB swap, ensuring
 * the swap happens at the NEXT vsync after rendering completes.
 *
 * @return Binary semaphore handle, or NULL for QSPI panels.
 */
SemaphoreHandle_t display_get_sem_vsync_end(void);

/**
 * @brief Check if display is an RGB parallel panel.
 *
 * RGB panels (ST7262 on 4.3C-BOX) use continuous DMA scanout and require
 * vsync synchronization to prevent tearing. QSPI panels use command-based
 * transfers and don't need vsync.
 *
 * @return true if the display is an RGB parallel panel.
 */
bool display_is_rgb(void);

/**
 * @brief Get the active framebuffer being scanned to the display.
 *
 * For RGB panels: returns the panel's internal DMA framebuffer (what's on screen).
 * For QSPI panels: returns NULL (no persistent framebuffer).
 *
 * @return Pointer to RGB565 pixel data, or NULL.
 */
uint16_t* display_get_framebuffer(void);

/**
 * @brief Get both RGB panel framebuffers for direct LVGL rendering.
 *
 * RGB panels with num_fbs=2 have two internal DMA framebuffers in PSRAM.
 * LVGL should use these DIRECTLY as its draw buffers (zero-copy pattern).
 * When esp_lcd_panel_draw_bitmap() receives a pointer that IS one of these
 * FBs, it performs an atomic buffer swap instead of a bounce-buffer copy.
 * This eliminates tearing.
 *
 * For QSPI panels: returns ESP_ERR_NOT_SUPPORTED (no persistent FBs).
 *
 * @param[out] fb0 First framebuffer pointer
 * @param[out] fb1 Second framebuffer pointer
 * @return ESP_OK on success
 */
esp_err_t display_get_rgb_framebuffers(void** fb0, void** fb1);

/**
 * @brief Get display health information for diagnostics.
 *
 * Returns a snapshot of display initialization, verification, and runtime
 * status. Use this in web APIs and diagnostics to detect board/firmware
 * mismatches (e.g., firmware compiled for 3.49 running on 4.3C hardware).
 *
 * @return Pointer to static display_health_t struct (never NULL).
 */
const display_health_t* display_get_health(void);

/**
 * @brief Increment the frame counter (call after each successful draw_bitmap).
 *
 * Apps should call this after pushing a frame to the display. A zero frame
 * count in the health struct indicates the display may not be rendering.
 */
void display_count_frame(void);

#ifdef __cplusplus
}
#endif
