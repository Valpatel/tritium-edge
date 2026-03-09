/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "shell_apps.h"
#include "shell_theme.h"
#include "display.h"
#include "tritium_splash.h"  // TRITIUM_VERSION
#include "os_settings.h"     // TritiumSettings, SettingsDomain
#include <cstdio>

#ifndef SIMULATOR
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#else
#include <SDL2/SDL.h>
static uint32_t millis() { return SDL_GetTicks(); }
#endif

// HAL includes must be outside namespace to avoid resolving std:: as shell_apps::std::
#if defined(ENABLE_WIFI) && __has_include("wifi_manager.h")
#include "wifi_manager.h"
#define WIFI_APP_AVAILABLE 1
#else
#define WIFI_APP_AVAILABLE 0
#endif

#if defined(ENABLE_DIAG) && __has_include("hal_diag.h")
#include "hal_diag.h"
#define SYSMON_AVAILABLE 1
#else
#define SYSMON_AVAILABLE 0
#endif

#if defined(ENABLE_MESH) && __has_include("mesh_manager.h")
#include "mesh_manager.h"
#define MESH_APP_AVAILABLE 1
#else
#define MESH_APP_AVAILABLE 0
#endif

#if defined(ENABLE_POWER) && __has_include("hal_power.h")
#include "hal_power.h"
#define POWER_APP_AVAILABLE 1
#else
#define POWER_APP_AVAILABLE 0
#endif

#if __has_include("hal_fs.h")
#include "hal_fs.h"
#define FILES_FS_AVAILABLE 1
#else
#define FILES_FS_AVAILABLE 0
#endif

#if __has_include("hal_sdcard.h")
#include "hal_sdcard.h"
#define FILES_SD_AVAILABLE 1
#else
#define FILES_SD_AVAILABLE 0
#endif

namespace shell_apps {

// ---------------------------------------------------------------------------
// Brightness slider callback
// ---------------------------------------------------------------------------

static void brightness_slider_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    display_set_brightness((uint8_t)val);

    // Update the value label (stored as user data on the slider)
    lv_obj_t* val_label = (lv_obj_t*)lv_event_get_user_data(e);
    if (val_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", (val * 100) / 255);
        lv_label_set_text(val_label, buf);
    }
}

// ---------------------------------------------------------------------------
// Reboot button callback
// ---------------------------------------------------------------------------

static void reboot_cb(lv_event_t* e) {
    (void)e;
#ifndef SIMULATOR
    esp_restart();
#endif
}

// ===========================================================================
//  WiFi App statics and callbacks (used by Settings WIFI tab)
// ===========================================================================

static lv_obj_t* s_wifi_status_ssid  = nullptr;
static lv_obj_t* s_wifi_status_ip    = nullptr;
static lv_obj_t* s_wifi_rssi_bar     = nullptr;
static lv_obj_t* s_wifi_scan_list    = nullptr;
static lv_obj_t* s_wifi_saved_list   = nullptr;
static lv_timer_t* s_wifi_timer      = nullptr;

#if WIFI_APP_AVAILABLE

static void wifi_refresh_status() {
    auto* wm = WifiManager::_instance;
    if (!wm) return;
    WifiStatus st = wm->getStatus();
    char buf[64];

    if (st.connected) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s", st.ssid);
        lv_label_set_text(s_wifi_status_ssid, buf);
        snprintf(buf, sizeof(buf), "IP: %s  CH: %d", st.ip, st.channel);
        lv_label_set_text(s_wifi_status_ip, buf);
        int pct = (st.rssi + 100);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_wifi_rssi_bar, pct, LV_ANIM_ON);
        lv_color_t c = (st.rssi > -50) ? T_GREEN : (st.rssi > -70) ? T_YELLOW : T_MAGENTA;
        lv_obj_set_style_bg_color(s_wifi_rssi_bar, c, LV_PART_INDICATOR);
    } else {
        lv_label_set_text(s_wifi_status_ssid, LV_SYMBOL_WIFI " Disconnected");
        lv_label_set_text(s_wifi_status_ip, "IP: --");
        lv_bar_set_value(s_wifi_rssi_bar, 0, LV_ANIM_OFF);
    }
}

static void wifi_timer_cb(lv_timer_t* t) {
    (void)t;
    wifi_refresh_status();
}

static void wifi_scan_cb(lv_event_t* e) {
    (void)e;
    auto* wm = WifiManager::_instance;
    if (!wm) return;
    wm->startScan();
    ScanResult results[WIFI_MAX_SCAN_RESULTS];
    int count = wm->getScanResults(results, WIFI_MAX_SCAN_RESULTS);
    lv_obj_clean(s_wifi_scan_list);
    for (int i = 0; i < count; i++) {
        lv_obj_t* row = lv_obj_create(s_wifi_scan_list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        const char* lock = (results[i].auth != WifiAuth::OPEN) ? LV_SYMBOL_EYE_CLOSE " " : "";
        char label[48];
        snprintf(label, sizeof(label), "%s%s", lock, results[i].ssid);
        lv_obj_t* name = tritium_theme::createLabel(row, label);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_style_text_color(name, results[i].known ? T_CYAN : T_TEXT, 0);

        char rssi_str[8];
        snprintf(rssi_str, sizeof(rssi_str), "%d", (int)results[i].rssi);
        lv_obj_t* rssi_lbl = tritium_theme::createLabel(row, rssi_str, true);
        int rpct = (results[i].rssi + 100);
        if (rpct < 0) rpct = 0;
        if (rpct > 100) rpct = 100;
        lv_color_t rc = (rpct > 60) ? T_GREEN : (rpct > 30) ? T_YELLOW : T_MAGENTA;
        lv_obj_set_style_text_color(rssi_lbl, rc, 0);
    }
}

static void wifi_ap_toggle_cb(lv_event_t* e) {
    auto* wm = WifiManager::_instance;
    if (!wm) return;
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        wm->startAP();
    } else {
        wm->stopAP();
    }
}

static void wifi_refresh_saved() {
    auto* wm = WifiManager::_instance;
    if (!wm || !s_wifi_saved_list) return;
    lv_obj_clean(s_wifi_saved_list);
    SavedNetwork nets[WIFI_MAX_SAVED_NETWORKS];
    int count = wm->getSavedNetworks(nets, WIFI_MAX_SAVED_NETWORKS);
    for (int i = 0; i < count; i++) {
        if (!nets[i].enabled && nets[i].ssid[0] == '\0') continue;
        lv_obj_t* row = lv_obj_create(s_wifi_saved_list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char lbl[48];
        snprintf(lbl, sizeof(lbl), "[%d] %s", nets[i].priority, nets[i].ssid);
        lv_obj_t* name = tritium_theme::createLabel(row, lbl, true);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_style_text_color(name, nets[i].enabled ? T_TEXT : T_GHOST, 0);

        lv_obj_t* del_btn = tritium_theme::createButton(row, LV_SYMBOL_TRASH, T_MAGENTA);
        lv_obj_set_user_data(del_btn, (void*)(uintptr_t)i);
    }
}

#endif  // WIFI_APP_AVAILABLE

// ===========================================================================
//  Power App statics and callbacks (used by Settings POWER tab)
// ===========================================================================

static lv_obj_t* s_power_pct_lbl     = nullptr;
static lv_obj_t* s_power_icon_lbl    = nullptr;
static lv_obj_t* s_power_source_lbl  = nullptr;
static lv_obj_t* s_power_volt_lbl    = nullptr;
static lv_obj_t* s_power_state_lbl   = nullptr;
static lv_obj_t* s_power_dim_slider  = nullptr;
static lv_obj_t* s_power_off_slider  = nullptr;
static lv_obj_t* s_power_dim_val     = nullptr;
static lv_obj_t* s_power_off_val     = nullptr;
static lv_timer_t* s_power_timer     = nullptr;

// Power profile buttons
static lv_obj_t* s_power_prof_btns[4] = {};
static int s_power_profile = 1;  // Default: Balanced

static void power_update(lv_timer_t* t) {
    (void)t;
#if POWER_APP_AVAILABLE
#if SYSMON_AVAILABLE
    hal_diag::HealthSnapshot snap = hal_diag::take_snapshot();
    char buf[48];

    int pct = (int)snap.battery_percent;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_power_pct_lbl, buf);

    lv_color_t c = (pct > 60) ? T_GREEN : (pct > 20) ? T_YELLOW : T_MAGENTA;
    lv_obj_set_style_text_color(s_power_pct_lbl, c, 0);
    lv_obj_set_style_text_color(s_power_icon_lbl, c, 0);

    const char* icon = (pct > 75) ? LV_SYMBOL_BATTERY_FULL :
                       (pct > 50) ? LV_SYMBOL_BATTERY_3 :
                       (pct > 25) ? LV_SYMBOL_BATTERY_2 :
                       (pct > 5)  ? LV_SYMBOL_BATTERY_1 :
                                    LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text(s_power_icon_lbl, icon);

    const char* src = (snap.power_source == 1) ? LV_SYMBOL_USB " USB" :
                      (snap.power_source == 2) ? LV_SYMBOL_BATTERY_FULL " Battery" :
                                                  "Unknown";
    lv_label_set_text(s_power_source_lbl, src);

    snprintf(buf, sizeof(buf), "%.2fV", snap.battery_voltage);
    lv_label_set_text(s_power_volt_lbl, buf);
#endif
#endif
}

