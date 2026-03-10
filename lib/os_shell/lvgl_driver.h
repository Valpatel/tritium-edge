/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file lvgl_driver.h
 * @brief LVGL 9.2 display driver bridging LVGL to the esp_lcd panel system.
 *
 * Replaces the LovyanGFX-based ui_init for Tritium-OS builds. Provides the
 * flush callback that pushes LVGL draw buffers to the hardware panel via
 * esp_lcd_panel_draw_bitmap(), with proper DMA synchronization.
 *
 * Supports all 6 board display types:
 *   - QSPI panels (AXS15231B, SH8601, RM690B0, RM67162): async DMA with
 *     flush semaphore from display_get_flush_semaphore()
 *   - RGB parallel panel (ST7262, 4.3C-BOX): synchronous bounce-buffer DMA,
 *     no semaphore needed
 *
 * Tick source: LVGL reads time via LV_TICK_CUSTOM / millis() configured in
 * lv_conf.h. No additional tick timer is needed.
 *
 * Usage:
 *   #include "lvgl_driver.h"
 *   #include "display.h"
 *
 *   display_init();
 *   lv_display_t* disp = lvgl_driver::init(
 *       display_get_panel(),
 *       display_get_width(),
 *       display_get_height());
 *
 *   // Main loop:
 *   uint32_t delay = lvgl_driver::tick();
 */

#pragma once

#if defined(ENABLE_SHELL)

#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"

namespace lvgl_driver {

/**
 * @brief Initialize LVGL core and create a display backed by esp_lcd.
 *
 * Sets up LVGL, allocates double-buffered draw buffers in PSRAM (with
 * fallback to regular heap), and configures the flush callback for DMA
 * rendering through the given esp_lcd panel handle.
 *
 * Call after display_init() and before tritium_shell::init().
 *
 * @param panel  esp_lcd panel handle from display_get_panel()
 * @param width  display width in pixels
 * @param height display height in pixels
 * @return LVGL display object, or nullptr on allocation failure
 */
lv_display_t* init(esp_lcd_panel_handle_t panel, int width, int height);

/**
 * @brief Run LVGL timer handler — call from the main loop.
 *
 * Drives LVGL rendering, animation, and input processing. Must be called
 * from the same task/core as LVGL widget creation (typically core 1).
 *
 * @return Milliseconds until the next call is needed (from lv_timer_handler)
 */
uint32_t tick();

/**
 * @brief Get the LVGL display object.
 *
 * @return LVGL display, or nullptr if init() has not been called.
 */
lv_display_t* display();

/// True if the panel is RGB parallel (DIRECT render mode)
bool isRgb();

/// Debug: total flush_cb invocations
uint32_t getFlushCount();

/// Debug: millis() of last flush
uint32_t getLastFlushMs();

int getWidth();
int getHeight();

/// For DIRECT mode: pointer to the current draw buffer (full-screen)
const uint8_t* getFramebuffer();

}  // namespace lvgl_driver

#endif  // ENABLE_SHELL
