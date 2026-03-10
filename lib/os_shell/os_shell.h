/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file os_shell.h
 * @brief Tritium-OS Shell — LVGL window manager with status bar, navigation,
 *        app launcher, and notification system for ESP32-S3.
 *
 * Provides the system chrome that wraps all apps: a persistent status bar at
 * the top, a gesture-activated nav bar at the bottom, a grid-based app
 * launcher, and stacking toast notifications.
 *
 * Adapts layout automatically across all 6 supported display resolutions
 * (172x640 through 800x480).
 *
 * Usage:
 *   // After LVGL is initialized (via ui_init or equivalent):
 *   tritium_shell::init(panel, width, height);
 *   tritium_shell::registerApp({"Settings", "System settings", LV_SYMBOL_SETTINGS,
 *                               true, settings_create});
 *   // In main loop:
 *   tritium_shell::tick();
 */

#pragma once
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

namespace tritium_shell {

/// Shell layout configuration (auto-calculated from screen dimensions).
struct ShellConfig {
    int screen_width;
    int screen_height;
    int status_bar_height;
    int nav_bar_height;
};

// --- UI scaling helpers (for use by shell apps) ---------------------------

/// Get the body text font (scaled for current display).
const lv_font_t* uiFont();

/// Get the heading font (larger, scaled for current display).
const lv_font_t* uiHeadingFont();

/// Get the small/caption font (scaled for current display).
const lv_font_t* uiSmallFont();

/// Get the icon font for buttons/icons (scaled for current display).
const lv_font_t* uiIconFont();

/// Get the current shell config (screen dimensions, bar heights).
const ShellConfig& uiConfig();

/// Severity level for toast and shade notifications.
enum NotifyLevel {
    NOTIFY_INFO,
    NOTIFY_SUCCESS,
    NOTIFY_WARNING,
    NOTIFY_ERROR
};

/// Descriptor for a launchable application.
struct AppDescriptor {
    const char* name;
    const char* description;
    const char* icon;        ///< LV_SYMBOL_* constant or custom icon data
    bool is_system;
    void (*launch)(lv_obj_t* viewport);  ///< Creates the app UI inside viewport
};

/**
 * @brief Initialize the shell (status bar, nav bar, viewport, gesture layer).
 *
 * LVGL must already be initialized before calling this. The shell creates its
 * widget tree on the active screen and sets the Tritium cyberpunk theme.
 *
 * @param panel  esp_lcd panel handle (stored for brightness control)
 * @param width  display width in pixels
 * @param height display height in pixels
 * @return true on success
 */
bool init(esp_lcd_panel_handle_t panel, int width, int height);

/**
 * @brief Shell tick — call from the main loop.
 *
 * Updates the clock, checks gesture state, auto-hides nav bar, and processes
 * toast timeouts. Does NOT call lv_timer_handler() — the caller must do that
 * separately (or via ui_tick()).
 */
void tick();

// --- Status bar updates ---------------------------------------------------

void setWifiStatus(bool connected, int8_t rssi);
void setBleStatus(int device_count);
void setMeshStatus(int peer_count);
void setBatteryStatus(int percent, bool charging);
void setClock(int hour, int minute);
void setAppName(const char* name);

// --- Notifications --------------------------------------------------------

/// Show a brief toast overlay (auto-dismisses). Max 3 stacked.
void toast(const char* message,
           NotifyLevel level = NOTIFY_INFO,
           uint32_t duration_ms = 3000);

/// Add a persistent notification to the shade.
void notify(const char* title, const char* message,
            NotifyLevel level = NOTIFY_INFO);

// --- App management -------------------------------------------------------

/// Register an app for the launcher (max 16).
void registerApp(const AppDescriptor& app);

/// Show the grid app launcher in the viewport.
void showLauncher();

/// Launch a registered app by index.
void showApp(int app_index);

/// Get the app viewport container (apps render into this).
lv_obj_t* getViewport();

/// Get number of registered apps.
int getAppCount();

/// Get app descriptor by index (nullptr if out of range).
const AppDescriptor* getApp(int index);

/// Get currently active app index (-1 if none).
int getActiveApp();

// --- Navigation -----------------------------------------------------------

void showNavBar();
void hideNavBar();
void showNotificationShade();
void hideNotificationShade();

}  // namespace tritium_shell