static void power_profile_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_power_profile = idx;
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_border_color(s_power_prof_btns[i],
                                      (i == idx) ? T_CYAN : T_GHOST, 0);
        lv_obj_t* lbl = lv_obj_get_child(s_power_prof_btns[i], 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, (i == idx) ? T_CYAN : T_GHOST, 0);
        }
    }
    static const char* names[] = {"Performance", "Balanced", "Power Saver", "Auto"};
    char msg[32];
    snprintf(msg, sizeof(msg), "Profile: %s", names[idx]);
    tritium_shell::toast(msg, tritium_shell::NOTIFY_INFO, 1500);
}

static void power_dim_slider_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    char buf[16];
    if (val == 0) {
        snprintf(buf, sizeof(buf), "Never");
    } else {
        snprintf(buf, sizeof(buf), "%ds", val);
    }
    lv_label_set_text(s_power_dim_val, buf);
}

static void power_off_slider_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    char buf[16];
    if (val == 0) {
        snprintf(buf, sizeof(buf), "Never");
    } else {
        snprintf(buf, sizeof(buf), "%ds", val);
    }
    lv_label_set_text(s_power_off_val, buf);
}

// ---------------------------------------------------------------------------
// Settings Hub — tabbed interface with 5 categories
// ---------------------------------------------------------------------------

static lv_obj_t* s_settings_content = nullptr;
static lv_obj_t* s_settings_tab_btns[5] = {};
static int s_settings_active_tab = 0;

// Forward declarations for tab content builders
static void settings_build_display(lv_obj_t* cont);
static void settings_build_wifi(lv_obj_t* cont);
static void settings_build_power(lv_obj_t* cont);
static void settings_build_screensaver(lv_obj_t* cont);
static void settings_build_system(lv_obj_t* cont);

static void settings_select_tab(int idx) {
    if (!s_settings_content) return;
    s_settings_active_tab = idx;

    // Update tab button styling
    for (int i = 0; i < 5; i++) {
        if (!s_settings_tab_btns[i]) continue;
        bool active = (i == idx);
        lv_obj_set_style_border_color(s_settings_tab_btns[i],
                                      active ? T_CYAN : T_GHOST, 0);
        lv_obj_set_style_border_opa(s_settings_tab_btns[i],
                                    active ? LV_OPA_COVER : LV_OPA_20, 0);
        lv_obj_set_style_bg_color(s_settings_tab_btns[i],
                                  active ? T_SURFACE3 : T_SURFACE1, 0);
        lv_obj_set_style_bg_opa(s_settings_tab_btns[i],
                                active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t* lbl = lv_obj_get_child(s_settings_tab_btns[i], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, active ? T_CYAN : T_GHOST, 0);
    }

    // Rebuild content
    lv_obj_clean(s_settings_content);
    switch (idx) {
        case 0: settings_build_display(s_settings_content); break;
        case 1: settings_build_wifi(s_settings_content); break;
        case 2: settings_build_power(s_settings_content); break;
        case 3: settings_build_screensaver(s_settings_content); break;
        case 4: settings_build_system(s_settings_content); break;
    }
}

static void settings_tab_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    settings_select_tab(idx);
}

void settings_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);

    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 4, 0);
    lv_obj_set_style_pad_gap(viewport, 4, 0);

    // --- Tab bar: 5 icon buttons ---
    lv_obj_t* tab_bar = lv_obj_create(viewport);
    lv_obj_set_width(tab_bar, lv_pct(100));
    lv_obj_set_height(tab_bar, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 2, 0);
    lv_obj_set_style_pad_gap(tab_bar, 4, 0);
    lv_obj_set_flex_flow(tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(tab_bar, LV_OBJ_FLAG_SCROLLABLE);

    static const char* tab_icons[] = {
        LV_SYMBOL_EYE_OPEN,    // Display
        LV_SYMBOL_WIFI,        // WiFi
        LV_SYMBOL_BATTERY_FULL,// Power
        LV_SYMBOL_IMAGE,       // Screensaver
        LV_SYMBOL_SETTINGS,    // System
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t* btn = lv_btn_create(tab_bar);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, 36);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tab_icons[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, settings_tab_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);
        s_settings_tab_btns[i] = btn;
    }

    // --- Scrollable content area ---
    s_settings_content = lv_obj_create(viewport);
    lv_obj_set_width(s_settings_content, lv_pct(100));
    lv_obj_set_flex_grow(s_settings_content, 1);
    lv_obj_set_style_bg_opa(s_settings_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_settings_content, 0, 0);
    lv_obj_set_style_pad_all(s_settings_content, 0, 0);
    lv_obj_set_flex_flow(s_settings_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_settings_content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_settings_content, 8, 0);

    // Select first tab
    settings_select_tab(0);
}

// ---------------------------------------------------------------------------
// Settings Tab: DISPLAY
// ---------------------------------------------------------------------------

