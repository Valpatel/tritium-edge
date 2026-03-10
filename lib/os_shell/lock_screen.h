// Tritium-OS Lock Screen — PIN-based device security.
//
// Shows a full-screen overlay with clock and PIN entry keypad.
// PIN is stored in NVS (SettingsDomain::SYSTEM, "lock_pin").
// Empty PIN = lock screen disabled.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "lvgl.h"

namespace lock_screen {

/// Initialize the lock screen (call after shell init).
void init();

/// Show the lock screen overlay.
void show();

/// Check if lock screen is currently showing.
bool is_locked();

/// Check if a PIN has been configured.
bool is_enabled();

/// Set or clear the PIN (empty string disables lock screen).
void set_pin(const char* pin);

/// Lock screen tick (updates clock).
void tick();

}  // namespace lock_screen
