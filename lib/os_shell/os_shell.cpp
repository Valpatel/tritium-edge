/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file os_shell.cpp
 * @brief Tritium-OS Shell implementation — LVGL window manager.
 *
 * Screen widget tree:
 *   lv_screen
 *   +-- status_bar (top, fixed height)
 *   |   +-- app_name_label (left)
 *   |   +-- status_icons_container (right, horizontal flex)
 *   |       +-- wifi_icon + rssi_label
 *   |       +-- ble_icon + count_label
 *   |       +-- mesh_icon + count_label
 *   |       +-- battery_icon + pct_label
 *   |       +-- clock_label
 *   +-- viewport (flex grow, fills remaining space)
 *   |   +-- [active app content]
 *   +-- nav_bar (bottom, initially hidden, shown by swipe-up gesture)
 *   |   +-- btn_home, btn_back, btn_launcher
 *   +-- gesture_top (invisible overlay, top strip, for swipe-down)
 *   +-- gesture_bottom (invisible overlay, bottom strip, for swipe-up)
 *   +-- toast_container (overlay, top-right, for stacking toasts)
 *   +-- notification_shade (overlay, full screen, initially hidden)
 */

#include "os_shell.h"
#include "shell_theme.h"
#include "shell_apps.h"
#include "lock_screen.h"
#include "tritium_splash.h"  // TRITIUM_VERSION
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef SIMULATOR
#include "tritium_compat.h"
#else
#include <SDL2/SDL.h>
static uint32_t millis() { return SDL_GetTicks(); }
#endif

// Event bus → toast bridge
#if __has_include("os_events.h")
#include "os_events.h"
#define EVENTS_AVAILABLE 1
#else
#define EVENTS_AVAILABLE 0
#endif

namespace tritium_shell {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static const int MAX_APPS = 16;
static const int MAX_TOASTS = 3;
static const int MAX_NOTIFICATIONS = 10;

static ShellConfig s_cfg = {};
static esp_lcd_panel_handle_t s_panel = nullptr;

// Screen layout objects
static lv_obj_t* s_status_bar = nullptr;
static lv_obj_t* s_viewport = nullptr;
static lv_obj_t* s_nav_bar = nullptr;
static lv_obj_t* s_toast_container = nullptr;
static lv_obj_t* s_notif_shade = nullptr;
static lv_obj_t* s_gesture_top = nullptr;
static lv_obj_t* s_gesture_bottom = nullptr;

// Status bar children
static lv_obj_t* s_app_name_label = nullptr;
static lv_obj_t* s_wifi_icon = nullptr;
static lv_obj_t* s_rssi_label = nullptr;
static lv_obj_t* s_ble_icon = nullptr;
static lv_obj_t* s_ble_count_label = nullptr;
static lv_obj_t* s_mesh_icon = nullptr;
static lv_obj_t* s_mesh_count_label = nullptr;
static lv_obj_t* s_batt_icon = nullptr;
static lv_obj_t* s_batt_pct_label = nullptr;
static lv_obj_t* s_clock_label = nullptr;

// Nav bar state
static bool s_nav_visible = false;
static uint32_t s_nav_show_time = 0;
static const uint32_t NAV_AUTO_HIDE_MS = 3000;
static bool s_nav_persistent = false;  // true when inside an app (always show nav)

// App registry
static AppDescriptor s_apps[MAX_APPS];
static int s_app_count = 0;
static int s_active_app = -1;

// Toast tracking
struct ToastEntry {
    lv_obj_t* obj;
    uint32_t expire_time;
};
static ToastEntry s_toasts[MAX_TOASTS] = {};

// Notification shade entries
struct NotifEntry {
    char title[32];
    char message[64];
    NotifyLevel level;
    bool active;
};
static NotifEntry s_notifs[MAX_NOTIFICATIONS] = {};
static bool s_shade_visible = false;

// Screen size class
enum SizeClass { SIZE_SMALL, SIZE_MEDIUM, SIZE_LARGE };
static SizeClass s_size_class = SIZE_MEDIUM;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const lv_font_t* font_for_size(SizeClass sc) {
    switch (sc) {
        case SIZE_SMALL:  return &lv_font_montserrat_10;
        case SIZE_MEDIUM: return &lv_font_montserrat_12;
        case SIZE_LARGE:  return &lv_font_montserrat_20;
    }
    return &lv_font_montserrat_12;
}

static lv_color_t color_for_level(NotifyLevel level) {
    switch (level) {
        case NOTIFY_INFO:    return T_CYAN;
        case NOTIFY_SUCCESS: return T_GREEN;
        case NOTIFY_WARNING: return T_YELLOW;
        case NOTIFY_ERROR:   return T_MAGENTA;
    }
    return T_CYAN;
}

// --- Public UI scaling helpers (used by shell_apps) -----------------------

const lv_font_t* uiFont() { return font_for_size(s_size_class); }

const lv_font_t* uiHeadingFont() {
    switch (s_size_class) {
        case SIZE_SMALL:  return &lv_font_montserrat_14;
        case SIZE_MEDIUM: return &lv_font_montserrat_20;
        case SIZE_LARGE:  return &lv_font_montserrat_28;
    }
    return &lv_font_montserrat_20;
}

const lv_font_t* uiSmallFont() {
    switch (s_size_class) {
        case SIZE_SMALL:  return &lv_font_montserrat_10;
        case SIZE_MEDIUM: return &lv_font_montserrat_10;
        case SIZE_LARGE:  return &lv_font_montserrat_14;
    }
    return &lv_font_montserrat_10;
}

const lv_font_t* uiIconFont() {
    switch (s_size_class) {
        case SIZE_SMALL:  return &lv_font_montserrat_16;
        case SIZE_MEDIUM: return &lv_font_montserrat_20;
        case SIZE_LARGE:  return &lv_font_montserrat_36;
    }
    return &lv_font_montserrat_20;
}

const ShellConfig& uiConfig() { return s_cfg; }

// Forward declarations
static void create_alive_dot(lv_obj_t* parent);

// ---------------------------------------------------------------------------
// Status bar creation
// ---------------------------------------------------------------------------

static void create_status_bar(lv_obj_t* screen) {
    s_status_bar = lv_obj_create(screen);
    lv_obj_set_size(s_status_bar, s_cfg.screen_width, s_cfg.status_bar_height);
    lv_obj_set_pos(s_status_bar, 0, 0);
    lv_obj_remove_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Styling: surface-1 background, 1px bottom border
    lv_obj_set_style_bg_color(s_status_bar, T_SURFACE1, 0);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_status_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_status_bar, 1, 0);
    lv_obj_set_style_border_color(s_status_bar, T_CYAN, 0);
    lv_obj_set_style_border_opa(s_status_bar, LV_OPA_20, 0);
    lv_obj_set_style_radius(s_status_bar, 0, 0);
    int sb_pad_h = (s_size_class == SIZE_LARGE) ? 8 : 4;
    int sb_pad_v = (s_size_class == SIZE_LARGE) ? 4 : 1;
    lv_obj_set_style_pad_hor(s_status_bar, sb_pad_h, 0);
    lv_obj_set_style_pad_ver(s_status_bar, sb_pad_v, 0);

