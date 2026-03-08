/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file tritium_splash.h
 * @brief Boot splash screen for Tritium-Edge nodes.
 *
 * Shows branded splash on every boot: "TRITIUM" title, creator credit,
 * version string. Fades in from black, holds, fades out, then returns.
 *
 * Call once between display_init()/display_set_brightness() and app->setup().
 */

#pragma once

#include "esp_lcd_panel_ops.h"

#define TRITIUM_VERSION "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Show Tritium boot splash with fade animation.
 *
 * Allocates a temporary PSRAM framebuffer, renders text, animates
 * fade-in (500ms) -> hold (1.5s) -> fade-out (500ms), then frees memory.
 *
 * @param panel  esp_lcd panel handle from display_get_panel()
 * @param w      display width in pixels
 * @param h      display height in pixels
 */
void tritium_splash(esp_lcd_panel_handle_t panel, int w, int h);

#ifdef __cplusplus
}
#endif
