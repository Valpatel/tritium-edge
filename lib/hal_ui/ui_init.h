#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lvgl.h>

// Initialize LVGL with LovyanGFX as the display backend.
// Call once from App::setup(). Returns the LVGL display object.
// Uses PSRAM for draw buffers when available.
lv_display_t* ui_init(lgfx::LGFX_Device& display);

// Call from App::loop() to drive LVGL timers.
// Returns milliseconds until next call is needed.
uint32_t ui_tick();