    // Flex layout: app name left, status icons right
    lv_obj_set_flex_flow(s_status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const lv_font_t* font = font_for_size(s_size_class);
    bool compact = (s_size_class == SIZE_SMALL);

    // App name (left side)
    s_app_name_label = lv_label_create(s_status_bar);
    lv_label_set_text(s_app_name_label, "TRITIUM");
    lv_obj_set_style_text_color(s_app_name_label, T_BRIGHT, 0);
    lv_obj_set_style_text_font(s_app_name_label, font, 0);

    // Right-side icon container — flex-grow to fill remaining space after app name.
    // Using LV_SIZE_CONTENT would let it overflow the status bar right edge.
    // With flex_grow, icons get exactly the remaining width, and LV_FLEX_ALIGN_END
    // within the container pushes all icons flush-right. No overflow, no clipping.
    lv_obj_t* icons = lv_obj_create(s_status_bar);
    lv_obj_set_height(icons, lv_pct(100));
    lv_obj_set_width(icons, 0);
    lv_obj_set_flex_grow(icons, 1);
    lv_obj_set_style_bg_opa(icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icons, 0, 0);
    lv_obj_set_style_pad_all(icons, 0, 0);
    lv_obj_set_style_pad_gap(icons, compact ? 3 : 6, 0);
    lv_obj_set_flex_flow(icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(icons, LV_OBJ_FLAG_SCROLLABLE);

    // WiFi signal icon
    s_wifi_icon = lv_label_create(icons);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_icon, T_GHOST, 0);
    lv_obj_set_style_text_font(s_wifi_icon, font, 0);

    if (!compact) {
        s_rssi_label = lv_label_create(icons);
        lv_label_set_text(s_rssi_label, "--");
        lv_obj_set_style_text_color(s_rssi_label, T_GHOST, 0);
        lv_obj_set_style_text_font(s_rssi_label, font, 0);
    }

    // BLE device count
    s_ble_icon = lv_label_create(icons);
    lv_label_set_text(s_ble_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(s_ble_icon, T_GHOST, 0);
    lv_obj_set_style_text_font(s_ble_icon, font, 0);

    if (!compact) {
        s_ble_count_label = lv_label_create(icons);
        lv_label_set_text(s_ble_count_label, "0");
        lv_obj_set_style_text_color(s_ble_count_label, T_GHOST, 0);
        lv_obj_set_style_text_font(s_ble_count_label, font, 0);
    }

    // Mesh peer count
    s_mesh_icon = lv_label_create(icons);
    lv_label_set_text(s_mesh_icon, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_color(s_mesh_icon, T_GHOST, 0);
    lv_obj_set_style_text_font(s_mesh_icon, font, 0);

    if (!compact) {
        s_mesh_count_label = lv_label_create(icons);
        lv_label_set_text(s_mesh_count_label, "0");
        lv_obj_set_style_text_color(s_mesh_count_label, T_GHOST, 0);
        lv_obj_set_style_text_font(s_mesh_count_label, font, 0);
    }

    // Battery level
    s_batt_icon = lv_label_create(icons);
    lv_label_set_text(s_batt_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(s_batt_icon, T_GHOST, 0);
    lv_obj_set_style_text_font(s_batt_icon, font, 0);

    if (!compact) {
        s_batt_pct_label = lv_label_create(icons);
        lv_label_set_text(s_batt_pct_label, "--%");
        lv_obj_set_style_text_color(s_batt_pct_label, T_GHOST, 0);
        lv_obj_set_style_text_font(s_batt_pct_label, font, 0);
    }

    // Clock
    s_clock_label = lv_label_create(icons);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_set_style_text_color(s_clock_label, T_TEXT, 0);
    lv_obj_set_style_text_font(s_clock_label, font, 0);

    // Alive breathing dot — subtle visual indicator that the system is running
    create_alive_dot(icons);
}

// ---------------------------------------------------------------------------
// Viewport creation
// ---------------------------------------------------------------------------

static void create_viewport(lv_obj_t* screen) {
    s_viewport = lv_obj_create(screen);

    int vp_y = s_cfg.status_bar_height;
    int vp_h = s_cfg.screen_height - s_cfg.status_bar_height;

    lv_obj_set_pos(s_viewport, 0, vp_y);
    lv_obj_set_size(s_viewport, s_cfg.screen_width, vp_h);
    lv_obj_set_style_bg_color(s_viewport, T_VOID, 0);
    lv_obj_set_style_bg_opa(s_viewport, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_viewport, 0, 0);
    lv_obj_set_style_radius(s_viewport, 0, 0);
    lv_obj_set_style_pad_all(s_viewport, 0, 0);
    lv_obj_add_flag(s_viewport, LV_OBJ_FLAG_SCROLLABLE);
}

// ---------------------------------------------------------------------------
// Nav bar creation (home / back / launcher)
// ---------------------------------------------------------------------------

static void nav_btn_home_cb(lv_event_t* e) {
    (void)e;
    showLauncher();
}

static void nav_btn_back_cb(lv_event_t* e) {
    (void)e;
    // Back goes to launcher (no app history stack yet)
    showLauncher();
}

static lv_obj_t* create_nav_button(lv_obj_t* parent, const char* symbol,
                                    lv_event_cb_t cb) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, lv_pct(90));
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, T_SURFACE3, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, T_CYAN, 0);
    const lv_font_t* nav_font = (s_size_class == SIZE_LARGE)
                                     ? &lv_font_montserrat_28
                                     : &lv_font_montserrat_16;
    lv_obj_set_style_text_font(lbl, nav_font, 0);
    lv_obj_center(lbl);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }

