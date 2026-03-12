// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#pragma once
// Starfield screensaver overlay for Tritium-OS shell.
// Renders a 3D starfield on a full-screen LVGL canvas that covers all
// shell content.  Activated by inactivity timeout, dismissed on touch.

#include "lvgl.h"

namespace shell_screensaver {

/// Initialize the screensaver subsystem (call once after shell init).
void init(int screen_width, int screen_height);

/// Call every tick (~1ms).  Checks idle time, manages activation.
void tick();

/// True when the screensaver overlay is visible.
bool isActive();

/// Dismiss the screensaver and return to the shell.
void dismiss();

/// Force-activate the screensaver (for testing / preview).
void activate();

/// Update timeout from settings (called when user changes slider).
void setTimeoutS(uint32_t seconds);

/// Reload all screensaver settings from NVS (call after any setting change).
void reloadSettings();

}  // namespace shell_screensaver
