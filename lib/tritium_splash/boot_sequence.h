/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file boot_sequence.h
 * @brief Cyberpunk-styled boot log for Tritium-OS builds.
 *
 * Replaces the simple splash screen with an animated service initialization
 * display showing each service as it comes online. Uses the same direct
 * framebuffer rendering as tritium_splash (no LVGL dependency).
 *
 * Only compiled for OS builds (ENABLE_SETTINGS or ENABLE_DIAG defined).
 */

#pragma once

#if defined(ENABLE_SETTINGS) || defined(ENABLE_DIAG)

#include "esp_lcd_panel_ops.h"

namespace boot_sequence {

/**
 * @brief Initialize the boot display.
 *
 * Allocates a PSRAM framebuffer and DMA transfer buffer.
 * Call after display_init() and display_set_brightness().
 *
 * @param panel  esp_lcd panel handle from display_get_panel()
 * @param width  display width in pixels
 * @param height display height in pixels
 */
void init(esp_lcd_panel_handle_t panel, int width, int height);

/**
 * @brief Show the Tritium-OS logo and version header.
 *
 * Renders the triangle logo glyph and "TRITIUM-OS vX.X" at the top
 * of the display. Call once after init().
 *
 * @param version version string (e.g. "2.0")
 */
void showLogo(const char* version);

/**
 * @brief Show a boot status line for a service.
 *
 * Each call appends a new line to the boot log and renders immediately.
 * The line format is: [ name      ] status  detail
 *
 * @param name   service name (will be left-padded to 10 chars)
 * @param status "ok", "fail", or "skip"
 * @param detail optional extra info (IP address, port, peer count, etc.)
 */
void showService(const char* name, const char* status, const char* detail = nullptr);

/**
 * @brief Show "SYSTEM READY" at the bottom of the boot log.
 *
 * Holds briefly so the user can see the final state.
 */
void showReady();

/**
 * @brief Clean up — free the framebuffer and DMA buffer.
 *
 * Call before handing off to the main app.
 */
void finish();

} // namespace boot_sequence

#endif // ENABLE_SETTINGS || ENABLE_DIAG