    return btn;
}

static void create_nav_bar(lv_obj_t* screen) {
    s_nav_bar = lv_obj_create(screen);
    lv_obj_set_size(s_nav_bar, s_cfg.screen_width, s_cfg.nav_bar_height);
    lv_obj_set_pos(s_nav_bar, 0,
                   s_cfg.screen_height - s_cfg.nav_bar_height);
    lv_obj_remove_flag(s_nav_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Styling — must be clearly visible and distinct from viewport
    lv_obj_set_style_bg_color(s_nav_bar, T_SURFACE3, 0);
    lv_obj_set_style_bg_opa(s_nav_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_nav_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_nav_bar, 2, 0);
    lv_obj_set_style_border_color(s_nav_bar, T_CYAN, 0);
    lv_obj_set_style_border_opa(s_nav_bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_nav_bar, 0, 0);
    int nav_pad = (s_size_class == SIZE_LARGE) ? 8 : 4;
    lv_obj_set_style_pad_all(s_nav_bar, nav_pad, 0);
    lv_obj_set_style_pad_gap(s_nav_bar, nav_pad, 0);

    // Horizontal flex for buttons
    lv_obj_set_flex_flow(s_nav_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_nav_bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    create_nav_button(s_nav_bar, LV_SYMBOL_LEFT, nav_btn_back_cb);
    create_nav_button(s_nav_bar, LV_SYMBOL_HOME, nav_btn_home_cb);

    // Start hidden
    lv_obj_add_flag(s_nav_bar, LV_OBJ_FLAG_HIDDEN);
    s_nav_visible = false;
}

// ---------------------------------------------------------------------------
// Alive animations
// ---------------------------------------------------------------------------

static lv_obj_t* s_alive_dot = nullptr;
static lv_anim_t s_alive_anim;
static bool s_clock_colon_visible = true;
static uint32_t s_clock_blink_time = 0;
static const uint32_t CLOCK_BLINK_MS = 1000;

// Breathing animation callback for the alive dot (opacity oscillation)
static void alive_dot_anim_cb(void* obj, int32_t value) {
    lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)value, 0);
}

