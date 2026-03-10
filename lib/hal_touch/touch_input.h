#pragma once
// Unified touch input layer — bridges hardware TouchHAL and remote touch
// injection into a single LVGL input device.
//
// Usage:
//   #include "touch_input.h"
//   touch_input::init();          // after LVGL + TouchHAL init
//   touch_input::inject(x, y, true);  // from web API
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstdint>

#ifndef SIMULATOR
#include "lvgl.h"
#endif

namespace touch_input {

/// Initialize: creates LVGL indev, starts polling hardware touch.
/// Call after lv_init() and TouchHAL::init().
bool init();

/// Inject a remote touch event (from web API). Thread-safe.
void inject(uint16_t x, uint16_t y, bool pressed);

/// Clear any pending injected touch (release).
void injectRelease();

/// Get millis() timestamp of last touch activity (hw or remote).
uint32_t lastActivityMs();

/// Returns true if a remote touch is currently active.
bool isRemoteActive();

/// Debug info for remote diagnostics
struct DebugInfo {
    uint32_t read_cb_calls;
    uint32_t hw_touch_count;
    uint32_t inject_count;
    int16_t last_raw_x;
    int16_t last_raw_y;
    uint32_t last_touch_ms;
    bool hw_available;
    bool currently_pressed;
};
DebugInfo getDebugInfo();

#ifndef SIMULATOR
/// LVGL read callback — registered automatically by init().
void read_cb(lv_indev_t* indev, lv_indev_data_t* data);
#endif

}  // namespace touch_input