static void settings_build_display(lv_obj_t* cont) {
    lv_obj_t* panel = tritium_theme::createPanel(cont, "DISPLAY");
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(panel, 24, 0);
    lv_obj_set_style_pad_gap(panel, 6, 0);

    // Brightness slider
    lv_obj_t* bright_row = lv_obj_create(panel);
    lv_obj_set_size(bright_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bright_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bright_row, 0, 0);
    lv_obj_set_style_pad_all(bright_row, 0, 0);
    lv_obj_set_flex_flow(bright_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bright_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bright_row, LV_OBJ_FLAG_SCROLLABLE);

    tritium_theme::createLabel(bright_row, LV_SYMBOL_EYE_OPEN " Brightness");
    lv_obj_t* bright_val = tritium_theme::createLabel(bright_row, "100%", true);

    lv_obj_t* slider = tritium_theme::createSlider(panel, 10, 255, 255);
    lv_obj_set_width(slider, lv_pct(95));
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED,
                         bright_val);

    // Screen timeout dropdown
    lv_obj_t* timeout_row = lv_obj_create(panel);
    lv_obj_set_size(timeout_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(timeout_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(timeout_row, 0, 0);
    lv_obj_set_style_pad_all(timeout_row, 0, 0);
    lv_obj_set_flex_flow(timeout_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(timeout_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(timeout_row, LV_OBJ_FLAG_SCROLLABLE);

    tritium_theme::createLabel(timeout_row, LV_SYMBOL_BELL " Timeout");

    int vp_w = lv_obj_get_width(cont);
    lv_obj_t* dd = lv_dropdown_create(timeout_row);
    lv_dropdown_set_options(dd, "30s\n1m\n5m\nNever");
    lv_dropdown_set_selected(dd, 2);
    lv_obj_set_width(dd, (vp_w > 300) ? 100 : 70);
    lv_obj_set_style_bg_color(dd, T_SURFACE3, 0);
    lv_obj_set_style_text_color(dd, T_TEXT, 0);
    lv_obj_set_style_border_color(dd, T_CYAN, 0);
    lv_obj_set_style_border_opa(dd, LV_OPA_20, 0);
}

// ---------------------------------------------------------------------------
// Settings Tab: WIFI
// ---------------------------------------------------------------------------

static void settings_build_wifi(lv_obj_t* cont) {
#if !WIFI_APP_AVAILABLE
    tritium_theme::createLabel(cont, "WiFi HAL not available");
    return;
#else
    // --- Status panel ---
    lv_obj_t* status_panel = tritium_theme::createPanel(cont, "STATUS");
    lv_obj_set_width(status_panel, lv_pct(100));
    lv_obj_set_height(status_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(status_panel, 24, 0);
    lv_obj_set_style_pad_gap(status_panel, 4, 0);

    s_wifi_status_ssid = tritium_theme::createLabel(status_panel, LV_SYMBOL_WIFI " --", true);
    s_wifi_status_ip   = tritium_theme::createLabel(status_panel, "IP: --", true);

    s_wifi_rssi_bar = lv_bar_create(status_panel);
    lv_obj_set_width(s_wifi_rssi_bar, lv_pct(90));
    lv_obj_set_height(s_wifi_rssi_bar, 8);
    lv_bar_set_range(s_wifi_rssi_bar, 0, 100);
    lv_bar_set_value(s_wifi_rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_wifi_rssi_bar, T_SURFACE3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_wifi_rssi_bar, T_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_wifi_rssi_bar, 4, 0);
    lv_obj_set_style_radius(s_wifi_rssi_bar, 4, LV_PART_INDICATOR);

    // --- AP toggle ---
    lv_obj_t* ap_row = lv_obj_create(cont);
    lv_obj_set_size(ap_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ap_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ap_row, 0, 0);
    lv_obj_set_style_pad_all(ap_row, 0, 0);
    lv_obj_set_flex_flow(ap_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ap_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(ap_row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(ap_row, "AP MODE");
    lv_obj_t* ap_sw = tritium_theme::createSwitch(ap_row,
        WifiManager::_instance ? WifiManager::_instance->isAPActive() : false);
    lv_obj_add_event_cb(ap_sw, wifi_ap_toggle_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- Saved networks ---
    lv_obj_t* saved_panel = tritium_theme::createPanel(cont, "SAVED NETWORKS");
    lv_obj_set_width(saved_panel, lv_pct(100));
    lv_obj_set_height(saved_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(saved_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(saved_panel, 24, 0);
    lv_obj_set_style_pad_gap(saved_panel, 2, 0);
    lv_obj_set_style_max_height(saved_panel, 120, 0);

    s_wifi_saved_list = lv_obj_create(saved_panel);
    lv_obj_set_size(s_wifi_saved_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_wifi_saved_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_saved_list, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_saved_list, 0, 0);
    lv_obj_set_flex_flow(s_wifi_saved_list, LV_FLEX_FLOW_COLUMN);
    wifi_refresh_saved();

    // --- Scan section ---
    lv_obj_t* scan_btn = tritium_theme::createButton(cont, LV_SYMBOL_REFRESH " SCAN");
    lv_obj_set_width(scan_btn, lv_pct(50));
    lv_obj_add_event_cb(scan_btn, wifi_scan_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* scan_panel = tritium_theme::createPanel(cont, "SCAN RESULTS");
    lv_obj_set_width(scan_panel, lv_pct(100));
    lv_obj_set_height(scan_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(scan_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(scan_panel, 24, 0);
    lv_obj_set_style_max_height(scan_panel, 180, 0);

    s_wifi_scan_list = lv_obj_create(scan_panel);
    lv_obj_set_size(s_wifi_scan_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_wifi_scan_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_scan_list, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_scan_list, 0, 0);
    lv_obj_set_flex_flow(s_wifi_scan_list, LV_FLEX_FLOW_COLUMN);
    tritium_theme::createLabel(s_wifi_scan_list, "Tap SCAN to search", false);

    // Auto-refresh timer
    wifi_refresh_status();
    s_wifi_timer = lv_timer_create(wifi_timer_cb, 3000, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// Settings Tab: POWER
// ---------------------------------------------------------------------------

static void settings_build_power(lv_obj_t* cont) {
    // --- Battery display ---
    lv_obj_t* batt_panel = tritium_theme::createPanel(cont, "BATTERY");
    lv_obj_set_width(batt_panel, lv_pct(100));
    lv_obj_set_height(batt_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(batt_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(batt_panel, 24, 0);
    lv_obj_set_style_pad_gap(batt_panel, 4, 0);
    lv_obj_set_flex_align(batt_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_power_icon_lbl = lv_label_create(batt_panel);
    lv_label_set_text(s_power_icon_lbl, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(s_power_icon_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_power_icon_lbl, T_GREEN, 0);

    s_power_pct_lbl = lv_label_create(batt_panel);
    lv_label_set_text(s_power_pct_lbl, "--%");
    lv_obj_set_style_text_font(s_power_pct_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_power_pct_lbl, T_GREEN, 0);

    lv_obj_t* info_row = lv_obj_create(batt_panel);
    lv_obj_set_size(info_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_row, 0, 0);
    lv_obj_set_style_pad_all(info_row, 0, 0);
    lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(info_row, LV_OBJ_FLAG_SCROLLABLE);
    s_power_source_lbl = tritium_theme::createLabel(info_row, "Source: --", true);
    s_power_volt_lbl   = tritium_theme::createLabel(info_row, "--V", true);

    // --- Power Profile ---
    lv_obj_t* prof_panel = tritium_theme::createPanel(cont, "PROFILE");
    lv_obj_set_width(prof_panel, lv_pct(100));
    lv_obj_set_height(prof_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(prof_panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(prof_panel, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(prof_panel, 24, 0);

    static const char* prof_labels[] = {
        LV_SYMBOL_CHARGE, LV_SYMBOL_OK, LV_SYMBOL_DOWN, LV_SYMBOL_LOOP
    };
    for (int i = 0; i < 4; i++) {
        lv_color_t accent = (i == s_power_profile) ? T_CYAN : T_GHOST;
        s_power_prof_btns[i] = tritium_theme::createButton(prof_panel, prof_labels[i], accent);
        lv_obj_add_event_cb(s_power_prof_btns[i], power_profile_cb,
                            LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    lv_obj_t* legend = tritium_theme::createLabel(cont, "PERF | BAL | SAVE | AUTO", true);
    lv_obj_set_style_text_color(legend, T_GHOST, 0);
    lv_obj_set_width(legend, lv_pct(100));
    lv_obj_set_style_text_align(legend, LV_TEXT_ALIGN_CENTER, 0);

    // --- Timeouts ---
    lv_obj_t* timeout_panel = tritium_theme::createPanel(cont, "TIMEOUTS");
    lv_obj_set_width(timeout_panel, lv_pct(100));
    lv_obj_set_height(timeout_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(timeout_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(timeout_panel, 24, 0);
    lv_obj_set_style_pad_gap(timeout_panel, 6, 0);

    // Screen dim
    lv_obj_t* dim_row = lv_obj_create(timeout_panel);
    lv_obj_set_size(dim_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dim_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dim_row, 0, 0);
    lv_obj_set_style_pad_all(dim_row, 0, 0);
    lv_obj_set_flex_flow(dim_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dim_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(dim_row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(dim_row, "Dim");
    s_power_dim_val = tritium_theme::createLabel(dim_row, "30s", true);

    s_power_dim_slider = tritium_theme::createSlider(timeout_panel, 0, 300, 30);
    lv_obj_set_width(s_power_dim_slider, lv_pct(95));
    lv_obj_add_event_cb(s_power_dim_slider, power_dim_slider_cb,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    // Screen off
    lv_obj_t* off_row = lv_obj_create(timeout_panel);
    lv_obj_set_size(off_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(off_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(off_row, 0, 0);
    lv_obj_set_style_pad_all(off_row, 0, 0);
    lv_obj_set_flex_flow(off_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(off_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(off_row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(off_row, "Screen Off");
    s_power_off_val = tritium_theme::createLabel(off_row, "60s", true);

    s_power_off_slider = tritium_theme::createSlider(timeout_panel, 0, 600, 60);
    lv_obj_set_width(s_power_off_slider, lv_pct(95));
    lv_obj_add_event_cb(s_power_off_slider, power_off_slider_cb,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    // Reboot button
    lv_obj_t* reboot_btn = tritium_theme::createButton(
        cont, LV_SYMBOL_POWER " REBOOT", T_MAGENTA);
    lv_obj_set_width(reboot_btn, lv_pct(60));
    lv_obj_add_event_cb(reboot_btn, reboot_cb, LV_EVENT_CLICKED, nullptr);

    // Auto-refresh battery
    power_update(nullptr);
    s_power_timer = lv_timer_create(power_update, 3000, nullptr);
}

// ---------------------------------------------------------------------------
// Settings Tab: SCREENSAVER
// ---------------------------------------------------------------------------

static void ss_reverse_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    TritiumSettings::instance().setBool(SettingsDomain::SCREENSAVER, "sf_reverse", checked);
}

static void ss_colors_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    TritiumSettings::instance().setBool(SettingsDomain::SCREENSAVER, "sf_colors", checked);
}

static void ss_starsize_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    TritiumSettings::instance().setInt(SettingsDomain::SCREENSAVER, "sf_star_size", val);
    lv_obj_t* lbl = (lv_obj_t*)lv_event_get_user_data(e);
    if (lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%dpx", val);
        lv_label_set_text(lbl, buf);
    }
}

static void ss_timeout_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    TritiumSettings::instance().setInt(SettingsDomain::SCREENSAVER, "timeout_s", val);
    lv_obj_t* lbl = (lv_obj_t*)lv_event_get_user_data(e);
    if (lbl) {
        char buf[16];
        if (val == 0)
            snprintf(buf, sizeof(buf), "Never");
        else if (val < 60)
            snprintf(buf, sizeof(buf), "%ds", val);
        else
            snprintf(buf, sizeof(buf), "%dm", val / 60);
        lv_label_set_text(lbl, buf);
    }
}

static void settings_build_screensaver(lv_obj_t* cont) {
    auto& cfg = TritiumSettings::instance();

    // --- General ---
    lv_obj_t* gen_panel = tritium_theme::createPanel(cont, "SCREENSAVER");
    lv_obj_set_width(gen_panel, lv_pct(100));
    lv_obj_set_height(gen_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(gen_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(gen_panel, 24, 0);
    lv_obj_set_style_pad_gap(gen_panel, 6, 0);

    // Type label (currently only starfield)
    char type_buf[48];
    snprintf(type_buf, sizeof(type_buf), "Type: %s",
             cfg.getString(SettingsDomain::SCREENSAVER, "type", "starfield"));
    tritium_theme::createLabel(gen_panel, type_buf, true);

    // Timeout slider
    lv_obj_t* timeout_row = lv_obj_create(gen_panel);
    lv_obj_set_size(timeout_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(timeout_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(timeout_row, 0, 0);
    lv_obj_set_style_pad_all(timeout_row, 0, 0);
    lv_obj_set_flex_flow(timeout_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(timeout_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(timeout_row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(timeout_row, "Timeout");

    int timeout_s = cfg.getInt(SettingsDomain::SCREENSAVER, "timeout_s", 120);
    char timeout_str[16];
    if (timeout_s == 0)
        snprintf(timeout_str, sizeof(timeout_str), "Never");
    else if (timeout_s < 60)
        snprintf(timeout_str, sizeof(timeout_str), "%ds", timeout_s);
    else
        snprintf(timeout_str, sizeof(timeout_str), "%dm", timeout_s / 60);
    lv_obj_t* timeout_val = tritium_theme::createLabel(timeout_row, timeout_str, true);

    lv_obj_t* timeout_slider = tritium_theme::createSlider(gen_panel, 0, 600, timeout_s);
    lv_obj_set_width(timeout_slider, lv_pct(95));
    lv_obj_add_event_cb(timeout_slider, ss_timeout_cb, LV_EVENT_VALUE_CHANGED, timeout_val);

    // --- Starfield options ---
    lv_obj_t* sf_panel = tritium_theme::createPanel(cont, "STARFIELD OPTIONS");
    lv_obj_set_width(sf_panel, lv_pct(100));
    lv_obj_set_height(sf_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sf_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(sf_panel, 24, 0);
    lv_obj_set_style_pad_gap(sf_panel, 6, 0);

    // Reverse direction toggle
    lv_obj_t* rev_row = lv_obj_create(sf_panel);
    lv_obj_set_size(rev_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(rev_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rev_row, 0, 0);
    lv_obj_set_style_pad_all(rev_row, 0, 0);
    lv_obj_set_flex_flow(rev_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rev_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(rev_row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(rev_row, "Reverse Direction");
    bool sf_reverse = cfg.getBool(SettingsDomain::SCREENSAVER, "sf_reverse", false);
    lv_obj_t* rev_sw = tritium_theme::createSwitch(rev_row, sf_reverse);
    lv_obj_add_event_cb(rev_sw, ss_reverse_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Colors toggle
    lv_obj_t* col_row = lv_obj_create(sf_panel);
    lv_obj_set_size(col_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col_row, 0, 0);
    lv_obj_set_style_pad_all(col_row, 0, 0);
    lv_obj_set_flex_flow(col_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(col_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col_row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(col_row, "Colored Stars");
    bool sf_colors = cfg.getBool(SettingsDomain::SCREENSAVER, "sf_colors", true);
    lv_obj_t* col_sw = tritium_theme::createSwitch(col_row, sf_colors);
    lv_obj_add_event_cb(col_sw, ss_colors_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Star size slider
    lv_obj_t* size_row = lv_obj_create(sf_panel);
    lv_obj_set_size(size_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(size_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(size_row, 0, 0);
    lv_obj_set_style_pad_all(size_row, 0, 0);
    lv_obj_set_flex_flow(size_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(size_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(size_row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(size_row, "Star Size");

    int sf_size = cfg.getInt(SettingsDomain::SCREENSAVER, "sf_star_size", 2);
    char size_str[8];
    snprintf(size_str, sizeof(size_str), "%dpx", sf_size);
    lv_obj_t* size_val = tritium_theme::createLabel(size_row, size_str, true);

    lv_obj_t* size_slider = tritium_theme::createSlider(sf_panel, 1, 6, sf_size);
    lv_obj_set_width(size_slider, lv_pct(95));
    lv_obj_add_event_cb(size_slider, ss_starsize_cb, LV_EVENT_VALUE_CHANGED, size_val);
}

// ---------------------------------------------------------------------------
// Settings Tab: SYSTEM (merged About + system info)
// ---------------------------------------------------------------------------

static void settings_build_system(lv_obj_t* cont) {
    // --- About section ---
    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text(title, "TRITIUM-OS");
    lv_obj_set_style_text_color(title, T_CYAN, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, lv_pct(100));

    lv_obj_t* tagline = lv_label_create(cont);
    lv_label_set_text(tagline, "Software-Defined Edge Intelligence");
    lv_obj_set_style_text_color(tagline, T_GHOST, 0);
    lv_obj_set_style_text_font(tagline, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(tagline, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(tagline, lv_pct(100));

    // --- Info panel ---
    lv_obj_t* info = tritium_theme::createPanel(cont, "SYSTEM INFO");
    lv_obj_set_width(info, lv_pct(100));
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(info, 24, 0);
    lv_obj_set_style_pad_gap(info, 4, 0);

    char buf[80];

    snprintf(buf, sizeof(buf), "Version: %s", TRITIUM_VERSION);
    tritium_theme::createLabel(info, buf, true);

    const display_health_t* dh = display_get_health();
    snprintf(buf, sizeof(buf), "Board: %s", dh ? dh->board_name : "Unknown");
    tritium_theme::createLabel(info, buf, true);

    snprintf(buf, sizeof(buf), "Display: %s %dx%d",
             dh ? dh->driver : "?",
             display_get_width(), display_get_height());
    tritium_theme::createLabel(info, buf, true);

#ifndef SIMULATOR
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(buf, sizeof(buf), "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    tritium_theme::createLabel(info, buf, true);

    snprintf(buf, sizeof(buf), "Heap: %uKB free",
             (unsigned)(esp_get_free_heap_size() / 1024));
    tritium_theme::createLabel(info, buf, true);

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free > 0) {
        snprintf(buf, sizeof(buf), "PSRAM: %.1fMB free",
                 (float)psram_free / (1024.0f * 1024.0f));
        tritium_theme::createLabel(info, buf, true);
    }

    uint32_t secs = millis() / 1000;
    uint32_t mins = secs / 60;
    uint32_t hrs = mins / 60;
    snprintf(buf, sizeof(buf), "Uptime: %uh %um %us",
             (unsigned)hrs, (unsigned)(mins % 60), (unsigned)(secs % 60));
    tritium_theme::createLabel(info, buf, true);
#endif

    // --- Reboot ---
    lv_obj_t* reboot_btn = tritium_theme::createButton(
        cont, LV_SYMBOL_POWER " REBOOT", T_MAGENTA);
    lv_obj_set_width(reboot_btn, lv_pct(60));
    lv_obj_add_event_cb(reboot_btn, reboot_cb, LV_EVENT_CLICKED, nullptr);

    // --- Copyright ---
    lv_obj_t* copy = lv_label_create(cont);
    lv_label_set_text(copy, "2026 Valpatel Software LLC");
    lv_obj_set_style_text_color(copy, T_GHOST, 0);
    lv_obj_set_style_text_font(copy, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(copy, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(copy, lv_pct(100));
}

// ---------------------------------------------------------------------------
// About App
// ---------------------------------------------------------------------------

void about_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);

    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 12, 0);
    lv_obj_set_style_pad_gap(viewport, 8, 0);

    // Logo / title
    lv_obj_t* title = lv_label_create(viewport);
    lv_label_set_text(title, "TRITIUM-OS");
    lv_obj_set_style_text_color(title, T_CYAN, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, lv_pct(100));

    // Tagline
    lv_obj_t* tagline = lv_label_create(viewport);
    lv_label_set_text(tagline, "Software-Defined Edge Intelligence");
    lv_obj_set_style_text_color(tagline, T_GHOST, 0);
    lv_obj_set_style_text_font(tagline, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(tagline, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(tagline, lv_pct(100));

    // Separator
    lv_obj_t* sep = lv_obj_create(viewport);
    lv_obj_set_size(sep, lv_pct(80), 1);
    lv_obj_set_style_bg_color(sep, T_CYAN, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_20, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    // Info panel
    lv_obj_t* info = tritium_theme::createPanel(viewport);
    lv_obj_set_width(info, lv_pct(100));
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(info, 4, 0);

    char buf[80];

    // Version
    snprintf(buf, sizeof(buf), "Version: %s", TRITIUM_VERSION);
    tritium_theme::createLabel(info, buf, true);

    // Board
    const display_health_t* dh = display_get_health();
    snprintf(buf, sizeof(buf), "Board: %s", dh ? dh->board_name : "Unknown");
    tritium_theme::createLabel(info, buf, true);

    // Display
    snprintf(buf, sizeof(buf), "Display: %s %dx%d",
             dh ? dh->driver : "?",
             display_get_width(), display_get_height());
    tritium_theme::createLabel(info, buf, true);

#ifndef SIMULATOR
    // MAC address
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(buf, sizeof(buf), "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    tritium_theme::createLabel(info, buf, true);

    // Memory stats
    snprintf(buf, sizeof(buf), "Heap: %uKB free",
             (unsigned)(esp_get_free_heap_size() / 1024));
    tritium_theme::createLabel(info, buf, true);

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free > 0) {
        snprintf(buf, sizeof(buf), "PSRAM: %.1fMB free",
                 (float)psram_free / (1024.0f * 1024.0f));
        tritium_theme::createLabel(info, buf, true);
    }

    // Uptime
    uint32_t secs = millis() / 1000;
    uint32_t mins = secs / 60;
    uint32_t hrs = mins / 60;
    snprintf(buf, sizeof(buf), "Uptime: %uh %um %us",
             (unsigned)hrs, (unsigned)(mins % 60), (unsigned)(secs % 60));
    tritium_theme::createLabel(info, buf, true);
#endif

    // Copyright
    lv_obj_t* copy = lv_label_create(viewport);
    lv_label_set_text(copy, "2026 Valpatel Software LLC");
    lv_obj_set_style_text_color(copy, T_GHOST, 0);
    lv_obj_set_style_text_font(copy, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(copy, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(copy, lv_pct(100));
}

// ---------------------------------------------------------------------------
// Brightness App
// ---------------------------------------------------------------------------

void brightness_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);

    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 16, 0);
    lv_obj_set_style_pad_gap(viewport, 16, 0);

    // Icon
    lv_obj_t* icon = lv_label_create(viewport);
    lv_label_set_text(icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(icon, T_CYAN, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);

    // Value label
    lv_obj_t* val_label = tritium_theme::createLabel(viewport, "100%", true);
    lv_obj_set_style_text_font(val_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(val_label, T_BRIGHT, 0);

    // Slider
    lv_obj_t* slider = tritium_theme::createSlider(viewport, 10, 255, 255);
    lv_obj_set_width(slider, lv_pct(85));
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED,
                         val_label);
}

// ===========================================================================
//  WiFi Manager App (standalone — kept for API compatibility)
// ===========================================================================

void wifi_app_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 8, 0);
    lv_obj_set_style_pad_gap(viewport, 6, 0);

#if !WIFI_APP_AVAILABLE
    tritium_theme::createLabel(viewport, "WiFi HAL not available");
    return;
#else
    // --- Status panel ---
    lv_obj_t* status_panel = tritium_theme::createPanel(viewport, "STATUS");
    lv_obj_set_width(status_panel, lv_pct(100));
    lv_obj_set_height(status_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(status_panel, 24, 0);
    lv_obj_set_style_pad_gap(status_panel, 4, 0);

    s_wifi_status_ssid = tritium_theme::createLabel(status_panel, LV_SYMBOL_WIFI " --", true);
    s_wifi_status_ip   = tritium_theme::createLabel(status_panel, "IP: --", true);

    s_wifi_rssi_bar = lv_bar_create(status_panel);
    lv_obj_set_width(s_wifi_rssi_bar, lv_pct(90));
    lv_obj_set_height(s_wifi_rssi_bar, 8);
    lv_bar_set_range(s_wifi_rssi_bar, 0, 100);
    lv_bar_set_value(s_wifi_rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_wifi_rssi_bar, T_SURFACE3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_wifi_rssi_bar, T_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_wifi_rssi_bar, 4, 0);
    lv_obj_set_style_radius(s_wifi_rssi_bar, 4, LV_PART_INDICATOR);

    // --- AP toggle ---
    lv_obj_t* ap_row = lv_obj_create(viewport);
    lv_obj_set_size(ap_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ap_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ap_row, 0, 0);
    lv_obj_set_style_pad_all(ap_row, 0, 0);
    lv_obj_set_flex_flow(ap_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ap_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(ap_row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(ap_row, "AP MODE");
    lv_obj_t* ap_sw = tritium_theme::createSwitch(ap_row,
        WifiManager::_instance ? WifiManager::_instance->isAPActive() : false);
    lv_obj_add_event_cb(ap_sw, wifi_ap_toggle_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- Saved networks ---
    lv_obj_t* saved_panel = tritium_theme::createPanel(viewport, "SAVED NETWORKS");
    lv_obj_set_width(saved_panel, lv_pct(100));
    lv_obj_set_height(saved_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(saved_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(saved_panel, 24, 0);
    lv_obj_set_style_pad_gap(saved_panel, 2, 0);
    lv_obj_set_style_max_height(saved_panel, 120, 0);

    s_wifi_saved_list = lv_obj_create(saved_panel);
    lv_obj_set_size(s_wifi_saved_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_wifi_saved_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_saved_list, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_saved_list, 0, 0);
    lv_obj_set_flex_flow(s_wifi_saved_list, LV_FLEX_FLOW_COLUMN);
    wifi_refresh_saved();

    // --- Scan section ---
    lv_obj_t* scan_btn = tritium_theme::createButton(viewport, LV_SYMBOL_REFRESH " SCAN");
    lv_obj_set_width(scan_btn, lv_pct(50));
    lv_obj_add_event_cb(scan_btn, wifi_scan_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* scan_panel = tritium_theme::createPanel(viewport, "SCAN RESULTS");
    lv_obj_set_width(scan_panel, lv_pct(100));
    lv_obj_set_height(scan_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(scan_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(scan_panel, 24, 0);
    lv_obj_set_style_max_height(scan_panel, 180, 0);

    s_wifi_scan_list = lv_obj_create(scan_panel);
    lv_obj_set_size(s_wifi_scan_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_wifi_scan_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_scan_list, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_scan_list, 0, 0);
    lv_obj_set_flex_flow(s_wifi_scan_list, LV_FLEX_FLOW_COLUMN);

    tritium_theme::createLabel(s_wifi_scan_list, "Tap SCAN to search", false);

    // Auto-refresh timer
    wifi_refresh_status();
    s_wifi_timer = lv_timer_create(wifi_timer_cb, 3000, nullptr);
#endif
}

// ===========================================================================
//  System Monitor App
// ===========================================================================

static lv_obj_t* s_sysmon_heap_bar   = nullptr;
static lv_obj_t* s_sysmon_heap_lbl   = nullptr;
static lv_obj_t* s_sysmon_psram_bar  = nullptr;
static lv_obj_t* s_sysmon_psram_lbl  = nullptr;
static lv_obj_t* s_sysmon_loop_lbl   = nullptr;
static lv_obj_t* s_sysmon_uptime_lbl = nullptr;
static lv_obj_t* s_sysmon_temp_lbl   = nullptr;
static lv_obj_t* s_sysmon_wifi_lbl   = nullptr;
static lv_obj_t* s_sysmon_tasks_lbl  = nullptr;
static lv_timer_t* s_sysmon_timer    = nullptr;

static void sysmon_update(lv_timer_t* t) {
    (void)t;
#ifndef SIMULATOR
    // Heap (internal DRAM only, excluding PSRAM)
    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (total_heap == 0) total_heap = 1;  // avoid div-by-zero
    uint32_t used_heap = (free_heap < total_heap) ? total_heap - free_heap : 0;
    int heap_pct = (int)((used_heap * 100) / total_heap);
    char buf[64];
    snprintf(buf, sizeof(buf), "Heap: %uKB / %uKB (%d%%)",
             (unsigned)(free_heap / 1024), (unsigned)(total_heap / 1024), heap_pct);
    lv_label_set_text(s_sysmon_heap_lbl, buf);
    lv_bar_set_value(s_sysmon_heap_bar, heap_pct, LV_ANIM_OFF);
    lv_color_t hc = (heap_pct < 60) ? T_GREEN : (heap_pct < 85) ? T_YELLOW : T_MAGENTA;
    lv_obj_set_style_bg_color(s_sysmon_heap_bar, hc, LV_PART_INDICATOR);

    // PSRAM
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = 8 * 1024 * 1024;  // 8MB
    size_t used_psram = (free_psram < total_psram) ? total_psram - free_psram : 0;
    int psram_pct = (int)((used_psram * 100) / total_psram);
    snprintf(buf, sizeof(buf), "PSRAM: %.1fMB / 8MB (%d%%)",
             (float)used_psram / (1024.0f * 1024.0f), psram_pct);
    lv_label_set_text(s_sysmon_psram_lbl, buf);
    lv_bar_set_value(s_sysmon_psram_bar, psram_pct, LV_ANIM_OFF);
    lv_color_t pc = (psram_pct < 60) ? T_GREEN : (psram_pct < 85) ? T_YELLOW : T_MAGENTA;
    lv_obj_set_style_bg_color(s_sysmon_psram_bar, pc, LV_PART_INDICATOR);

    // Uptime
    uint32_t secs = millis() / 1000;
    uint32_t hrs = secs / 3600;
    uint32_t mins = (secs % 3600) / 60;
    snprintf(buf, sizeof(buf), "Uptime: %uh %um %us",
             (unsigned)hrs, (unsigned)mins, (unsigned)(secs % 60));
    lv_label_set_text(s_sysmon_uptime_lbl, buf);

    // Task count
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    snprintf(buf, sizeof(buf), "Tasks: %u", (unsigned)task_count);
    lv_label_set_text(s_sysmon_tasks_lbl, buf);

#if SYSMON_AVAILABLE
    hal_diag::HealthSnapshot snap = hal_diag::take_snapshot();

    snprintf(buf, sizeof(buf), "Loop: %.1fms (max %.1fms)",
             snap.loop_time_us / 1000.0f, snap.max_loop_time_us / 1000.0f);
    lv_label_set_text(s_sysmon_loop_lbl, buf);

    snprintf(buf, sizeof(buf), "CPU Temp: %.1f C", snap.cpu_temp_c);
    lv_label_set_text(s_sysmon_temp_lbl, buf);
    lv_color_t tc = (snap.cpu_temp_c < 60.0f) ? T_GREEN :
                    (snap.cpu_temp_c < 80.0f) ? T_YELLOW : T_MAGENTA;
    lv_obj_set_style_text_color(s_sysmon_temp_lbl, tc, 0);

    snprintf(buf, sizeof(buf), "WiFi: %ddBm  Drops: %u",
             (int)snap.wifi_rssi, (unsigned)snap.wifi_disconnects);
    lv_label_set_text(s_sysmon_wifi_lbl, buf);
#else
    lv_label_set_text(s_sysmon_loop_lbl, "Loop: --");
    lv_label_set_text(s_sysmon_temp_lbl, "CPU Temp: --");
    lv_label_set_text(s_sysmon_wifi_lbl, "WiFi: --");
#endif
#endif  // SIMULATOR
}

void sysmon_app_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 8, 0);
    lv_obj_set_style_pad_gap(viewport, 6, 0);

#ifdef SIMULATOR
    tritium_theme::createLabel(viewport, "System Monitor not available in simulator");
    return;
#else
    // --- Memory panel ---
    lv_obj_t* mem_panel = tritium_theme::createPanel(viewport, "MEMORY");
    lv_obj_set_width(mem_panel, lv_pct(100));
    lv_obj_set_height(mem_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mem_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(mem_panel, 24, 0);
    lv_obj_set_style_pad_gap(mem_panel, 4, 0);

    s_sysmon_heap_lbl = tritium_theme::createLabel(mem_panel, "Heap: --", true);
    s_sysmon_heap_bar = lv_bar_create(mem_panel);
    lv_obj_set_width(s_sysmon_heap_bar, lv_pct(95));
    lv_obj_set_height(s_sysmon_heap_bar, 10);
    lv_bar_set_range(s_sysmon_heap_bar, 0, 100);
    lv_obj_set_style_bg_color(s_sysmon_heap_bar, T_SURFACE3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sysmon_heap_bar, T_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_sysmon_heap_bar, 4, 0);
    lv_obj_set_style_radius(s_sysmon_heap_bar, 4, LV_PART_INDICATOR);

    s_sysmon_psram_lbl = tritium_theme::createLabel(mem_panel, "PSRAM: --", true);
    s_sysmon_psram_bar = lv_bar_create(mem_panel);
    lv_obj_set_width(s_sysmon_psram_bar, lv_pct(95));
    lv_obj_set_height(s_sysmon_psram_bar, 10);
    lv_bar_set_range(s_sysmon_psram_bar, 0, 100);
    lv_obj_set_style_bg_color(s_sysmon_psram_bar, T_SURFACE3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sysmon_psram_bar, T_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_sysmon_psram_bar, 4, 0);
    lv_obj_set_style_radius(s_sysmon_psram_bar, 4, LV_PART_INDICATOR);

    // --- CPU panel ---
    lv_obj_t* cpu_panel = tritium_theme::createPanel(viewport, "CPU");
    lv_obj_set_width(cpu_panel, lv_pct(100));
    lv_obj_set_height(cpu_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cpu_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(cpu_panel, 24, 0);
    lv_obj_set_style_pad_gap(cpu_panel, 4, 0);

    s_sysmon_loop_lbl   = tritium_theme::createLabel(cpu_panel, "Loop: --", true);
    s_sysmon_temp_lbl   = tritium_theme::createLabel(cpu_panel, "CPU Temp: --", true);
    s_sysmon_uptime_lbl = tritium_theme::createLabel(cpu_panel, "Uptime: --", true);
    s_sysmon_tasks_lbl  = tritium_theme::createLabel(cpu_panel, "Tasks: --", true);

    // --- Network panel ---
    lv_obj_t* net_panel = tritium_theme::createPanel(viewport, "NETWORK");
    lv_obj_set_width(net_panel, lv_pct(100));
    lv_obj_set_height(net_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(net_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(net_panel, 24, 0);
    lv_obj_set_style_pad_gap(net_panel, 4, 0);

    s_sysmon_wifi_lbl = tritium_theme::createLabel(net_panel, "WiFi: --", true);

    // Initial update + timer
    sysmon_update(nullptr);
    s_sysmon_timer = lv_timer_create(sysmon_update, 2000, nullptr);
#endif
}

// ===========================================================================
//  Mesh Viewer App
// ===========================================================================

static lv_obj_t* s_mesh_role_lbl    = nullptr;
static lv_obj_t* s_mesh_peers_lbl   = nullptr;
static lv_obj_t* s_mesh_peer_list   = nullptr;
static lv_obj_t* s_mesh_stats_lbl   = nullptr;
static lv_obj_t* s_mesh_bcast_ta    = nullptr;
static lv_timer_t* s_mesh_timer     = nullptr;

#if MESH_APP_AVAILABLE

static const char* mesh_role_str(MeshRole role) {
    switch (role) {
        case MESH_ROLE_GATEWAY: return "GATEWAY";
        case MESH_ROLE_RELAY:   return "NODE";
        case MESH_ROLE_LEAF:    return "LEAF";
        case MESH_ROLE_SENSOR:  return "SENSOR";
        default:                return "?";
    }
}

static lv_color_t mesh_role_color(MeshRole role) {
    switch (role) {
        case MESH_ROLE_GATEWAY: return T_CYAN;
        case MESH_ROLE_RELAY:   return T_GREEN;
        case MESH_ROLE_LEAF:    return T_GHOST;
        case MESH_ROLE_SENSOR:  return T_YELLOW;
        default:                return T_TEXT;
    }
}

static void mesh_refresh(lv_timer_t* t) {
    (void)t;
    auto& mm = MeshManager::instance();
    if (!mm.isReady()) return;
    char buf[80];

    // Status
    snprintf(buf, sizeof(buf), "Role: %s  %s",
             mesh_role_str(mm.getRole()),
             mm.isGateway() ? "[GW]" : "");
    lv_label_set_text(s_mesh_role_lbl, buf);
    lv_obj_set_style_text_color(s_mesh_role_lbl, mesh_role_color(mm.getRole()), 0);

    snprintf(buf, sizeof(buf), "Peers: %d", mm.peerCount());
    lv_label_set_text(s_mesh_peers_lbl, buf);

    // Peer list
    lv_obj_clean(s_mesh_peer_list);
    MeshPeerInfo peers[MESH_MAX_PEERS];
    int count = mm.getPeers(peers, MESH_MAX_PEERS);
    uint32_t now = millis();

    for (int i = 0; i < count; i++) {
        lv_obj_t* row = lv_obj_create(s_mesh_peer_list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // MAC (last 3 bytes)
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X",
                 peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
        lv_obj_t* mac_lbl = tritium_theme::createLabel(row, buf, true);
        lv_obj_set_style_text_color(mac_lbl, T_BRIGHT, 0);

        // Role badge
        lv_obj_t* role_dot = tritium_theme::createStatusDot(row, mesh_role_color(peers[i].role));

        // RSSI
        snprintf(buf, sizeof(buf), "%ddBm", (int)peers[i].rssi);
        tritium_theme::createLabel(row, buf, true);

        // Hops
        snprintf(buf, sizeof(buf), "H%d", peers[i].hop_count);
        tritium_theme::createLabel(row, buf, true);

        // Last seen
        uint32_t age_s = (now > peers[i].last_seen_ms) ?
                         (now - peers[i].last_seen_ms) / 1000 : 0;
        snprintf(buf, sizeof(buf), "%us", (unsigned)age_s);
        lv_obj_t* age_lbl = tritium_theme::createLabel(row, buf, true);
        lv_obj_set_style_text_color(age_lbl, (age_s < 30) ? T_GREEN : T_GHOST, 0);
    }

    // Stats
    const auto& stats = mm.getStats();
    snprintf(buf, sizeof(buf), "TX:%u RX:%u Relay:%u Drop:%u",
             (unsigned)stats.tx_count, (unsigned)stats.rx_count,
             (unsigned)stats.relay_count, (unsigned)stats.dedup_drop);
    lv_label_set_text(s_mesh_stats_lbl, buf);
}

static void mesh_send_cb(lv_event_t* e) {
    (void)e;
    auto& mm = MeshManager::instance();
    if (!mm.isReady() || !s_mesh_bcast_ta) return;
    const char* text = lv_textarea_get_text(s_mesh_bcast_ta);
    if (text && text[0]) {
        mm.broadcast((const uint8_t*)text, strlen(text));
        lv_textarea_set_text(s_mesh_bcast_ta, "");
        tritium_shell::toast("Broadcast sent", tritium_shell::NOTIFY_SUCCESS, 1500);
    }
}

#endif  // MESH_APP_AVAILABLE

void mesh_app_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 8, 0);
    lv_obj_set_style_pad_gap(viewport, 6, 0);

#if !MESH_APP_AVAILABLE
    tritium_theme::createLabel(viewport, "Mesh HAL not available");
    return;
#else
    // --- Status panel ---
    lv_obj_t* status_panel = tritium_theme::createPanel(viewport, "MESH STATUS");
    lv_obj_set_width(status_panel, lv_pct(100));
    lv_obj_set_height(status_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(status_panel, 24, 0);
    lv_obj_set_style_pad_gap(status_panel, 4, 0);

    s_mesh_role_lbl  = tritium_theme::createLabel(status_panel, "Role: --", true);
    s_mesh_peers_lbl = tritium_theme::createLabel(status_panel, "Peers: 0", true);

    // --- Peer list ---
    lv_obj_t* peer_panel = tritium_theme::createPanel(viewport, "PEERS");
    lv_obj_set_width(peer_panel, lv_pct(100));
    lv_obj_set_height(peer_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(peer_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(peer_panel, 24, 0);
    lv_obj_set_style_max_height(peer_panel, 200, 0);

    s_mesh_peer_list = lv_obj_create(peer_panel);
    lv_obj_set_size(s_mesh_peer_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_mesh_peer_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_mesh_peer_list, 0, 0);
    lv_obj_set_style_pad_all(s_mesh_peer_list, 0, 0);
    lv_obj_set_flex_flow(s_mesh_peer_list, LV_FLEX_FLOW_COLUMN);

    // --- Broadcast ---
    lv_obj_t* bcast_panel = tritium_theme::createPanel(viewport, "BROADCAST");
    lv_obj_set_width(bcast_panel, lv_pct(100));
    lv_obj_set_height(bcast_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bcast_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(bcast_panel, 24, 0);
    lv_obj_set_style_pad_gap(bcast_panel, 4, 0);

    s_mesh_bcast_ta = lv_textarea_create(bcast_panel);
    lv_textarea_set_one_line(s_mesh_bcast_ta, true);
    lv_textarea_set_placeholder_text(s_mesh_bcast_ta, "Message...");
    lv_obj_set_width(s_mesh_bcast_ta, lv_pct(100));
    lv_obj_set_style_bg_color(s_mesh_bcast_ta, T_SURFACE3, 0);
    lv_obj_set_style_text_color(s_mesh_bcast_ta, T_TEXT, 0);
    lv_obj_set_style_border_color(s_mesh_bcast_ta, T_CYAN, 0);
    lv_obj_set_style_border_opa(s_mesh_bcast_ta, LV_OPA_20, 0);

    lv_obj_t* send_btn = tritium_theme::createButton(bcast_panel, LV_SYMBOL_OK " SEND");
    lv_obj_set_width(send_btn, lv_pct(50));
    lv_obj_add_event_cb(send_btn, mesh_send_cb, LV_EVENT_CLICKED, nullptr);

    // --- Stats ---
    s_mesh_stats_lbl = tritium_theme::createLabel(viewport, "TX:0 RX:0 Relay:0 Drop:0", true);
    lv_obj_set_style_text_color(s_mesh_stats_lbl, T_GHOST, 0);

    // Auto-refresh
    mesh_refresh(nullptr);
    s_mesh_timer = lv_timer_create(mesh_refresh, 3000, nullptr);
#endif
}

// ===========================================================================
//  File Browser App
// ===========================================================================

#ifndef SIMULATOR
#include <dirent.h>
#include <sys/stat.h>
#endif

static char s_files_cwd[128] = "/";
static lv_obj_t* s_files_path_lbl   = nullptr;
static lv_obj_t* s_files_list       = nullptr;
static lv_obj_t* s_files_info_lbl   = nullptr;
static bool s_files_use_sd          = false;

static void files_navigate(const char* path);

#ifndef SIMULATOR

static void files_item_cb(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    const char* path = (const char*)lv_event_get_user_data(e);
    if (path) {
        files_navigate(path);
    }
}

static void files_back_cb(lv_event_t* e) {
    (void)e;
    // Go up one level
    char parent[128];
    strncpy(parent, s_files_cwd, sizeof(parent));
    parent[sizeof(parent) - 1] = '\0';
    // Trim trailing slash
    size_t len = strlen(parent);
    if (len > 1 && parent[len - 1] == '/') parent[len - 1] = '\0';
    // Find last slash
    char* last = strrchr(parent, '/');
    if (last && last != parent) {
        *(last + 1) = '\0';
    } else {
        strcpy(parent, "/");
    }
    files_navigate(parent);
}

static void files_navigate(const char* path) {
    strncpy(s_files_cwd, path, sizeof(s_files_cwd));
    s_files_cwd[sizeof(s_files_cwd) - 1] = '\0';
    lv_label_set_text(s_files_path_lbl, s_files_cwd);
    lv_obj_clean(s_files_list);

    // Mount point prefix for SD
    const char* mount = s_files_use_sd ? "/sd" : "/spiffs";
    char fullpath[160];
    snprintf(fullpath, sizeof(fullpath), "%s%s", mount,
             s_files_cwd[0] == '/' ? s_files_cwd : "/");

    DIR* dir = opendir(fullpath);
    if (!dir) {
        tritium_theme::createLabel(s_files_list, "Cannot open directory");
        return;
    }

    struct dirent* entry;
    // We store path strings in a static ring buffer to keep them alive for callbacks
    static char path_buf[16][160];
    static int path_idx = 0;

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;  // Skip hidden

        bool is_dir = (entry->d_type == DT_DIR);

        lv_obj_t* row = lv_obj_create(s_files_list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 3, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        const char* icon = is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
        char label[80];
        if (is_dir) {
            snprintf(label, sizeof(label), "%s %s", icon, entry->d_name);
        } else {
            // Get file size
            char fpath[200];
            snprintf(fpath, sizeof(fpath), "%s/%s", fullpath, entry->d_name);
            struct stat st;
            size_t fsize = 0;
            if (stat(fpath, &st) == 0) fsize = st.st_size;
            if (fsize > 1024 * 1024) {
                snprintf(label, sizeof(label), "%s %s  %.1fMB",
                         icon, entry->d_name, fsize / (1024.0f * 1024.0f));
            } else if (fsize > 1024) {
                snprintf(label, sizeof(label), "%s %s  %.1fKB",
                         icon, entry->d_name, fsize / 1024.0f);
            } else {
                snprintf(label, sizeof(label), "%s %s  %uB",
                         icon, entry->d_name, (unsigned)fsize);
            }
        }

        lv_obj_t* name_lbl = tritium_theme::createLabel(row, label);
        lv_obj_set_flex_grow(name_lbl, 1);
        lv_obj_set_style_text_color(name_lbl, is_dir ? T_CYAN : T_TEXT, 0);

        if (is_dir) {
            // Build navigation path
            int pi = path_idx % 16;
            path_idx++;
            if (strcmp(s_files_cwd, "/") == 0) {
                snprintf(path_buf[pi], sizeof(path_buf[pi]), "/%s/", entry->d_name);
            } else {
                snprintf(path_buf[pi], sizeof(path_buf[pi]), "%s%s/",
                         s_files_cwd, entry->d_name);
            }
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, files_item_cb, LV_EVENT_CLICKED, path_buf[pi]);
        }
    }
    closedir(dir);

    // Storage info
#if FILES_FS_AVAILABLE
    if (!s_files_use_sd) {
        FsHAL fs;
        if (fs.isReady()) {
            char info[64];
            snprintf(info, sizeof(info), "LittleFS: %uKB / %uKB",
                     (unsigned)(fs.usedBytes() / 1024),
                     (unsigned)(fs.totalBytes() / 1024));
            lv_label_set_text(s_files_info_lbl, info);
        }
    }
#endif
#if FILES_SD_AVAILABLE
    // SD info handled similarly if mounted
#endif
}

#else  // SIMULATOR

static void files_navigate(const char* path) {
    (void)path;
}

#endif  // SIMULATOR

void files_app_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 8, 0);
    lv_obj_set_style_pad_gap(viewport, 6, 0);

#ifdef SIMULATOR
    tritium_theme::createLabel(viewport, "File Browser not available in simulator");
    return;
#else
    // --- Path bar with back button ---
    lv_obj_t* path_row = lv_obj_create(viewport);
    lv_obj_set_size(path_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(path_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(path_row, 0, 0);
    lv_obj_set_style_pad_all(path_row, 0, 0);
    lv_obj_set_flex_flow(path_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(path_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(path_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back_btn = tritium_theme::createButton(path_row, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(back_btn, files_back_cb, LV_EVENT_CLICKED, nullptr);

    s_files_path_lbl = tritium_theme::createLabel(path_row, "/", true);
    lv_obj_set_flex_grow(s_files_path_lbl, 1);
    lv_obj_set_style_text_color(s_files_path_lbl, T_CYAN, 0);

    // --- Storage toggle ---
#if FILES_SD_AVAILABLE
    lv_obj_t* toggle_row = lv_obj_create(viewport);
    lv_obj_set_size(toggle_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(toggle_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(toggle_row, 0, 0);
    lv_obj_set_style_pad_all(toggle_row, 0, 0);
    lv_obj_set_flex_flow(toggle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toggle_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(toggle_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_gap(toggle_row, 8, 0);

    tritium_theme::createLabel(toggle_row, "LittleFS");
    lv_obj_t* sd_sw = tritium_theme::createSwitch(toggle_row, false);
    tritium_theme::createLabel(toggle_row, "SD");
    // SD toggle callback would set s_files_use_sd and re-navigate
#endif

    // --- File list ---
    lv_obj_t* list_panel = tritium_theme::createPanel(viewport, "FILES");
    lv_obj_set_width(list_panel, lv_pct(100));
    lv_obj_set_flex_grow(list_panel, 1);
    lv_obj_set_flex_flow(list_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(list_panel, 24, 0);

    s_files_list = lv_obj_create(list_panel);
    lv_obj_set_size(s_files_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_files_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_files_list, 0, 0);
    lv_obj_set_style_pad_all(s_files_list, 0, 0);
    lv_obj_set_flex_flow(s_files_list, LV_FLEX_FLOW_COLUMN);

    // --- Storage info bar ---
    s_files_info_lbl = tritium_theme::createLabel(viewport, "Storage: --", true);
    lv_obj_set_style_text_color(s_files_info_lbl, T_GHOST, 0);

    // Navigate to root
    strcpy(s_files_cwd, "/");
    files_navigate("/");
#endif
}

// ===========================================================================
//  Power App (standalone — kept for API compatibility, content now in Settings)
// ===========================================================================

void power_app_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 8, 0);
    lv_obj_set_style_pad_gap(viewport, 6, 0);

    // --- Battery display ---
    lv_obj_t* batt_panel = tritium_theme::createPanel(viewport, "BATTERY");
    lv_obj_set_width(batt_panel, lv_pct(100));
    lv_obj_set_height(batt_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(batt_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(batt_panel, 24, 0);
    lv_obj_set_style_pad_gap(batt_panel, 4, 0);
    lv_obj_set_flex_align(batt_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Large battery icon
    s_power_icon_lbl = lv_label_create(batt_panel);
    lv_label_set_text(s_power_icon_lbl, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(s_power_icon_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_power_icon_lbl, T_GREEN, 0);

    // Large percentage
    s_power_pct_lbl = lv_label_create(batt_panel);
    lv_label_set_text(s_power_pct_lbl, "--%");
    lv_obj_set_style_text_font(s_power_pct_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_power_pct_lbl, T_GREEN, 0);

    // Source + voltage row
    lv_obj_t* info_row = lv_obj_create(batt_panel);
    lv_obj_set_size(info_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_row, 0, 0);
    lv_obj_set_style_pad_all(info_row, 0, 0);
    lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(info_row, LV_OBJ_FLAG_SCROLLABLE);

    s_power_source_lbl = tritium_theme::createLabel(info_row, "Source: --", true);
    s_power_volt_lbl   = tritium_theme::createLabel(info_row, "--V", true);

    // --- Power Profile ---
    lv_obj_t* prof_panel = tritium_theme::createPanel(viewport, "PROFILE");
    lv_obj_set_width(prof_panel, lv_pct(100));
    lv_obj_set_height(prof_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(prof_panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(prof_panel, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(prof_panel, 24, 0);

    static const char* prof_labels[] = {
        LV_SYMBOL_CHARGE, LV_SYMBOL_OK, LV_SYMBOL_DOWN, LV_SYMBOL_LOOP
    };
    for (int i = 0; i < 4; i++) {
        lv_color_t accent = (i == s_power_profile) ? T_CYAN : T_GHOST;
        s_power_prof_btns[i] = tritium_theme::createButton(prof_panel, prof_labels[i], accent);
        lv_obj_add_event_cb(s_power_prof_btns[i], power_profile_cb,
                            LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    // Profile legend
    lv_obj_t* legend = tritium_theme::createLabel(viewport, "PERF | BAL | SAVE | AUTO", true);
    lv_obj_set_style_text_color(legend, T_GHOST, 0);
    lv_obj_set_width(legend, lv_pct(100));
    lv_obj_set_style_text_align(legend, LV_TEXT_ALIGN_CENTER, 0);

    // --- Timeouts ---
    lv_obj_t* timeout_panel = tritium_theme::createPanel(viewport, "TIMEOUTS");
    lv_obj_set_width(timeout_panel, lv_pct(100));
    lv_obj_set_height(timeout_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(timeout_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(timeout_panel, 24, 0);
    lv_obj_set_style_pad_gap(timeout_panel, 6, 0);

    // Screen dim
    lv_obj_t* dim_row = lv_obj_create(timeout_panel);
    lv_obj_set_size(dim_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dim_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dim_row, 0, 0);
    lv_obj_set_style_pad_all(dim_row, 0, 0);
    lv_obj_set_flex_flow(dim_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dim_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(dim_row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(dim_row, "Dim");
    s_power_dim_val = tritium_theme::createLabel(dim_row, "30s", true);

    s_power_dim_slider = tritium_theme::createSlider(timeout_panel, 0, 300, 30);
    lv_obj_set_width(s_power_dim_slider, lv_pct(95));
    lv_obj_add_event_cb(s_power_dim_slider, power_dim_slider_cb,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    // Screen off
    lv_obj_t* off_row = lv_obj_create(timeout_panel);
    lv_obj_set_size(off_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(off_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(off_row, 0, 0);
    lv_obj_set_style_pad_all(off_row, 0, 0);
    lv_obj_set_flex_flow(off_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(off_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(off_row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(off_row, "Screen Off");
    s_power_off_val = tritium_theme::createLabel(off_row, "60s", true);

    s_power_off_slider = tritium_theme::createSlider(timeout_panel, 0, 600, 60);
    lv_obj_set_width(s_power_off_slider, lv_pct(95));
    lv_obj_add_event_cb(s_power_off_slider, power_off_slider_cb,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    // --- Screen state indicator ---
    s_power_state_lbl = tritium_theme::createLabel(viewport, "Screen: ON", true);
    lv_obj_set_style_text_color(s_power_state_lbl, T_GREEN, 0);

    // Auto-refresh
    power_update(nullptr);
    s_power_timer = lv_timer_create(power_update, 3000, nullptr);
}

// ===========================================================================
//  Cleanup — delete active LVGL timers before switching apps
// ===========================================================================

void cleanup_timers() {
    if (s_sysmon_timer) { lv_timer_delete(s_sysmon_timer); s_sysmon_timer = nullptr; }
    if (s_wifi_timer)   { lv_timer_delete(s_wifi_timer);   s_wifi_timer   = nullptr; }
    if (s_mesh_timer)   { lv_timer_delete(s_mesh_timer);   s_mesh_timer   = nullptr; }
    if (s_power_timer)  { lv_timer_delete(s_power_timer);  s_power_timer  = nullptr; }
}

//  Register all new apps
// ===========================================================================

void register_all_apps() {
    // WiFi, Power, Brightness, About are now sub-panels inside Settings.
    tritium_shell::registerApp({"Monitor", "System health",    LV_SYMBOL_EYE_OPEN,     true, sysmon_app_create});
    tritium_shell::registerApp({"Mesh",    "P2P network",      LV_SYMBOL_SHUFFLE,      true, mesh_app_create});
    tritium_shell::registerApp({"Files",   "File browser",     LV_SYMBOL_DIRECTORY,     true, files_app_create});
}

}  // namespace shell_apps