static void create_alive_dot(lv_obj_t* parent) {
    s_alive_dot = lv_obj_create(parent);
    lv_obj_set_size(s_alive_dot, 6, 6);
    lv_obj_set_style_radius(s_alive_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_alive_dot, T_CYAN, 0);
    lv_obj_set_style_bg_opa(s_alive_dot, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_alive_dot, 0, 0);
    lv_obj_set_style_shadow_color(s_alive_dot, T_CYAN, 0);
    lv_obj_set_style_shadow_width(s_alive_dot, 4, 0);
    lv_obj_set_style_shadow_opa(s_alive_dot, LV_OPA_20, 0);
    lv_obj_remove_flag(s_alive_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_alive_dot, LV_OBJ_FLAG_CLICKABLE);

    // Breathing animation: opacity oscillates between 20 and 80
    lv_anim_init(&s_alive_anim);
    lv_anim_set_var(&s_alive_anim, s_alive_dot);
    lv_anim_set_values(&s_alive_anim, LV_OPA_20, LV_OPA_80);
    lv_anim_set_duration(&s_alive_anim, 2000);
    lv_anim_set_repeat_count(&s_alive_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_duration(&s_alive_anim, 2000);
    lv_anim_set_exec_cb(&s_alive_anim, alive_dot_anim_cb);
    lv_anim_start(&s_alive_anim);
}

// ---------------------------------------------------------------------------
// Gesture zones
// ---------------------------------------------------------------------------

static lv_point_t s_gesture_start = {0, 0};

static void gesture_bottom_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_t* indev = lv_indev_active();
        if (indev) {
            lv_indev_get_point(indev, &s_gesture_start);
        }
    } else if (code == LV_EVENT_RELEASED) {
        lv_indev_t* indev = lv_indev_active();
        if (indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            int dy = s_gesture_start.y - p.y;  // positive = swipe up
            if (dy > 20) {
                showNavBar();
            }
        }
    }
}

static void gesture_top_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_t* indev = lv_indev_active();
        if (indev) {
            lv_indev_get_point(indev, &s_gesture_start);
        }
    } else if (code == LV_EVENT_RELEASED) {
        lv_indev_t* indev = lv_indev_active();
        if (indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            int dy = p.y - s_gesture_start.y;  // positive = swipe down
            if (dy > 20) {
                showNotificationShade();
            }
        }
    }
}

static void create_gesture_zones(lv_obj_t* screen) {
    int zone_h = 20;

    // Bottom swipe zone (swipe up to show nav bar)
    s_gesture_bottom = lv_obj_create(screen);
    lv_obj_set_size(s_gesture_bottom, s_cfg.screen_width, zone_h);
    lv_obj_set_pos(s_gesture_bottom, 0, s_cfg.screen_height - zone_h);
    lv_obj_set_style_bg_opa(s_gesture_bottom, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_gesture_bottom, 0, 0);
    lv_obj_remove_flag(s_gesture_bottom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_gesture_bottom, gesture_bottom_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_gesture_bottom, gesture_bottom_cb, LV_EVENT_RELEASED, nullptr);

    // Top swipe zone (swipe down to show notification shade)
    s_gesture_top = lv_obj_create(screen);
    lv_obj_set_size(s_gesture_top, s_cfg.screen_width, zone_h);
    lv_obj_set_pos(s_gesture_top, 0, 0);
    lv_obj_set_style_bg_opa(s_gesture_top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_gesture_top, 0, 0);
    lv_obj_remove_flag(s_gesture_top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_gesture_top, gesture_top_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_gesture_top, gesture_top_cb, LV_EVENT_RELEASED, nullptr);
}

// ---------------------------------------------------------------------------
// Toast container
// ---------------------------------------------------------------------------

static void create_toast_container(lv_obj_t* screen) {
    s_toast_container = lv_obj_create(screen);
    int toast_w = (s_size_class == SIZE_SMALL) ? s_cfg.screen_width - 8
                                                : s_cfg.screen_width * 3 / 4;
    lv_obj_set_size(s_toast_container, toast_w, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_toast_container,
                   s_cfg.screen_width - toast_w - 4,
                   s_cfg.status_bar_height + 4);
    lv_obj_set_style_bg_opa(s_toast_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_toast_container, 0, 0);
    lv_obj_set_style_pad_all(s_toast_container, 0, 0);
    lv_obj_set_style_pad_gap(s_toast_container, 4, 0);
    lv_obj_set_flex_flow(s_toast_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_toast_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_toast_container, LV_OBJ_FLAG_CLICKABLE);
}

// ---------------------------------------------------------------------------
// Notification shade
// ---------------------------------------------------------------------------

static void shade_close_cb(lv_event_t* e) {
    (void)e;
    hideNotificationShade();
}

static void create_notification_shade(lv_obj_t* screen) {
    s_notif_shade = lv_obj_create(screen);
    lv_obj_set_size(s_notif_shade, s_cfg.screen_width, s_cfg.screen_height);
    lv_obj_set_pos(s_notif_shade, 0, 0);

    lv_obj_set_style_bg_color(s_notif_shade, T_VOID, 0);
    lv_obj_set_style_bg_opa(s_notif_shade, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_notif_shade, 0, 0);
    lv_obj_set_style_radius(s_notif_shade, 0, 0);
    lv_obj_set_style_pad_all(s_notif_shade, 8, 0);
    lv_obj_set_style_pad_gap(s_notif_shade, 6, 0);

    lv_obj_set_flex_flow(s_notif_shade, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_notif_shade, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Header
    lv_obj_t* header = lv_label_create(s_notif_shade);
    lv_label_set_text(header, "NOTIFICATIONS");
    lv_obj_set_style_text_color(header, T_BRIGHT, 0);
    lv_obj_set_style_text_font(header, uiHeadingFont(), 0);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_style_text_align(header, LV_TEXT_ALIGN_CENTER, 0);

    // Close button at bottom
    lv_obj_t* close_btn = tritium_theme::createButton(s_notif_shade, "CLOSE");
    lv_obj_set_width(close_btn, lv_pct(50));
    lv_obj_add_event_cb(close_btn, shade_close_cb, LV_EVENT_CLICKED, nullptr);

    // Start hidden
    lv_obj_add_flag(s_notif_shade, LV_OBJ_FLAG_HIDDEN);
    s_shade_visible = false;
}

// ---------------------------------------------------------------------------
// App launcher grid builder
// ---------------------------------------------------------------------------

static void launcher_app_click_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    showApp(idx);
}

