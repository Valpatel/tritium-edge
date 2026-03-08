// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#pragma once

#include <lvgl.h>
#include "esp_lcd_panel_ops.h"

// Initialize LVGL with esp_lcd as the display backend.
// Call once from App::setup(). Returns the LVGL display object.
// Uses PSRAM for draw buffers when available.
lv_display_t* ui_init(esp_lcd_panel_handle_t panel, int width, int height);

// Call from App::loop() to drive LVGL timers.
// Returns milliseconds until next call is needed.
uint32_t ui_tick();
