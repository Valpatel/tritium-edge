/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file shell_theme.h
 * @brief Tritium cyberpunk theme for LVGL — colors, styles, and widget helpers.
 *
 * Defines the Valpatel design language: neon accents on dark surfaces, monospace
 * data values, glowing borders, and resolution-adaptive sizing. All color
 * constants use the canonical Tritium palette from TRITIUM-OS-VISION.md.
 */

#pragma once
#include "lvgl.h"

// --- Tritium color palette (global scope — lv_color_hex is not namespaced) --

#define T_CYAN      lv_color_hex(0x00f0ff)
#define T_MAGENTA   lv_color_hex(0xff2a6d)
#define T_GREEN     lv_color_hex(0x05ffa1)
#define T_YELLOW    lv_color_hex(0xfcee0a)
#define T_VOID      lv_color_hex(0x0a0a0f)
#define T_SURFACE1  lv_color_hex(0x0e0e14)
#define T_SURFACE2  lv_color_hex(0x12121a)
#define T_SURFACE3  lv_color_hex(0x1a1a2e)
#define T_GHOST     lv_color_hex(0x8888aa)
#define T_TEXT      lv_color_hex(0xc8d0dc)
#define T_BRIGHT    lv_color_hex(0xe0e0ff)

namespace tritium_theme {

// --- Theme application ----------------------------------------------------

/**
 * @brief Apply the Tritium theme to the active LVGL display.
 *
 * Overrides the default theme with cyberpunk colors, sets global scrollbar
 * and focus styles, and configures the screen background to void black.
 */
void apply();

// --- Themed widget constructors -------------------------------------------
// Each function creates a styled widget and returns its LVGL object pointer.

/// Panel with surface-2 background, 1px cyan border at 20% opacity, 4px radius.
lv_obj_t* createPanel(lv_obj_t* parent, const char* title = nullptr);

/// Button with transparent background, cyan border, uppercase label.
lv_obj_t* createButton(lv_obj_t* parent, const char* label,
                        lv_color_t accent = T_CYAN);

/// Label in text color. Set mono=true for monospace (Unscii-style) rendering.
lv_obj_t* createLabel(lv_obj_t* parent, const char* text, bool mono = false);

/// Small colored dot for status indicators.
lv_obj_t* createStatusDot(lv_obj_t* parent, lv_color_t color);

/// Slider with surface-3 track and cyan knob.
lv_obj_t* createSlider(lv_obj_t* parent, int min, int max, int initial);

/// Switch: ghost when off, cyan when on.
lv_obj_t* createSwitch(lv_obj_t* parent, bool initial);

}  // namespace tritium_theme