static void build_launcher_grid(lv_obj_t* viewport) {
    lv_obj_clean(viewport);

    // Launcher doesn't need scrolling — disable to prevent content overflow
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    int pad = (s_size_class == SIZE_LARGE) ? 16 : 8;
    int gap = (s_size_class == SIZE_LARGE) ? 16 : 8;
    lv_obj_set_style_pad_all(viewport, pad, 0);
    lv_obj_set_style_pad_gap(viewport, gap, 0);

    // Calculate cell size dynamically from available viewport space.
    // Goal: fill the viewport with a grid that uses most of the space.
    int vp_w = s_cfg.screen_width - 2 * pad;
    int vp_h = s_cfg.screen_height - s_cfg.status_bar_height - 2 * pad;

    // Count only available apps for layout calculation
    int n_apps = 0;
    for (int i = 0; i < s_app_count; i++) {
        if (s_apps[i].available) n_apps++;
    }
    if (n_apps < 1) n_apps = 1;
    int cols, rows;
    if (s_size_class == SIZE_SMALL) {
        cols = (vp_w + gap) / (60 + gap);
        if (cols < 2) cols = 2;
    } else {
        // Target: fill width with evenly sized cells
        // For 800px wide screen with 5 apps: 5 cols x 1 row
        // For 800px wide with 10 apps: 5 cols x 2 rows
        cols = (n_apps <= 6) ? n_apps : (int)sqrtf((float)n_apps * vp_w / vp_h) + 1;
        if (cols > n_apps) cols = n_apps;
        if (cols < 2) cols = 2;
        if (cols > 6) cols = 6;
    }
    rows = (n_apps + cols - 1) / cols;
    if (rows < 1) rows = 1;

    int cell_w = (vp_w - (cols - 1) * gap) / cols;
    int cell_h = (vp_h - (rows - 1) * gap) / rows;

    // Cap width to keep cells from being absurdly wide (preserves card-like aspect)
    int max_w = vp_h * 2 / 3;  // width shouldn't exceed ~67% of viewport height
    if (cell_w > max_w) cell_w = max_w;

    const lv_font_t* name_font = font_for_size(s_size_class);

    // System apps first, then user apps — skip unavailable apps
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s_app_count; i++) {
            if (!s_apps[i].available) continue;
            bool is_sys = s_apps[i].is_system;
            if ((pass == 0 && !is_sys) || (pass == 1 && is_sys)) continue;

            lv_obj_t* cell = lv_obj_create(viewport);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, cell_w, cell_h);
            lv_obj_set_style_bg_color(cell, T_SURFACE2, 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(cell, T_CYAN, 0);
            lv_obj_set_style_border_opa(cell, LV_OPA_30, 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_radius(cell, 6, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

            // Pressed state
            lv_obj_set_style_bg_color(cell, T_SURFACE3, LV_STATE_PRESSED);
            lv_obj_set_style_border_opa(cell, LV_OPA_40, LV_STATE_PRESSED);

            // Icon — position at vertical center of cell
            lv_obj_t* icon = lv_label_create(cell);
            lv_label_set_text(icon, s_apps[i].icon ? s_apps[i].icon : LV_SYMBOL_FILE);
            lv_obj_set_style_text_color(icon, T_CYAN, 0);
            const lv_font_t* icon_font;
            if (s_size_class == SIZE_LARGE)
                icon_font = &lv_font_montserrat_36;
            else if (s_size_class == SIZE_SMALL)
                icon_font = &lv_font_montserrat_16;
            else
                icon_font = &lv_font_montserrat_20;
            lv_obj_set_style_text_font(icon, icon_font, 0);
            lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(icon, cell_w);
            lv_obj_align(icon, LV_ALIGN_CENTER, 0, -12);

            // Name — below icon
            lv_obj_t* name = lv_label_create(cell);
            lv_label_set_text(name, s_apps[i].name);
            lv_obj_set_style_text_color(name, T_TEXT, 0);
            lv_obj_set_style_text_font(name, name_font, 0);
            lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(name, cell_w - 8);
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lv_obj_align_to(name, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

            // Click handler
            lv_obj_add_event_cb(cell, launcher_app_click_cb, LV_EVENT_CLICKED,
                                 (void*)(intptr_t)i);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init(esp_lcd_panel_handle_t panel, int width, int height) {
    s_panel = panel;

    // Determine size class from shortest dimension
    // Size class based on total pixel area, not just short side.
    // 800x480 = 384000 is a large screen. 320x480 = 153600 is medium.
    int area = width * height;
    if (area >= 300000) {
        s_size_class = SIZE_LARGE;    // 800x480, 450x600, etc.
    } else if (area >= 100000) {
        s_size_class = SIZE_MEDIUM;   // 320x480, 368x448, etc.
    } else {
        s_size_class = SIZE_SMALL;    // 172x640 (110080) — actually medium, 240x536 (128640)
    }
    // Override: narrow but tall screens (e.g. 172x640) are medium at most
    int short_side = (width < height) ? width : height;
    if (short_side < 250 && s_size_class > SIZE_SMALL) {
        s_size_class = SIZE_SMALL;
    }

    // Calculate bar heights
    s_cfg.screen_width = width;
    s_cfg.screen_height = height;
    if (s_size_class == SIZE_LARGE) {
        s_cfg.status_bar_height = 36;
        s_cfg.nav_bar_height = 56;
    } else if (s_size_class == SIZE_MEDIUM) {
        s_cfg.status_bar_height = 24;
        s_cfg.nav_bar_height = 48;
    } else {
        s_cfg.status_bar_height = 16;
        s_cfg.nav_bar_height = 32;
    }

    // Apply the Tritium cyberpunk theme
    tritium_theme::apply();

    lv_obj_t* screen = lv_screen_active();

    // Remove default scrolling on the screen object
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Build the widget tree
    create_status_bar(screen);
    create_viewport(screen);
    create_nav_bar(screen);
    create_toast_container(screen);
    create_notification_shade(screen);
    create_gesture_zones(screen);

    // Reset state
    s_app_count = 0;
    s_active_app = -1;
    memset(s_toasts, 0, sizeof(s_toasts));
    memset(s_notifs, 0, sizeof(s_notifs));

    // Register built-in system apps
    // About, Brightness, WiFi, Power are now sub-panels inside Settings.
    // The launcher IS the home screen — no need for a separate Launcher app icon.
    registerApp({"Settings", "System settings", LV_SYMBOL_SETTINGS, true,
                  shell_apps::settings_create, true});

    // Initialize lock screen (loads stored PIN from NVS)
    lock_screen::init();

    // Subscribe to key events for toast + shade notifications
#if EVENTS_AVAILABLE
    auto& bus = TritiumEventBus::instance();
    bus.subscribe(EVT_WIFI_CONNECTED, [](const TritiumEvent&, void*) {
        toast("WiFi connected", NOTIFY_SUCCESS);
    }, nullptr);
    bus.subscribe(EVT_WIFI_DISCONNECTED, [](const TritiumEvent&, void*) {
        toast("WiFi disconnected", NOTIFY_WARNING);
        notify("WiFi", "Connection lost", NOTIFY_WARNING);
    }, nullptr);
    bus.subscribe(EVT_MESH_PEER_JOINED, [](const TritiumEvent&, void*) {
        toast("Mesh peer joined", NOTIFY_INFO);
    }, nullptr);
    bus.subscribe(EVT_MESH_PEER_LEFT, [](const TritiumEvent&, void*) {
        toast("Mesh peer left", NOTIFY_WARNING);
    }, nullptr);
    bus.subscribe(EVT_POWER_LOW_BATTERY, [](const TritiumEvent&, void*) {
        toast("Low battery!", NOTIFY_ERROR);
        notify("Power", "Battery low — connect charger", NOTIFY_ERROR);
    }, nullptr);
    bus.subscribe(EVT_POWER_USB_CONNECT, [](const TritiumEvent&, void*) {
        toast("USB connected", NOTIFY_INFO);
    }, nullptr);
#endif

    // Don't show launcher yet — caller registers additional apps first,
    // then calls showLauncher() to build the grid with all apps visible.
    return true;
}

void tick() {
    uint32_t now = millis();

    // Auto-hide nav bar after timeout (only if not persistent — i.e., on launcher)
    if (s_nav_visible && !s_nav_persistent &&
        (now - s_nav_show_time > NAV_AUTO_HIDE_MS)) {
        hideNavBar();
    }

    // Process toast expirations
    for (int i = 0; i < MAX_TOASTS; i++) {
        if (s_toasts[i].obj && now >= s_toasts[i].expire_time) {
            lv_obj_delete(s_toasts[i].obj);
            s_toasts[i].obj = nullptr;
        }
    }

    // Lock screen tick (updates clock, handles lockout timer)
    lock_screen::tick();

    // Clock colon blink — toggles ":" visibility every second for alive feel.
    // Only modifies the text, LVGL handles the minimal redraw internally.
    if (s_clock_label && (now - s_clock_blink_time >= CLOCK_BLINK_MS)) {
        s_clock_blink_time = now;
        s_clock_colon_visible = !s_clock_colon_visible;
        const char* text = lv_label_get_text(s_clock_label);
        if (text && strlen(text) >= 5) {
            // Replace colon with space or vice versa
            static char buf[8];
            strncpy(buf, text, 7);
            buf[7] = '\0';
            buf[2] = s_clock_colon_visible ? ':' : ' ';
            lv_label_set_text(s_clock_label, buf);
        }
    }
}

// --- Status bar updates ---------------------------------------------------

void setWifiStatus(bool connected, int8_t rssi) {
    if (!s_wifi_icon) return;

    if (connected) {
        lv_obj_set_style_text_color(s_wifi_icon, T_CYAN, 0);
        if (s_rssi_label) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", rssi);
            lv_label_set_text(s_rssi_label, buf);
            lv_obj_set_style_text_color(s_rssi_label, T_TEXT, 0);
        }
    } else {
        lv_obj_set_style_text_color(s_wifi_icon, T_GHOST, 0);
        if (s_rssi_label) {
            lv_label_set_text(s_rssi_label, "--");
            lv_obj_set_style_text_color(s_rssi_label, T_GHOST, 0);
        }
    }
}

void setBleStatus(int device_count) {
    if (!s_ble_icon) return;

    lv_color_t color = (device_count > 0) ? T_CYAN
                                          : T_GHOST;
    lv_obj_set_style_text_color(s_ble_icon, color, 0);

    if (s_ble_count_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", device_count);
        lv_label_set_text(s_ble_count_label, buf);
        lv_obj_set_style_text_color(s_ble_count_label, color, 0);
    }
}

void setMeshStatus(int peer_count) {
    if (!s_mesh_icon) return;

    lv_color_t color = (peer_count > 0) ? T_GREEN
                                        : T_GHOST;
    lv_obj_set_style_text_color(s_mesh_icon, color, 0);

    if (s_mesh_count_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", peer_count);
        lv_label_set_text(s_mesh_count_label, buf);
        lv_obj_set_style_text_color(s_mesh_count_label, color, 0);
    }
}

void setBatteryStatus(int percent, bool charging) {
    if (!s_batt_icon) return;

    const char* symbol;
    lv_color_t color;

    if (charging) {
        symbol = LV_SYMBOL_CHARGE;
        color = T_CYAN;
    } else if (percent >= 75) {
        symbol = LV_SYMBOL_BATTERY_FULL;
        color = T_GREEN;
    } else if (percent >= 50) {
        symbol = LV_SYMBOL_BATTERY_3;
        color = T_GREEN;
    } else if (percent >= 25) {
        symbol = LV_SYMBOL_BATTERY_2;
        color = T_YELLOW;
    } else if (percent >= 10) {
        symbol = LV_SYMBOL_BATTERY_1;
        color = T_YELLOW;
    } else {
        symbol = LV_SYMBOL_BATTERY_EMPTY;
        color = T_MAGENTA;
    }

    lv_label_set_text(s_batt_icon, symbol);
    lv_obj_set_style_text_color(s_batt_icon, color, 0);

    if (s_batt_pct_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", percent);
        lv_label_set_text(s_batt_pct_label, buf);
        lv_obj_set_style_text_color(s_batt_pct_label, color, 0);
    }
}

void setClock(int hour, int minute) {
    if (!s_clock_label) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    lv_label_set_text(s_clock_label, buf);
}

void setAppName(const char* name) {
    if (s_app_name_label && name) {
        lv_label_set_text(s_app_name_label, name);
    }
}

// --- Notifications --------------------------------------------------------

void toast(const char* message, NotifyLevel level, uint32_t duration_ms) {
    if (!s_toast_container || !message) return;

    // Find a free slot (or evict oldest)
    int slot = -1;
    for (int i = 0; i < MAX_TOASTS; i++) {
        if (!s_toasts[i].obj) { slot = i; break; }
    }
    if (slot < 0) {
        // Remove oldest (slot 0) and shift down
        if (s_toasts[0].obj) {
            lv_obj_delete(s_toasts[0].obj);
        }
        for (int i = 0; i < MAX_TOASTS - 1; i++) {
            s_toasts[i] = s_toasts[i + 1];
        }
        slot = MAX_TOASTS - 1;
    }

    // Create toast widget inside the toast container
    lv_obj_t* t = lv_obj_create(s_toast_container);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_height(t, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(t, T_SURFACE3, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_90, 0);
    lv_obj_set_style_radius(t, 4, 0);
    lv_obj_set_style_pad_all(t, 6, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

    // Colored left border accent
    lv_color_t accent = color_for_level(level);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(t, 4, 0);
    lv_obj_set_style_border_color(t, accent, 0);

    // Message text
    lv_obj_t* lbl = lv_label_create(t);
    lv_label_set_text(lbl, message);
    lv_obj_set_style_text_color(lbl, T_TEXT, 0);
    lv_obj_set_style_text_font(lbl, font_for_size(s_size_class), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));

    s_toasts[slot].obj = t;
    s_toasts[slot].expire_time = millis() + duration_ms;
}

void notify(const char* title, const char* message, NotifyLevel level) {
    // Find free slot
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (!s_notifs[i].active) {
            strncpy(s_notifs[i].title, title ? title : "", 31);
            s_notifs[i].title[31] = '\0';
            strncpy(s_notifs[i].message, message ? message : "", 63);
            s_notifs[i].message[63] = '\0';
            s_notifs[i].level = level;
            s_notifs[i].active = true;
            return;
        }
    }
    // If full, overwrite oldest (index 0)
    strncpy(s_notifs[0].title, title ? title : "", 31);
    s_notifs[0].title[31] = '\0';
    strncpy(s_notifs[0].message, message ? message : "", 63);
    s_notifs[0].message[63] = '\0';
    s_notifs[0].level = level;
}

// --- App management -------------------------------------------------------

void registerApp(const AppDescriptor& app) {
    if (s_app_count >= MAX_APPS) return;
    s_apps[s_app_count++] = app;
}

void showLauncher() {
    if (!s_viewport) return;
    shell_apps::cleanup_timers();
    setAppName("TRITIUM OS v" TRITIUM_VERSION);
    s_active_app = 0;
    s_nav_persistent = false;
    hideNavBar();
    build_launcher_grid(s_viewport);
}

void showApp(int app_index) {
    if (app_index < 0 || app_index >= s_app_count) return;
    if (!s_apps[app_index].launch) {
        // No launch function (e.g., Launcher entry) -> show launcher
        showLauncher();
        return;
    }

    shell_apps::cleanup_timers();
    setAppName(s_apps[app_index].name);
    s_active_app = app_index;

    lv_obj_clean(s_viewport);

    // Show nav bar and size viewport
    s_nav_persistent = true;
    showNavBar();

    // Build app UI (viewport is now sized for nav bar)
    s_apps[app_index].launch(s_viewport);

    // Force viewport to exact bounds and invalidate the entire screen.
    // The RGB parallel panel (ST7262) uses direct framebuffer access, so
    // partial LVGL renders may leave stale pixels when the viewport shrinks.
    int vp_h = s_cfg.screen_height - s_cfg.status_bar_height - s_cfg.nav_bar_height;
    lv_obj_set_height(s_viewport, vp_h);
    lv_obj_set_style_height(s_viewport, vp_h, 0);
    lv_obj_set_style_clip_corner(s_viewport, true, 0);
    lv_obj_move_foreground(s_nav_bar);

    // Force complete redraw of the entire screen
    lv_obj_invalidate(lv_screen_active());

}

lv_obj_t* getViewport() {
    return s_viewport;
}

int getAppCount() { return s_app_count; }

const AppDescriptor* getApp(int index) {
    if (index < 0 || index >= s_app_count) return nullptr;
    return &s_apps[index];
}

int getActiveApp() { return s_active_app; }

// --- Navigation -----------------------------------------------------------

void showNavBar() {
    if (!s_nav_bar) return;
    lv_obj_remove_flag(s_nav_bar, LV_OBJ_FLAG_HIDDEN);
    s_nav_visible = true;
    s_nav_show_time = millis();

    // Shrink viewport to make room for nav bar.
    // Use both explicit size and style to ensure LVGL doesn't override
    // the height via flex layout or content sizing.
    int vp_h = s_cfg.screen_height - s_cfg.status_bar_height - s_cfg.nav_bar_height;
    lv_obj_set_height(s_viewport, vp_h);
    lv_obj_set_style_height(s_viewport, vp_h, 0);

    // Hide bottom gesture zone so it doesn't intercept nav bar clicks
    if (s_gesture_bottom) {
        lv_obj_add_flag(s_gesture_bottom, LV_OBJ_FLAG_HIDDEN);
    }

    // Raise nav bar to top of z-order so nothing overlaps it
    lv_obj_move_foreground(s_nav_bar);
}

void hideNavBar() {
    if (!s_nav_bar) return;
    lv_obj_add_flag(s_nav_bar, LV_OBJ_FLAG_HIDDEN);
    s_nav_visible = false;

    // Restore viewport to full height
    int vp_h = s_cfg.screen_height - s_cfg.status_bar_height;
    lv_obj_set_height(s_viewport, vp_h);
    lv_obj_set_style_height(s_viewport, vp_h, 0);

    // Restore bottom gesture zone
    if (s_gesture_bottom) {
        lv_obj_remove_flag(s_gesture_bottom, LV_OBJ_FLAG_HIDDEN);
    }
}

void showNotificationShade() {
    if (!s_notif_shade) return;

    // Rebuild shade content with current notifications
    lv_obj_clean(s_notif_shade);

    // Header
    lv_obj_t* header = lv_label_create(s_notif_shade);
    lv_label_set_text(header, "NOTIFICATIONS");
    lv_obj_set_style_text_color(header, T_BRIGHT, 0);
    lv_obj_set_style_text_font(header, uiHeadingFont(), 0);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_style_text_align(header, LV_TEXT_ALIGN_CENTER, 0);

    // List persistent notifications
    bool any = false;
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (!s_notifs[i].active) continue;
        any = true;

        lv_obj_t* card = tritium_theme::createPanel(s_notif_shade);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_border_color(card, color_for_level(s_notifs[i].level), 0);
        lv_obj_set_style_border_opa(card, LV_OPA_40, 0);

        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(card, 2, 0);

        lv_obj_t* t = tritium_theme::createLabel(card, s_notifs[i].title);
        lv_obj_set_style_text_color(t, T_BRIGHT, 0);

        tritium_theme::createLabel(card, s_notifs[i].message);
    }

    if (!any) {
        lv_obj_t* empty = lv_label_create(s_notif_shade);
        lv_label_set_text(empty, "No notifications");
        lv_obj_set_style_text_color(empty, T_GHOST, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, lv_pct(100));
    }

    // Close button
    lv_obj_t* close_btn = tritium_theme::createButton(s_notif_shade, "CLOSE");
    lv_obj_set_width(close_btn, lv_pct(50));
    lv_obj_add_event_cb(close_btn, shade_close_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_remove_flag(s_notif_shade, LV_OBJ_FLAG_HIDDEN);
    s_shade_visible = true;
}

void hideNotificationShade() {
    if (!s_notif_shade) return;
    lv_obj_add_flag(s_notif_shade, LV_OBJ_FLAG_HIDDEN);
    s_shade_visible = false;
}

}  // namespace tritium_shell
