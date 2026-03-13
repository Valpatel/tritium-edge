/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "shell_apps.h"
#include "shell_theme.h"
#include "shell_screensaver.h"
#include "display.h"
#include "tritium_splash.h"  // TRITIUM_VERSION
#include "os_settings.h"     // TritiumSettings, SettingsDomain
#include <cstdio>

#ifndef SIMULATOR
#include "tritium_compat.h"
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <driver/i2c.h>
#include <esp_chip_info.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <SDL2/SDL.h>
static uint32_t millis() { return SDL_GetTicks(); }
#endif

// HAL includes must be outside namespace to avoid resolving std:: as shell_apps::std::
#if defined(ENABLE_WIFI) && __has_include("wifi_manager.h")
#include "wifi_manager.h"
#ifndef SIMULATOR
#include "esp_wifi.h"
#include "esp_netif.h"
#endif
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

#if __has_include("hal_ntp.h")
#include "hal_ntp.h"
#define NTP_AVAILABLE 1
#else
#define NTP_AVAILABLE 0
#endif

#if defined(HAS_RTC) && HAS_RTC && __has_include("hal_rtc.h")
#include "hal_rtc.h"
#define RTC_AVAILABLE 1
#else
#define RTC_AVAILABLE 0
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

#if __has_include("service_registry.h")
#include "service_registry.h"
#endif

#if defined(ENABLE_STORAGE) && __has_include("storage_service.h")
#include "storage_service.h"
#define STORAGE_SVC_AVAILABLE 1
#else
#define STORAGE_SVC_AVAILABLE 0
#endif

#if defined(ENABLE_BLE_SCANNER) && __has_include("hal_ble_scanner.h")
#include "hal_ble_scanner.h"
#define BLE_APP_AVAILABLE 1
#else
#define BLE_APP_AVAILABLE 0
#endif

#if defined(ENABLE_WEBSERVER) && __has_include("serial_capture.h")
#include "serial_capture.h"
#include "service_registry.h"
#define TERMINAL_AVAILABLE 1
#else
#define TERMINAL_AVAILABLE 0
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

static lv_obj_t* s_wifi_detail_lbl = nullptr;

static void wifi_refresh_status() {
    auto* wm = WifiManager::_instance;
    if (!wm) return;
    bool connected = wm->isConnected();
    const char* ssid = wm->getSSID();
    const char* ip = wm->getIP();
    int32_t rssi = wm->getRSSI();
    char buf[96];

    if (connected) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s  (%ddBm)", ssid ? ssid : "", (int)rssi);
        lv_label_set_text(s_wifi_status_ssid, buf);
        snprintf(buf, sizeof(buf), "IP: %s", ip ? ip : "--");
        lv_label_set_text(s_wifi_status_ip, buf);
        int pct = (rssi + 100);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_wifi_rssi_bar, pct, LV_ANIM_ON);
        lv_color_t c = (rssi > -50) ? T_GREEN : (rssi > -70) ? T_YELLOW : T_MAGENTA;
        lv_obj_set_style_bg_color(s_wifi_rssi_bar, c, LV_PART_INDICATOR);

        // Detailed info
        if (s_wifi_detail_lbl) {
            char detail[256];
            int pos = 0;

            // BSSID, Gateway, Subnet, DNS via ESP-IDF
            {
                wifi_ap_record_t ap_info;
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                    pos += snprintf(detail + pos, sizeof(detail) - pos,
                        "BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
                        ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                        ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);
                }
                esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif) {
                    esp_netif_ip_info_t ip_info;
                    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                        char gw_str[16], sn_str[16];
                        esp_ip4addr_ntoa(&ip_info.gw, gw_str, sizeof(gw_str));
                        esp_ip4addr_ntoa(&ip_info.netmask, sn_str, sizeof(sn_str));
                        pos += snprintf(detail + pos, sizeof(detail) - pos,
                            "Gateway: %s\n", gw_str);
                        pos += snprintf(detail + pos, sizeof(detail) - pos,
                            "Subnet: %s\n", sn_str);
                    }
                    esp_netif_dns_info_t dns_info;
                    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
                        char dns_str[16];
                        esp_ip4addr_ntoa((const esp_ip4_addr_t*)&dns_info.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                        pos += snprintf(detail + pos, sizeof(detail) - pos,
                            "DNS: %s\n", dns_str);
                    }
                }
            }

            // MAC address via ESP-IDF
            {
                uint8_t mac[6];
                esp_read_mac(mac, ESP_MAC_WIFI_STA);
                pos += snprintf(detail + pos, sizeof(detail) - pos,
                    "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            }
            lv_label_set_text(s_wifi_detail_lbl, detail);
        }
    } else {
        lv_label_set_text(s_wifi_status_ssid, LV_SYMBOL_WIFI " Disconnected");
        lv_label_set_text(s_wifi_status_ip, "IP: --");
        lv_bar_set_value(s_wifi_rssi_bar, 0, LV_ANIM_OFF);
        if (s_wifi_detail_lbl) {
            lv_label_set_text(s_wifi_detail_lbl, "Not connected");
        }
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
        lv_obj_set_style_text_color(name, T_TEXT, 0);  // ScanResult.known not in dev API

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
        if (nets[i].ssid[0] == '\0') continue;
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
        snprintf(lbl, sizeof(lbl), "[%d] %s", i, nets[i].ssid);
        lv_obj_t* name = tritium_theme::createLabel(row, lbl, true);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_style_text_color(name, T_TEXT, 0);

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
static const int SETTINGS_NUM_TABS = 7;
static lv_obj_t* s_settings_tab_btns[SETTINGS_NUM_TABS] = {};
static int s_settings_active_tab = 0;

// Forward declarations for tab content builders
static void settings_build_display(lv_obj_t* cont);
static void settings_build_wifi(lv_obj_t* cont);
static void settings_build_power(lv_obj_t* cont);
static void settings_build_screensaver(lv_obj_t* cont);
static void settings_build_system(lv_obj_t* cont);
static void settings_build_clock(lv_obj_t* cont);
static void settings_build_developer(lv_obj_t* cont);

static void settings_select_tab(int idx) {
    if (!s_settings_content) return;
    s_settings_active_tab = idx;

    // Update tab button styling
    for (int i = 0; i < SETTINGS_NUM_TABS; i++) {
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
        case 5: settings_build_clock(s_settings_content); break;
        case 6: settings_build_developer(s_settings_content); break;
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
    // Prevent viewport from scrolling — only the content area should scroll,
    // keeping the tab bar always visible at the top
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);

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
        LV_SYMBOL_BELL,        // Clock/Time
        LV_SYMBOL_LIST,        // Developer
    };
    for (int i = 0; i < SETTINGS_NUM_TABS; i++) {
        lv_obj_t* btn = lv_btn_create(tab_bar);
        lv_obj_set_flex_grow(btn, 1);
        int tab_h = (tritium_shell::uiConfig().screen_height > 400) ? 48 : 36;
        lv_obj_set_height(btn, tab_h);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tab_icons[i]);
        lv_obj_set_style_text_font(lbl, tritium_shell::uiIconFont(), 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, settings_tab_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);
        s_settings_tab_btns[i] = btn;
    }

    // --- Scrollable content area (fills remaining space, scrolls internally) ---
    s_settings_content = lv_obj_create(viewport);
    lv_obj_set_width(s_settings_content, lv_pct(100));
    lv_obj_set_flex_grow(s_settings_content, 1);
    lv_obj_set_style_min_height(s_settings_content, 0, 0);
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

static lv_obj_t* make_row(lv_obj_t* parent, const char* label) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    tritium_theme::createLabel(row, label);
    return row;
}

static lv_obj_t* make_col(lv_obj_t* parent, int pct) {
    lv_obj_t* col = lv_obj_create(parent);
    lv_obj_set_width(col, lv_pct(pct));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_gap(col, 4, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    return col;
}

static void settings_build_display(lv_obj_t* cont) {
    lv_obj_t* panel = tritium_theme::createPanel(cont, "DISPLAY");
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(panel, 24, 0);
    lv_obj_set_style_pad_gap(panel, 6, 0);

    // Two-column layout
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* left = make_col(panel, 55);
    lv_obj_t* right = make_col(panel, 38);

    // Left: Brightness
    lv_obj_t* bright_row = make_row(left, LV_SYMBOL_EYE_OPEN " Brightness");
    lv_obj_t* bright_val = tritium_theme::createLabel(bright_row, "100%", true);

    lv_obj_t* slider = tritium_theme::createSlider(left, 10, 255, 255);
    lv_obj_set_width(slider, lv_pct(100));
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED,
                         bright_val);

    // Right: Timeout
    lv_obj_t* timeout_row = make_row(right, LV_SYMBOL_BELL " Timeout");
    (void)timeout_row;

    lv_obj_t* dd = lv_dropdown_create(right);
    lv_dropdown_set_options(dd, "30s\n1m\n5m\nNever");
    lv_dropdown_set_selected(dd, 2);
    lv_obj_set_width(dd, lv_pct(100));
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

    // --- Connection detail (BSSID, gateway, DNS, subnet, MAC, duration) ---
    s_wifi_detail_lbl = tritium_theme::createLabel(status_panel, "...", true);
    lv_obj_set_style_text_font(s_wifi_detail_lbl, tritium_shell::uiSmallFont(), 0);
    lv_obj_set_style_text_color(s_wifi_detail_lbl, T_GHOST, 0);

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
        WifiManager::_instance ? WifiManager::_instance->isAPMode() : false);
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
    lv_obj_set_style_text_font(s_power_icon_lbl, tritium_shell::uiHeadingFont(), 0);
    lv_obj_set_style_text_color(s_power_icon_lbl, T_GREEN, 0);

    s_power_pct_lbl = lv_label_create(batt_panel);
    lv_label_set_text(s_power_pct_lbl, "--%");
    lv_obj_set_style_text_font(s_power_pct_lbl, tritium_shell::uiHeadingFont(), 0);
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

    // --- Timeouts (two-column) ---
    lv_obj_t* timeout_panel = tritium_theme::createPanel(cont, "TIMEOUTS");
    lv_obj_set_width(timeout_panel, lv_pct(100));
    lv_obj_set_height(timeout_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(timeout_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(timeout_panel, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(timeout_panel, 24, 0);

    // Left: Dim timeout
    lv_obj_t* dim_col = make_col(timeout_panel, 45);
    lv_obj_t* dim_row = make_row(dim_col, "Dim");
    s_power_dim_val = tritium_theme::createLabel(dim_row, "30s", true);
    s_power_dim_slider = tritium_theme::createSlider(dim_col, 0, 300, 30);
    lv_obj_set_width(s_power_dim_slider, lv_pct(100));
    lv_obj_add_event_cb(s_power_dim_slider, power_dim_slider_cb,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    // Right: Screen off timeout
    lv_obj_t* off_col = make_col(timeout_panel, 45);
    lv_obj_t* off_row = make_row(off_col, "Screen Off");
    s_power_off_val = tritium_theme::createLabel(off_row, "60s", true);
    s_power_off_slider = tritium_theme::createSlider(off_col, 0, 600, 60);
    lv_obj_set_width(s_power_off_slider, lv_pct(100));
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

static void ss_direction_cb(lv_event_t* e) {
    lv_obj_t* dd = (lv_obj_t*)lv_event_get_target(e);
    int sel = lv_dropdown_get_selected(dd);
    TritiumSettings::instance().setInt(SettingsDomain::SCREENSAVER, "sf_direction", sel);
    shell_screensaver::reloadSettings();
}

static void ss_colors_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    TritiumSettings::instance().setBool(SettingsDomain::SCREENSAVER, "sf_colors", checked);
    shell_screensaver::reloadSettings();
}

static void ss_warp_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    TritiumSettings::instance().setBool(SettingsDomain::SCREENSAVER, "sf_warp", checked);
    shell_screensaver::reloadSettings();
}

static void ss_clock_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    TritiumSettings::instance().setBool(SettingsDomain::SCREENSAVER, "sf_clock", checked);
    shell_screensaver::reloadSettings();
}

static void ss_starsize_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    TritiumSettings::instance().setInt(SettingsDomain::SCREENSAVER, "sf_star_size", val);
    shell_screensaver::reloadSettings();
    lv_obj_t* lbl = (lv_obj_t*)lv_event_get_user_data(e);
    if (lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%dpx", val);
        lv_label_set_text(lbl, buf);
    }
}

static void ss_speed_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    TritiumSettings::instance().setInt(SettingsDomain::SCREENSAVER, "sf_speed", val);
    shell_screensaver::reloadSettings();
    lv_obj_t* lbl = (lv_obj_t*)lv_event_get_user_data(e);
    if (lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.3f", (float)val * 0.001f);
        lv_label_set_text(lbl, buf);
    }
}

static void ss_brightness_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    TritiumSettings::instance().setInt(SettingsDomain::SCREENSAVER, "sf_brightness", val);
    shell_screensaver::reloadSettings();
    lv_obj_t* lbl = (lv_obj_t*)lv_event_get_user_data(e);
    if (lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(lbl, buf);
    }
}

static void ss_timeout_cb(lv_event_t* e) {
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    TritiumSettings::instance().setInt(SettingsDomain::SCREENSAVER, "timeout_s", val);
    shell_screensaver::setTimeoutS(val);
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

// Reuse generic make_row/make_col helpers defined above settings_build_display
static inline lv_obj_t* ss_make_row(lv_obj_t* p, const char* l) { return make_row(p, l); }
static inline lv_obj_t* ss_make_col(lv_obj_t* p, int pct) { return make_col(p, pct); }

static void settings_build_screensaver(lv_obj_t* cont) {
    auto& cfg = TritiumSettings::instance();

    // --- Two-column layout: toggles left, sliders right ---
    lv_obj_t* panel = tritium_theme::createPanel(cont, "SCREENSAVER");
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(panel, 24, 0);
    lv_obj_set_style_pad_gap(panel, 6, 0);

    // Two-column row container
    lv_obj_t* cols = lv_obj_create(panel);
    lv_obj_set_size(cols, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cols, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cols, 0, 0);
    lv_obj_set_style_pad_all(cols, 0, 0);
    lv_obj_set_style_pad_gap(cols, 8, 0);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(cols, LV_OBJ_FLAG_SCROLLABLE);

    // --- LEFT COLUMN: Toggles ---
    lv_obj_t* left = ss_make_col(cols, 48);

    // Timeout
    lv_obj_t* timeout_row = ss_make_row(left, "Timeout");
    int timeout_s = cfg.getInt(SettingsDomain::SCREENSAVER, "timeout_s", 10);
    char timeout_str[16];
    if (timeout_s == 0) snprintf(timeout_str, sizeof(timeout_str), "Off");
    else if (timeout_s < 60) snprintf(timeout_str, sizeof(timeout_str), "%ds", timeout_s);
    else snprintf(timeout_str, sizeof(timeout_str), "%dm", timeout_s / 60);
    lv_obj_t* timeout_val = tritium_theme::createLabel(timeout_row, timeout_str, true);
    lv_obj_t* timeout_slider = tritium_theme::createSlider(left, 0, 600, timeout_s);
    lv_obj_set_width(timeout_slider, lv_pct(100));
    lv_obj_add_event_cb(timeout_slider, ss_timeout_cb, LV_EVENT_VALUE_CHANGED, timeout_val);

    // Direction
    lv_obj_t* dir_row = ss_make_row(left, "Direction");
    (void)dir_row;
    int sf_dir = cfg.getInt(SettingsDomain::SCREENSAVER, "sf_direction", 0); // default FORWARD
    lv_obj_t* dir_dd = lv_dropdown_create(left);
    lv_dropdown_set_options(dir_dd, "Forward\nReverse\nLeft\nRight\nUp\nDown");
    lv_dropdown_set_selected(dir_dd, sf_dir);
    lv_obj_set_width(dir_dd, lv_pct(100));
    lv_obj_set_style_bg_color(dir_dd, T_SURFACE3, 0);
    lv_obj_set_style_text_color(dir_dd, T_TEXT, 0);
    lv_obj_set_style_border_color(dir_dd, T_CYAN, 0);
    lv_obj_set_style_border_opa(dir_dd, LV_OPA_20, 0);
    lv_obj_add_event_cb(dir_dd, ss_direction_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Colored stars
    lv_obj_t* col_row = ss_make_row(left, "Colors");
    bool sf_colors = cfg.getBool(SettingsDomain::SCREENSAVER, "sf_colors", true);
    lv_obj_t* col_sw = tritium_theme::createSwitch(col_row, sf_colors);
    lv_obj_add_event_cb(col_sw, ss_colors_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Warp cycle
    lv_obj_t* warp_row = ss_make_row(left, "Warp");
    bool sf_warp = cfg.getBool(SettingsDomain::SCREENSAVER, "sf_warp", false);
    lv_obj_t* warp_sw = tritium_theme::createSwitch(warp_row, sf_warp);
    lv_obj_add_event_cb(warp_sw, ss_warp_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Clock overlay
    lv_obj_t* clock_row = ss_make_row(left, "Clock");
    bool sf_clock = cfg.getBool(SettingsDomain::SCREENSAVER, "sf_clock", false);
    lv_obj_t* clock_sw = tritium_theme::createSwitch(clock_row, sf_clock);
    lv_obj_add_event_cb(clock_sw, ss_clock_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- RIGHT COLUMN: Sliders ---
    lv_obj_t* right = ss_make_col(cols, 48);

    // Brightness
    lv_obj_t* brt_row = ss_make_row(right, "Brightness");
    int sf_bright = cfg.getInt(SettingsDomain::SCREENSAVER, "sf_brightness", 80);
    char brt_str[8];
    snprintf(brt_str, sizeof(brt_str), "%d%%", sf_bright);
    lv_obj_t* brt_val = tritium_theme::createLabel(brt_row, brt_str, true);
    lv_obj_t* brt_slider = tritium_theme::createSlider(right, 10, 100, sf_bright);
    lv_obj_set_width(brt_slider, lv_pct(100));
    lv_obj_add_event_cb(brt_slider, ss_brightness_cb, LV_EVENT_VALUE_CHANGED, brt_val);

    // Star size
    lv_obj_t* size_row = ss_make_row(right, "Size");
    int sf_size = cfg.getInt(SettingsDomain::SCREENSAVER, "sf_star_size", 2);
    if (sf_size < 1) sf_size = 1;
    if (sf_size > 4) sf_size = 4;
    char size_str[8];
    snprintf(size_str, sizeof(size_str), "%dpx", sf_size);
    lv_obj_t* size_val = tritium_theme::createLabel(size_row, size_str, true);
    lv_obj_t* size_slider = tritium_theme::createSlider(right, 1, 4, sf_size);
    lv_obj_set_width(size_slider, lv_pct(100));
    lv_obj_add_event_cb(size_slider, ss_starsize_cb, LV_EVENT_VALUE_CHANGED, size_val);

    // Travel speed
    lv_obj_t* spd_row = ss_make_row(right, "Speed");
    int sf_speed = cfg.getInt(SettingsDomain::SCREENSAVER, "sf_speed", 12);
    char spd_str[16];
    snprintf(spd_str, sizeof(spd_str), "%.3f", (float)sf_speed * 0.001f);
    lv_obj_t* spd_val = tritium_theme::createLabel(spd_row, spd_str, true);
    lv_obj_t* spd_slider = tritium_theme::createSlider(right, 1, 100, sf_speed);
    lv_obj_set_width(spd_slider, lv_pct(100));
    lv_obj_add_event_cb(spd_slider, ss_speed_cb, LV_EVENT_VALUE_CHANGED, spd_val);

    // --- TEST button (full width below columns) ---
    lv_obj_t* test_btn = tritium_theme::createButton(panel, LV_SYMBOL_PLAY " TEST");
    lv_obj_set_width(test_btn, lv_pct(100));
    lv_obj_add_event_cb(test_btn, [](lv_event_t* e) {
        (void)e;
        shell_screensaver::activate();
    }, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// Settings Tab: SYSTEM (merged About + system info)
// ---------------------------------------------------------------------------

static void settings_build_system(lv_obj_t* cont) {
    // --- About section ---
    lv_obj_t* title = lv_label_create(cont);
    lv_label_set_text(title, "TRITIUM-OS");
    lv_obj_set_style_text_color(title, T_CYAN, 0);
    lv_obj_set_style_text_font(title, tritium_shell::uiHeadingFont(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, lv_pct(100));

    lv_obj_t* tagline = lv_label_create(cont);
    lv_label_set_text(tagline, "Software-Defined Edge Intelligence");
    lv_obj_set_style_text_color(tagline, T_GHOST, 0);
    lv_obj_set_style_text_font(tagline, tritium_shell::uiSmallFont(), 0);
    lv_obj_set_style_text_align(tagline, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(tagline, lv_pct(100));

    char buf[128];

    // --- Device Info panel ---
    lv_obj_t* info = tritium_theme::createPanel(cont, "DEVICE INFO");
    lv_obj_set_width(info, lv_pct(100));
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(info, 24, 0);
    lv_obj_set_style_pad_gap(info, 4, 0);

    snprintf(buf, sizeof(buf), "Version: %s", TRITIUM_VERSION);
    tritium_theme::createLabel(info, buf, true);

    display_health_t dh = display_get_health();
    snprintf(buf, sizeof(buf), "Board: %s", dh.board_name ? dh.board_name : "Unknown");
    tritium_theme::createLabel(info, buf, true);

    snprintf(buf, sizeof(buf), "Display: %s %dx%d",
             dh.driver ? dh.driver : "?",
             display_get_width(), display_get_height());
    tritium_theme::createLabel(info, buf, true);

#ifndef SIMULATOR
    // --- Chip Info panel ---
    lv_obj_t* chip_panel = tritium_theme::createPanel(cont, "CHIP INFO");
    lv_obj_set_width(chip_panel, lv_pct(100));
    lv_obj_set_height(chip_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chip_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(chip_panel, 24, 0);
    lv_obj_set_style_pad_gap(chip_panel, 4, 0);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char* chip_model = "ESP32-S3";
    snprintf(buf, sizeof(buf), "Model: %s rev %d.%d",
             chip_model, chip.revision / 100, chip.revision % 100);
    tritium_theme::createLabel(chip_panel, buf, true);

    snprintf(buf, sizeof(buf), "Cores: %d  Features: %s%s%s",
             chip.cores,
             (chip.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
             (chip.features & CHIP_FEATURE_BLE) ? "BLE " : "",
             (chip.features & CHIP_FEATURE_IEEE802154) ? "802.15.4" : "");
    tritium_theme::createLabel(chip_panel, buf, true);

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(buf, sizeof(buf), "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    tritium_theme::createLabel(chip_panel, buf, true);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    snprintf(buf, sizeof(buf), "Flash: %uMB", (unsigned)(flash_size / (1024 * 1024)));
    tritium_theme::createLabel(chip_panel, buf, true);

    // --- Memory panel ---
    lv_obj_t* mem_panel = tritium_theme::createPanel(cont, "MEMORY");
    lv_obj_set_width(mem_panel, lv_pct(100));
    lv_obj_set_height(mem_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mem_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(mem_panel, 24, 0);
    lv_obj_set_style_pad_gap(mem_panel, 4, 0);

    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snprintf(buf, sizeof(buf), "Heap: %uKB / %uKB free",
             (unsigned)(free_heap / 1024), (unsigned)(total_heap / 1024));
    tritium_theme::createLabel(mem_panel, buf, true);

    uint32_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snprintf(buf, sizeof(buf), "Heap min free: %uKB", (unsigned)(min_heap / 1024));
    tritium_theme::createLabel(mem_panel, buf, true);

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total > 0) {
        snprintf(buf, sizeof(buf), "PSRAM: %.1fMB / %.1fMB free",
                 (float)psram_free / (1024.0f * 1024.0f),
                 (float)psram_total / (1024.0f * 1024.0f));
        tritium_theme::createLabel(mem_panel, buf, true);
    }

    // --- Partitions panel ---
    lv_obj_t* part_panel = tritium_theme::createPanel(cont, "PARTITIONS");
    lv_obj_set_width(part_panel, lv_pct(100));
    lv_obj_set_height(part_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(part_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(part_panel, 24, 0);
    lv_obj_set_style_pad_gap(part_panel, 4, 0);

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                     ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it) {
        const esp_partition_t* p = esp_partition_get(it);
        if (p) {
            const char* type_str = (p->type == ESP_PARTITION_TYPE_APP) ? "app" :
                                   (p->type == ESP_PARTITION_TYPE_DATA) ? "data" : "?";
            snprintf(buf, sizeof(buf), "%-12s %s  %uKB @ 0x%06x",
                     p->label, type_str,
                     (unsigned)(p->size / 1024),
                     (unsigned)p->address);
            lv_obj_t* plbl = tritium_theme::createLabel(part_panel, buf, true);
            lv_obj_set_style_text_font(plbl, tritium_shell::uiSmallFont(), 0);
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    // --- Runtime panel ---
    lv_obj_t* run_panel = tritium_theme::createPanel(cont, "RUNTIME");
    lv_obj_set_width(run_panel, lv_pct(100));
    lv_obj_set_height(run_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(run_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(run_panel, 24, 0);
    lv_obj_set_style_pad_gap(run_panel, 4, 0);

    uint32_t secs = millis() / 1000;
    uint32_t hrs = secs / 3600;
    uint32_t mins = (secs % 3600) / 60;
    snprintf(buf, sizeof(buf), "Uptime: %uh %um %us",
             (unsigned)hrs, (unsigned)mins, (unsigned)(secs % 60));
    tritium_theme::createLabel(run_panel, buf, true);

    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    snprintf(buf, sizeof(buf), "FreeRTOS tasks: %u", (unsigned)task_count);
    tritium_theme::createLabel(run_panel, buf, true);

    const esp_app_desc_t* app_desc = esp_app_get_description();
    if (app_desc) {
        snprintf(buf, sizeof(buf), "IDF: %s", app_desc->idf_ver);
        tritium_theme::createLabel(run_panel, buf, true);
        snprintf(buf, sizeof(buf), "Built: %s %s", app_desc->date, app_desc->time);
        tritium_theme::createLabel(run_panel, buf, true);
    }

    // --- Active services ---
    lv_obj_t* svc_panel = tritium_theme::createPanel(cont, "ACTIVE SERVICES");
    lv_obj_set_width(svc_panel, lv_pct(100));
    lv_obj_set_height(svc_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(svc_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(svc_panel, 24, 0);
    lv_obj_set_style_pad_gap(svc_panel, 2, 0);

    int svc_count = ServiceRegistry::count();
    for (int i = 0; i < svc_count; i++) {
        ServiceInterface* svc = ServiceRegistry::at(i);
        if (svc) {
            snprintf(buf, sizeof(buf), "[%d] %s", svc->initPriority(), svc->name());
            lv_obj_t* slbl = tritium_theme::createLabel(svc_panel, buf, true);
            lv_obj_set_style_text_color(slbl, T_GREEN, 0);
        }
    }
    if (svc_count == 0) {
        tritium_theme::createLabel(svc_panel, "No services registered", true);
    }
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
    lv_obj_set_style_text_font(copy, tritium_shell::uiSmallFont(), 0);
    lv_obj_set_style_text_align(copy, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(copy, lv_pct(100));
}

// ---------------------------------------------------------------------------
// Settings Tab: CLOCK / TIME
// ---------------------------------------------------------------------------

#if NTP_AVAILABLE
static NtpHAL s_clock_ntp;
#endif

static lv_obj_t* s_clock_time_lbl = nullptr;
static lv_obj_t* s_clock_date_lbl = nullptr;
static lv_obj_t* s_clock_ntp_status_lbl = nullptr;
static lv_timer_t* s_clock_timer = nullptr;

static const char* s_tz_labels[] = {
    "UTC",
    "US Eastern (EST5EDT)",
    "US Central (CST6CDT)",
    "US Mountain (MST7MDT)",
    "US Pacific (PST8PDT)",
    "US Alaska (AKST9AKDT)",
    "US Hawaii (HST10)",
    "Europe London (GMT0BST)",
    "Europe Berlin (CET-1CEST)",
    "Europe Moscow (MSK-3)",
    "Asia Tokyo (JST-9)",
    "Asia Shanghai (CST-8)",
    "Australia Sydney (AEST-10AEDT)",
};
static const char* s_tz_posix[] = {
    "UTC0",
    "EST5EDT,M3.2.0,M11.1.0",
    "CST6CDT,M3.2.0,M11.1.0",
    "MST7MDT,M3.2.0,M11.1.0",
    "PST8PDT,M3.2.0,M11.1.0",
    "AKST9AKDT,M3.2.0,M11.1.0",
    "HST10",
    "GMT0BST,M3.5.0/1,M10.5.0",
    "CET-1CEST,M3.5.0,M10.5.0/3",
    "MSK-3",
    "JST-9",
    "CST-8",
    "AEST-10AEDT,M10.1.0,M4.1.0/3",
};
static constexpr int TZ_COUNT = sizeof(s_tz_labels) / sizeof(s_tz_labels[0]);

static void clock_timer_cb(lv_timer_t* /*t*/) {
    if (!s_clock_time_lbl) return;
#ifndef SIMULATOR
    struct tm ti;
    time_t now = time(nullptr);
    localtime_r(&now, &ti);
    if (ti.tm_year > (2024 - 1900)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
        lv_label_set_text(s_clock_time_lbl, buf);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        lv_label_set_text(s_clock_date_lbl, buf);
    } else {
        lv_label_set_text(s_clock_time_lbl, "--:--:--");
        lv_label_set_text(s_clock_date_lbl, "NO TIME");
    }
#endif
}

static void clock_sync_cb(lv_event_t* /*e*/) {
#if NTP_AVAILABLE && !defined(SIMULATOR)
    lv_label_set_text(s_clock_ntp_status_lbl, "SYNCING...");
    lv_obj_set_style_text_color(s_clock_ntp_status_lbl, T_YELLOW, 0);
    lv_refr_now(nullptr);
    bool ok = s_clock_ntp.sync();
    if (ok) {
        lv_label_set_text(s_clock_ntp_status_lbl, "SYNCED");
        lv_obj_set_style_text_color(s_clock_ntp_status_lbl, T_GREEN, 0);
    } else {
        lv_label_set_text(s_clock_ntp_status_lbl, "SYNC FAILED");
        lv_obj_set_style_text_color(s_clock_ntp_status_lbl, T_MAGENTA, 0);
    }
#endif
}

static void clock_tz_cb(lv_event_t* e) {
    lv_obj_t* dd = (lv_obj_t*)lv_event_get_target(e);
    int idx = lv_dropdown_get_selected(dd);
    if (idx >= 0 && idx < TZ_COUNT) {
#if NTP_AVAILABLE
        s_clock_ntp.setTimezone(s_tz_posix[idx]);
#endif
#ifndef SIMULATOR
        setenv("TZ", s_tz_posix[idx], 1);
        tzset();
#endif
        // Save to settings
        TritiumSettings& settings = TritiumSettings::instance();
        settings.setString(SettingsDomain::SYSTEM, "timezone", s_tz_posix[idx]);
    }
}

static void settings_build_clock(lv_obj_t* cont) {
    s_clock_time_lbl = nullptr;
    s_clock_date_lbl = nullptr;
    s_clock_ntp_status_lbl = nullptr;
    if (s_clock_timer) { lv_timer_delete(s_clock_timer); s_clock_timer = nullptr; }

    // --- Current Time panel ---
    lv_obj_t* time_panel = tritium_theme::createPanel(cont, "CURRENT TIME");
    lv_obj_set_width(time_panel, lv_pct(100));
    lv_obj_set_height(time_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(time_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(time_panel, 24, 0);
    lv_obj_set_style_pad_gap(time_panel, 4, 0);
    lv_obj_set_flex_align(time_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_clock_time_lbl = lv_label_create(time_panel);
    lv_label_set_text(s_clock_time_lbl, "--:--:--");
    lv_obj_set_style_text_font(s_clock_time_lbl, tritium_shell::uiHeadingFont(), 0);
    lv_obj_set_style_text_color(s_clock_time_lbl, T_CYAN, 0);
    lv_obj_set_style_text_align(s_clock_time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_clock_time_lbl, lv_pct(100));

    s_clock_date_lbl = lv_label_create(time_panel);
    lv_label_set_text(s_clock_date_lbl, "----.--.--");
    lv_obj_set_style_text_color(s_clock_date_lbl, T_GHOST, 0);
    lv_obj_set_style_text_font(s_clock_date_lbl, tritium_shell::uiSmallFont(), 0);
    lv_obj_set_style_text_align(s_clock_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_clock_date_lbl, lv_pct(100));

    // Source info
#if RTC_AVAILABLE
    tritium_theme::createLabel(time_panel, "Source: NTP + RTC", true);
#else
    tritium_theme::createLabel(time_panel, "Source: NTP only (no RTC)", true);
#endif

    // --- NTP Sync panel ---
    lv_obj_t* ntp_panel = tritium_theme::createPanel(cont, "NTP SYNC");
    lv_obj_set_width(ntp_panel, lv_pct(100));
    lv_obj_set_height(ntp_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ntp_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(ntp_panel, 24, 0);
    lv_obj_set_style_pad_gap(ntp_panel, 6, 0);

    lv_obj_t* status_row = lv_obj_create(ntp_panel);
    lv_obj_set_size(status_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_all(status_row, 0, 0);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);

    tritium_theme::createLabel(status_row, "Status:");
    s_clock_ntp_status_lbl = tritium_theme::createLabel(status_row, "UNKNOWN", true);
    lv_obj_set_style_text_color(s_clock_ntp_status_lbl, T_GHOST, 0);

#if NTP_AVAILABLE && !defined(SIMULATOR)
    if (s_clock_ntp.isSynced()) {
        lv_label_set_text(s_clock_ntp_status_lbl, "SYNCED");
        lv_obj_set_style_text_color(s_clock_ntp_status_lbl, T_GREEN, 0);
    } else {
        lv_label_set_text(s_clock_ntp_status_lbl, "NOT SYNCED");
        lv_obj_set_style_text_color(s_clock_ntp_status_lbl, T_MAGENTA, 0);
    }
#endif

    lv_obj_t* sync_btn = tritium_theme::createButton(ntp_panel, "SYNC NOW", T_CYAN);
    lv_obj_set_width(sync_btn, lv_pct(100));
    lv_obj_add_event_cb(sync_btn, clock_sync_cb, LV_EVENT_CLICKED, nullptr);

    // --- Timezone panel ---
    lv_obj_t* tz_panel = tritium_theme::createPanel(cont, "TIMEZONE");
    lv_obj_set_width(tz_panel, lv_pct(100));
    lv_obj_set_height(tz_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tz_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(tz_panel, 24, 0);
    lv_obj_set_style_pad_gap(tz_panel, 6, 0);

    // Build dropdown option string
    char tz_opts[512];
    tz_opts[0] = '\0';
    for (int i = 0; i < TZ_COUNT; i++) {
        if (i > 0) strncat(tz_opts, "\n", sizeof(tz_opts) - strlen(tz_opts) - 1);
        strncat(tz_opts, s_tz_labels[i], sizeof(tz_opts) - strlen(tz_opts) - 1);
    }

    lv_obj_t* tz_dd = lv_dropdown_create(tz_panel);
    lv_dropdown_set_options(tz_dd, tz_opts);
    lv_obj_set_width(tz_dd, lv_pct(100));
    lv_obj_set_style_bg_color(tz_dd, T_SURFACE2, 0);
    lv_obj_set_style_bg_opa(tz_dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tz_dd, T_CYAN, 0);
    lv_obj_set_style_border_opa(tz_dd, LV_OPA_20, 0);
    lv_obj_set_style_border_width(tz_dd, 1, 0);
    lv_obj_set_style_text_color(tz_dd, T_TEXT, 0);
    lv_obj_set_style_radius(tz_dd, 4, 0);

    // Style the dropdown list popup
    lv_obj_t* list = lv_dropdown_get_list(tz_dd);
    if (list) {
        lv_obj_set_style_bg_color(list, T_SURFACE1, 0);
        lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(list, T_CYAN, 0);
        lv_obj_set_style_border_opa(list, LV_OPA_40, 0);
        lv_obj_set_style_text_color(list, T_TEXT, 0);
        lv_obj_set_style_bg_color(list, T_CYAN, (int)LV_PART_SELECTED | (int)LV_STATE_CHECKED);
        lv_obj_set_style_text_color(list, T_VOID, (int)LV_PART_SELECTED | (int)LV_STATE_CHECKED);
    }

    // Read saved timezone and select it
    TritiumSettings& settings = TritiumSettings::instance();
    const char* saved_tz = settings.getString(SettingsDomain::SYSTEM, "timezone", "UTC0");
    if (saved_tz && saved_tz[0]) {
        for (int i = 0; i < TZ_COUNT; i++) {
            if (strcmp(saved_tz, s_tz_posix[i]) == 0) {
                lv_dropdown_set_selected(tz_dd, i);
                break;
            }
        }
    }

    lv_obj_add_event_cb(tz_dd, clock_tz_cb, LV_EVENT_VALUE_CHANGED, nullptr);

#if RTC_AVAILABLE
    // --- RTC panel (boards with hardware RTC) ---
    lv_obj_t* rtc_panel = tritium_theme::createPanel(cont, "HARDWARE RTC");
    lv_obj_set_width(rtc_panel, lv_pct(100));
    lv_obj_set_height(rtc_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rtc_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(rtc_panel, 24, 0);
    lv_obj_set_style_pad_gap(rtc_panel, 4, 0);

    tritium_theme::createLabel(rtc_panel, "PCF85063 RTC available", true);
    tritium_theme::createLabel(rtc_panel, "RTC syncs from NTP automatically", true);
#endif

    // Start 1-second update timer
    s_clock_timer = lv_timer_create(clock_timer_cb, 1000, nullptr);
    clock_timer_cb(nullptr);  // Immediate first update
}

// ---------------------------------------------------------------------------
// Settings Tab: DEVELOPER
// ---------------------------------------------------------------------------

static lv_obj_t* s_dev_task_list = nullptr;
static lv_obj_t* s_dev_i2c_list = nullptr;
static lv_timer_t* s_dev_timer   = nullptr;

static void dev_refresh_tasks(lv_timer_t* t) {
    (void)t;
    if (!s_dev_task_list) return;
    lv_obj_clean(s_dev_task_list);

#ifndef SIMULATOR
    TaskStatus_t tasks[20];
    UBaseType_t n = uxTaskGetSystemState(tasks, 20, nullptr);

    // Sort by priority (highest first)
    for (UBaseType_t i = 0; i < n; i++) {
        for (UBaseType_t j = i + 1; j < n; j++) {
            if (tasks[j].uxCurrentPriority > tasks[i].uxCurrentPriority) {
                TaskStatus_t tmp = tasks[i];
                tasks[i] = tasks[j];
                tasks[j] = tmp;
            }
        }
    }

    const lv_font_t* font = tritium_shell::uiSmallFont();
    for (UBaseType_t i = 0; i < n; i++) {
        char buf[80];
        const char* state_str = "?";
        switch (tasks[i].eCurrentState) {
            case eRunning:   state_str = "RUN"; break;
            case eReady:     state_str = "RDY"; break;
            case eBlocked:   state_str = "BLK"; break;
            case eSuspended: state_str = "SUS"; break;
            case eDeleted:   state_str = "DEL"; break;
            default: break;
        }
        snprintf(buf, sizeof(buf), "%-16s P%d %s  stk:%lu",
                 tasks[i].pcTaskName,
                 (int)tasks[i].uxCurrentPriority,
                 state_str,
                 (unsigned long)tasks[i].usStackHighWaterMark);

        lv_obj_t* lbl = lv_label_create(s_dev_task_list);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, font, 0);

        // Color by state
        lv_color_t c = T_TEXT;
        if (tasks[i].eCurrentState == eRunning) c = T_GREEN;
        else if (tasks[i].usStackHighWaterMark < 256) c = T_MAGENTA;
        lv_obj_set_style_text_color(lbl, c, 0);
    }
#endif
}

static void dev_i2c_scan_cb(lv_event_t* e) {
    (void)e;
    if (!s_dev_i2c_list) return;
    lv_obj_clean(s_dev_i2c_list);

#ifndef SIMULATOR
    const i2c_port_t port = I2C_NUM_0;
    int found = 0;
    char buf[48];
    const lv_font_t* font = tritium_shell::uiSmallFont();

    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK) {
            snprintf(buf, sizeof(buf), "0x%02X  (%d)", addr, addr);
            lv_obj_t* lbl = lv_label_create(s_dev_i2c_list);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_font(lbl, font, 0);
            lv_obj_set_style_text_color(lbl, T_GREEN, 0);
            found++;
        }
    }

    if (found == 0) {
        lv_obj_t* lbl = lv_label_create(s_dev_i2c_list);
        lv_label_set_text(lbl, "No devices found");
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, T_GHOST, 0);
    } else {
        snprintf(buf, sizeof(buf), "%d device(s) found", found);
        lv_obj_t* lbl = lv_label_create(s_dev_i2c_list);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, T_CYAN, 0);
    }
#endif
}

static void settings_build_developer(lv_obj_t* cont) {
    s_dev_task_list = nullptr;
    s_dev_i2c_list = nullptr;
    if (s_dev_timer) { lv_timer_delete(s_dev_timer); s_dev_timer = nullptr; }

#ifndef SIMULATOR
    // --- Memory panel ---
    lv_obj_t* mem_panel = tritium_theme::createPanel(cont, "MEMORY");
    lv_obj_set_width(mem_panel, lv_pct(100));
    lv_obj_set_height(mem_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mem_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(mem_panel, 24, 0);
    lv_obj_set_style_pad_gap(mem_panel, 3, 0);

    {
        char buf[80];
        size_t free_h = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t min_h = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snprintf(buf, sizeof(buf), "Heap free: %uKB  min: %uKB",
                 (unsigned)(free_h / 1024), (unsigned)(min_h / 1024));
        lv_obj_t* lbl = tritium_theme::createLabel(mem_panel, buf, true);
        lv_obj_set_style_text_color(lbl, min_h < 20480 ? T_MAGENTA : T_TEXT, 0);
    }
    {
        char buf[80];
        size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t total_ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        snprintf(buf, sizeof(buf), "PSRAM free: %uKB / %uKB",
                 (unsigned)(free_ps / 1024), (unsigned)(total_ps / 1024));
        tritium_theme::createLabel(mem_panel, buf, true);
    }
    {
        char buf[40];
        snprintf(buf, sizeof(buf), "Uptime: %lus", (unsigned long)(millis() / 1000));
        tritium_theme::createLabel(mem_panel, buf, true);
    }

    // --- FreeRTOS tasks panel ---
    lv_obj_t* task_panel = tritium_theme::createPanel(cont, "FREERTOS TASKS");
    lv_obj_set_width(task_panel, lv_pct(100));
    lv_obj_set_height(task_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(task_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(task_panel, 24, 0);
    lv_obj_set_style_pad_gap(task_panel, 1, 0);

    s_dev_task_list = lv_obj_create(task_panel);
    lv_obj_set_width(s_dev_task_list, lv_pct(100));
    lv_obj_set_height(s_dev_task_list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_dev_task_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dev_task_list, 0, 0);
    lv_obj_set_style_pad_all(s_dev_task_list, 0, 0);
    lv_obj_set_flex_flow(s_dev_task_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_dev_task_list, 1, 0);

    dev_refresh_tasks(nullptr);
    s_dev_timer = lv_timer_create(dev_refresh_tasks, 3000, nullptr);

    // --- I2C bus scan panel ---
    lv_obj_t* i2c_panel = tritium_theme::createPanel(cont, "I2C BUS SCAN");
    lv_obj_set_width(i2c_panel, lv_pct(100));
    lv_obj_set_height(i2c_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(i2c_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(i2c_panel, 24, 0);
    lv_obj_set_style_pad_gap(i2c_panel, 4, 0);

    lv_obj_t* scan_btn = tritium_theme::createButton(i2c_panel, "SCAN I2C BUS", T_CYAN);
    lv_obj_set_width(scan_btn, lv_pct(100));
    lv_obj_add_event_cb(scan_btn, dev_i2c_scan_cb, LV_EVENT_CLICKED, nullptr);

    s_dev_i2c_list = lv_obj_create(i2c_panel);
    lv_obj_set_width(s_dev_i2c_list, lv_pct(100));
    lv_obj_set_height(s_dev_i2c_list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_dev_i2c_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dev_i2c_list, 0, 0);
    lv_obj_set_style_pad_all(s_dev_i2c_list, 0, 0);
    lv_obj_set_flex_flow(s_dev_i2c_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_dev_i2c_list, 2, 0);

    // --- System actions ---
    lv_obj_t* act_panel = tritium_theme::createPanel(cont, "SYSTEM ACTIONS");
    lv_obj_set_width(act_panel, lv_pct(100));
    lv_obj_set_height(act_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(act_panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(act_panel, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(act_panel, 24, 0);

    lv_obj_t* reboot_btn = tritium_theme::createButton(act_panel,
        LV_SYMBOL_REFRESH " Reboot", T_MAGENTA);
    lv_obj_add_event_cb(reboot_btn, [](lv_event_t* ev) {
        (void)ev;
        esp_restart();
    }, LV_EVENT_CLICKED, nullptr);

#else
    tritium_theme::createLabel(cont, "Developer tools not available in simulator");
#endif
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
    lv_obj_set_style_text_font(title, tritium_shell::uiHeadingFont(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, lv_pct(100));

    // Tagline
    lv_obj_t* tagline = lv_label_create(viewport);
    lv_label_set_text(tagline, "Software-Defined Edge Intelligence");
    lv_obj_set_style_text_color(tagline, T_GHOST, 0);
    lv_obj_set_style_text_font(tagline, tritium_shell::uiSmallFont(), 0);
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
    display_health_t dh2 = display_get_health();
    snprintf(buf, sizeof(buf), "Board: %s", dh2.board_name ? dh2.board_name : "Unknown");
    tritium_theme::createLabel(info, buf, true);

    // Display
    snprintf(buf, sizeof(buf), "Display: %s %dx%d",
             dh2.driver ? dh2.driver : "?",
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
    lv_obj_set_style_text_font(copy, tritium_shell::uiSmallFont(), 0);
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
    lv_obj_set_style_text_font(icon, tritium_shell::uiHeadingFont(), 0);

    // Value label
    lv_obj_t* val_label = tritium_theme::createLabel(viewport, "100%", true);
    lv_obj_set_style_text_font(val_label, tritium_shell::uiHeadingFont(), 0);
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

    // --- Connection detail ---
    s_wifi_detail_lbl = tritium_theme::createLabel(status_panel, "...", true);
    lv_obj_set_style_text_font(s_wifi_detail_lbl, tritium_shell::uiSmallFont(), 0);
    lv_obj_set_style_text_color(s_wifi_detail_lbl, T_GHOST, 0);

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
        WifiManager::_instance ? WifiManager::_instance->isAPMode() : false);
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
static lv_obj_t* s_sysmon_heap_min   = nullptr;
static lv_obj_t* s_sysmon_psram_bar  = nullptr;
static lv_obj_t* s_sysmon_psram_lbl  = nullptr;
static lv_obj_t* s_sysmon_loop_lbl   = nullptr;
static lv_obj_t* s_sysmon_uptime_lbl = nullptr;
static lv_obj_t* s_sysmon_temp_lbl   = nullptr;
static lv_obj_t* s_sysmon_wifi_lbl   = nullptr;
static lv_obj_t* s_sysmon_tasks_lbl  = nullptr;
static lv_obj_t* s_sysmon_storage_lbl = nullptr;
static lv_obj_t* s_sysmon_task_list  = nullptr;
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

    // Heap watermark + fragmentation
    if (s_sysmon_heap_min) {
        uint32_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        int frag_pct = (free_heap > 0) ? (int)(100 - (largest * 100 / free_heap)) : 0;
        snprintf(buf, sizeof(buf), "Min: %uKB  Largest: %uKB  Frag: %d%%",
                 (unsigned)(min_heap / 1024), (unsigned)(largest / 1024), frag_pct);
        lv_label_set_text(s_sysmon_heap_min, buf);
        lv_obj_set_style_text_color(s_sysmon_heap_min,
            frag_pct > 50 ? T_MAGENTA : frag_pct > 25 ? T_YELLOW : T_GHOST, 0);
    }

    // Task count
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    snprintf(buf, sizeof(buf), "Tasks: %u", (unsigned)task_count);
    lv_label_set_text(s_sysmon_tasks_lbl, buf);

    // Task detail list
    if (s_sysmon_task_list && task_count > 0 && task_count <= 32) {
        lv_obj_clean(s_sysmon_task_list);
        TaskStatus_t* task_arr = (TaskStatus_t*)malloc(task_count * sizeof(TaskStatus_t));
        if (task_arr) {
            UBaseType_t filled = uxTaskGetSystemState(task_arr, task_count, nullptr);
            for (UBaseType_t i = 0; i < filled; i++) {
                char tbuf[80];
                const char* state_str = "?";
                switch (task_arr[i].eCurrentState) {
                    case eRunning:   state_str = "RUN"; break;
                    case eReady:     state_str = "RDY"; break;
                    case eBlocked:   state_str = "BLK"; break;
                    case eSuspended: state_str = "SUS"; break;
                    case eDeleted:   state_str = "DEL"; break;
                    default: break;
                }
                snprintf(tbuf, sizeof(tbuf), "%-16s P%u %s  stk:%u",
                         task_arr[i].pcTaskName,
                         (unsigned)task_arr[i].uxCurrentPriority,
                         state_str,
                         (unsigned)task_arr[i].usStackHighWaterMark);
                lv_obj_t* tlbl = tritium_theme::createLabel(s_sysmon_task_list, tbuf, true);
                lv_obj_set_style_text_font(tlbl, tritium_shell::uiSmallFont(), 0);
                // Color by stack watermark: red if <200, yellow if <500
                lv_color_t sc = (task_arr[i].usStackHighWaterMark < 200) ? T_MAGENTA :
                                (task_arr[i].usStackHighWaterMark < 500) ? T_YELLOW : T_GREEN;
                lv_obj_set_style_text_color(tlbl, sc, 0);
            }
            free(task_arr);
        }
    }

    // Storage
    if (s_sysmon_storage_lbl) {
        char sbuf[128];
        int spos = 0;
#if FILES_FS_AVAILABLE
        FsHAL fs;
        if (fs.init()) {
            spos += snprintf(sbuf + spos, sizeof(sbuf) - spos,
                "LittleFS: %uKB/%uKB  ",
                (unsigned)(fs.usedBytes() / 1024),
                (unsigned)(fs.totalBytes() / 1024));
        }
#endif
#if FILES_SD_AVAILABLE || STORAGE_SVC_AVAILABLE
        SDCardHAL* sd = nullptr;
#if STORAGE_SVC_AVAILABLE
        auto* ssvc = ServiceRegistry::getAs<StorageService>("storage");
        if (ssvc) sd = &ssvc->sd();
#endif
        if (sd && sd->isMounted()) {
            spos += snprintf(sbuf + spos, sizeof(sbuf) - spos,
                "SD: %uMB/%uMB",
                (unsigned)(sd->usedBytes() / (1024*1024)),
                (unsigned)(sd->totalBytes() / (1024*1024)));
        } else {
            spos += snprintf(sbuf + spos, sizeof(sbuf) - spos, "SD: --");
        }
#endif
        if (spos == 0) snprintf(sbuf, sizeof(sbuf), "No storage");
        lv_label_set_text(s_sysmon_storage_lbl, sbuf);
    }

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

    s_sysmon_heap_min = tritium_theme::createLabel(mem_panel, "Min free: --", true);
    lv_obj_set_style_text_font(s_sysmon_heap_min, tritium_shell::uiSmallFont(), 0);
    lv_obj_set_style_text_color(s_sysmon_heap_min, T_GHOST, 0);

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

    // --- Tasks panel ---
    lv_obj_t* task_panel = tritium_theme::createPanel(viewport, "FREERTOS TASKS");
    lv_obj_set_width(task_panel, lv_pct(100));
    lv_obj_set_height(task_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(task_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(task_panel, 24, 0);
    lv_obj_set_style_pad_gap(task_panel, 2, 0);

    s_sysmon_task_list = lv_obj_create(task_panel);
    lv_obj_set_width(s_sysmon_task_list, lv_pct(100));
    lv_obj_set_height(s_sysmon_task_list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_sysmon_task_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sysmon_task_list, 0, 0);
    lv_obj_set_style_pad_all(s_sysmon_task_list, 0, 0);
    lv_obj_set_style_pad_gap(s_sysmon_task_list, 1, 0);
    lv_obj_set_flex_flow(s_sysmon_task_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_sysmon_task_list, LV_OBJ_FLAG_SCROLLABLE);

    // --- Network panel ---
    lv_obj_t* net_panel = tritium_theme::createPanel(viewport, "NETWORK");
    lv_obj_set_width(net_panel, lv_pct(100));
    lv_obj_set_height(net_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(net_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(net_panel, 24, 0);
    lv_obj_set_style_pad_gap(net_panel, 4, 0);

    s_sysmon_wifi_lbl = tritium_theme::createLabel(net_panel, "WiFi: --", true);

    // --- Storage panel ---
    lv_obj_t* stor_panel = tritium_theme::createPanel(viewport, "STORAGE");
    lv_obj_set_width(stor_panel, lv_pct(100));
    lv_obj_set_height(stor_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stor_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(stor_panel, 24, 0);
    lv_obj_set_style_pad_gap(stor_panel, 4, 0);

    s_sysmon_storage_lbl = tritium_theme::createLabel(stor_panel, "Storage: --", true);

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

// storage_service + service_registry included at file top (outside namespace)

// ===========================================================================
//  File Explorer state
// ===========================================================================
static char s_files_cwd[128] = "/";
static lv_obj_t* s_files_path_lbl    = nullptr;
static lv_obj_t* s_files_list        = nullptr;
static lv_obj_t* s_files_info_lbl    = nullptr;
static lv_obj_t* s_files_storage_bar = nullptr;
static lv_obj_t* s_files_viewport    = nullptr;  // for rebuilding
static bool s_files_use_sd           = true;      // default to SD card

// Get SD card HAL from StorageService (or nullptr)
#if STORAGE_SVC_AVAILABLE
static StorageService* storage_svc() {
    return ServiceRegistry::getAs<StorageService>("storage");
}
static SDCardHAL* sd_hal() {
    auto* svc = storage_svc();
    return svc ? &svc->sd() : nullptr;
}
static FsHAL* fs_hal() {
    auto* svc = storage_svc();
    return svc ? &svc->fs() : nullptr;
}
#elif FILES_SD_AVAILABLE
static SDCardHAL s_sd;
static SDCardHAL* sd_hal() { return &s_sd; }
#else
static SDCardHAL* sd_hal() { return nullptr; }
#endif

static void files_navigate(const char* path);
static void files_show_viewer(const char* filepath, const char* name);

#ifndef SIMULATOR

// ---- Format size for display ----
static void format_size(char* buf, size_t buflen, size_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, buflen, "%.1fGB", bytes / (1024.0f * 1024.0f * 1024.0f));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, buflen, "%.1fMB", bytes / (1024.0f * 1024.0f));
    } else if (bytes >= 1024) {
        snprintf(buf, buflen, "%.1fKB", bytes / 1024.0f);
    } else {
        snprintf(buf, buflen, "%uB", (unsigned)bytes);
    }
}

// ---- Check if file is text-viewable ----
static bool is_text_file(const char* name) {
    const char* ext = strrchr(name, '.');
    if (!ext) return false;
    ext++;
    static const char* text_exts[] = {
        "txt", "log", "json", "xml", "csv", "ini", "cfg", "conf",
        "md", "htm", "html", "css", "js", "h", "c", "cpp", "py",
        nullptr
    };
    for (int i = 0; text_exts[i]; i++) {
        if (strcasecmp(ext, text_exts[i]) == 0) return true;
    }
    return false;
}

// ---- Item click: directory = navigate, file = view ----
static void files_item_cb(lv_event_t* e) {
    const char* path = (const char*)lv_event_get_user_data(e);
    if (!path) return;
    // Check if it ends with '/' (directory)
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] == '/') {
        files_navigate(path);
    } else {
        // Extract filename from path
        const char* name = strrchr(path, '/');
        name = name ? name + 1 : path;
        files_show_viewer(path, name);
    }
}

// ---- Back button (used in file viewer close) ----
static void files_back_cb(lv_event_t* e) {
    (void)e;
    char parent[128];
    strncpy(parent, s_files_cwd, sizeof(parent));
    parent[sizeof(parent) - 1] = '\0';
    size_t len = strlen(parent);
    if (len > 1 && parent[len - 1] == '/') parent[len - 1] = '\0';
    char* last = strrchr(parent, '/');
    if (last && last != parent) {
        *(last + 1) = '\0';
    } else {
        strcpy(parent, "/");
    }
    files_navigate(parent);
}

// ---- Storage toggle callback ----
static void files_storage_toggle_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    s_files_use_sd = lv_obj_has_state(sw, LV_STATE_CHECKED);
    strcpy(s_files_cwd, "/");
    files_navigate("/");
}

// ---- Delete file callback ----
static void files_delete_cb(lv_event_t* e) {
    const char* path = (const char*)lv_event_get_user_data(e);
    if (!path) return;

    const char* mount = s_files_use_sd ? "/sdcard" : "/spiffs";
    char fullpath[200];
    snprintf(fullpath, sizeof(fullpath), "%s%s", mount, path);

    bool ok = false;
    struct stat st;
    if (stat(fullpath, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            ok = (rmdir(fullpath) == 0);
        } else {
            ok = (unlink(fullpath) == 0);
        }
    }

    // Refresh current directory
    files_navigate(s_files_cwd);
}

// ---- Format SD callback ----
static void files_format_cb(lv_event_t* e) {
    (void)e;
    auto* sd = sd_hal();
    if (sd && sd->isMounted()) {
        lv_label_set_text(s_files_info_lbl, "Formatting SD card...");
        lv_refr_now(nullptr);
        bool ok = sd->format();
        if (ok) {
            lv_label_set_text(s_files_info_lbl, "Format complete");
        } else {
            lv_label_set_text(s_files_info_lbl, "Format FAILED");
        }
        strcpy(s_files_cwd, "/");
        files_navigate("/");
    }
}

// ---- Mount/remount SD callback ----
static void files_mount_cb(lv_event_t* e) {
    (void)e;
    auto* sd = sd_hal();
    if (!sd) return;
    if (sd->isMounted()) {
        sd->deinit();
    }
    bool ok = sd->init();
    if (ok) {
        lv_label_set_text(s_files_info_lbl, "SD card mounted");
    } else {
        lv_label_set_text(s_files_info_lbl, "SD mount failed — no card?");
    }
    files_navigate(s_files_cwd);
}

// ---- Close file viewer, go back to directory ----
static void files_viewer_close_cb(lv_event_t* e) {
    (void)e;
    files_navigate(s_files_cwd);
}

// ---- File viewer: shows text content ----
static void files_show_viewer(const char* filepath, const char* name) {
    if (!s_files_list) return;
    lv_obj_clean(s_files_list);

    // Header row with filename and close button
    lv_obj_t* hdr = lv_obj_create(s_files_list);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 2, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    char title[80];
    snprintf(title, sizeof(title), LV_SYMBOL_FILE " %s", name);
    lv_obj_t* title_lbl = tritium_theme::createLabel(hdr, title);
    lv_obj_set_flex_grow(title_lbl, 1);
    lv_obj_set_style_text_color(title_lbl, T_CYAN, 0);

    lv_obj_t* close_btn = tritium_theme::createButton(hdr, LV_SYMBOL_CLOSE);
    lv_obj_add_event_cb(close_btn, files_viewer_close_cb, LV_EVENT_CLICKED, nullptr);

    // Build full filesystem path
    const char* mount = s_files_use_sd ? "/sdcard" : "/spiffs";
    char fullpath[200];
    snprintf(fullpath, sizeof(fullpath), "%s%s", mount, filepath);

    // Get file size
    struct stat st;
    size_t fsize = 0;
    if (stat(fullpath, &st) == 0) fsize = st.st_size;

    char size_str[32];
    format_size(size_str, sizeof(size_str), fsize);

    // File info
    char info[80];
    snprintf(info, sizeof(info), "Size: %s", size_str);
    lv_obj_t* info_lbl = tritium_theme::createLabel(s_files_list, info, true);
    lv_obj_set_style_text_color(info_lbl, T_GHOST, 0);

    // Separator
    lv_obj_t* sep = lv_obj_create(s_files_list);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, T_CYAN, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_20, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    if (!is_text_file(name)) {
        tritium_theme::createLabel(s_files_list, "Binary file — preview not available");
        return;
    }

    // Read file content (cap at 4KB for display)
    FILE* f = fopen(fullpath, "r");
    if (!f) {
        tritium_theme::createLabel(s_files_list, "Cannot open file");
        return;
    }

    static constexpr size_t TEXT_BUF_SIZE = 4096;
    char* text_buf = (char*)malloc(TEXT_BUF_SIZE);
    if (!text_buf) { fclose(f); tritium_theme::createLabel(s_files_list, "Out of memory"); return; }
    size_t read_len = fread(text_buf, 1, TEXT_BUF_SIZE - 1, f);
    text_buf[read_len] = '\0';
    fclose(f);

    // Text content in mono label (lv_label_set_text copies the string)
    lv_obj_t* content = lv_label_create(s_files_list);
    lv_label_set_text(content, text_buf);
    free(text_buf);
    lv_label_set_long_mode(content, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_style_text_color(content, T_TEXT, 0);
    lv_obj_set_style_text_font(content, tritium_shell::uiSmallFont(), 0);

    if (fsize > TEXT_BUF_SIZE - 1) {
        char trunc[48];
        snprintf(trunc, sizeof(trunc), "[ truncated — showing first 4KB of %s ]", size_str);
        lv_obj_t* trunc_lbl = tritium_theme::createLabel(s_files_list, trunc, true);
        lv_obj_set_style_text_color(trunc_lbl, T_GHOST, 0);
    }
}

// ---- Update storage info bar and bar widget ----
static void files_update_info() {
    if (!s_files_info_lbl) return;

    uint64_t total = 0, used = 0;
    const char* fs_type = "---";

    if (s_files_use_sd) {
        auto* sd = sd_hal();
        if (sd && sd->isMounted()) {
            total = sd->totalBytes();
            used = sd->usedBytes();
            fs_type = sd->getFilesystemType();
        }
    } else {
#if STORAGE_SVC_AVAILABLE
        auto* fsh = fs_hal();
        if (fsh && fsh->isReady()) {
            total = fsh->totalBytes();
            used = fsh->usedBytes();
            fs_type = "LittleFS";
        }
#elif FILES_FS_AVAILABLE
        FsHAL fs;
        if (fs.isReady()) {
            total = fs.totalBytes();
            used = fs.usedBytes();
            fs_type = "LittleFS";
        }
#endif
    }

    if (total > 0) {
        char used_str[16], total_str[16];
        format_size(used_str, sizeof(used_str), (size_t)used);
        format_size(total_str, sizeof(total_str), (size_t)total);

        char info[80];
        snprintf(info, sizeof(info), "%s: %s / %s (%s)",
                 s_files_use_sd ? "SD" : "LFS", used_str, total_str, fs_type);
        lv_label_set_text(s_files_info_lbl, info);

        if (s_files_storage_bar) {
            int pct = (int)(used * 100 / total);
            lv_bar_set_value(s_files_storage_bar, pct, LV_ANIM_ON);
            lv_color_t c = (pct < 70) ? T_CYAN : (pct < 90) ? T_YELLOW : T_MAGENTA;
            lv_obj_set_style_bg_color(s_files_storage_bar, c, LV_PART_INDICATOR);
        }
    } else {
        lv_label_set_text(s_files_info_lbl,
                          s_files_use_sd ? "SD: not mounted" : "LittleFS: not available");
        if (s_files_storage_bar) {
            lv_bar_set_value(s_files_storage_bar, 0, LV_ANIM_OFF);
        }
    }
}

// ---- Navigate directory ----
static void files_navigate(const char* path) {
    strncpy(s_files_cwd, path, sizeof(s_files_cwd));
    s_files_cwd[sizeof(s_files_cwd) - 1] = '\0';
    if (s_files_path_lbl) lv_label_set_text(s_files_path_lbl, s_files_cwd);
    if (!s_files_list) return;
    lv_obj_clean(s_files_list);

    const char* mount = s_files_use_sd ? "/sdcard" : "/spiffs";
    char fullpath[200];
    snprintf(fullpath, sizeof(fullpath), "%s%s", mount,
             s_files_cwd[0] == '/' ? s_files_cwd : "/");

    DIR* dir = opendir(fullpath);
    if (!dir) {
        if (s_files_use_sd) {
            tritium_theme::createLabel(s_files_list,
                "SD card not available.\nInsert card and tap Mount.");
        } else {
            tritium_theme::createLabel(s_files_list, "Cannot open directory");
        }
        files_update_info();
        return;
    }

    struct dirent* entry;
    // Static ring buffer for path strings (kept alive for callbacks)
    static char path_buf[12][160];
    static int path_idx = 0;

    int file_count = 0;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        bool is_dir = (entry->d_type == DT_DIR);
        file_count++;

        lv_obj_t* row = lv_obj_create(s_files_list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 3, 0);
        lv_obj_set_style_pad_gap(row, 4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // Highlight on press
        lv_obj_set_style_bg_color(row, T_SURFACE3, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

        // Icon + name
        const char* icon = is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
        char label[80];
        snprintf(label, sizeof(label), "%s %s", icon, entry->d_name);
        lv_obj_t* name_lbl = tritium_theme::createLabel(row, label);
        lv_obj_set_flex_grow(name_lbl, 1);
        lv_obj_set_style_text_color(name_lbl, is_dir ? T_CYAN : T_TEXT, 0);

        // Build callback path (before size/delete so we can reuse pi)
        int pi = path_idx % 12;
        path_idx++;
        if (is_dir) {
            if (strcmp(s_files_cwd, "/") == 0)
                snprintf(path_buf[pi], sizeof(path_buf[pi]), "/%s/", entry->d_name);
            else
                snprintf(path_buf[pi], sizeof(path_buf[pi]), "%s%s/",
                         s_files_cwd, entry->d_name);
        } else {
            if (strcmp(s_files_cwd, "/") == 0)
                snprintf(path_buf[pi], sizeof(path_buf[pi]), "/%s", entry->d_name);
            else
                snprintf(path_buf[pi], sizeof(path_buf[pi]), "%s%s",
                         s_files_cwd, entry->d_name);
        }

        // File size (right-aligned)
        if (!is_dir) {
            char fpath[200];
            snprintf(fpath, sizeof(fpath), "%s/%s", fullpath, entry->d_name);
            struct stat st;
            size_t fsize = 0;
            if (stat(fpath, &st) == 0) fsize = st.st_size;
            char size_str[16];
            format_size(size_str, sizeof(size_str), fsize);
            lv_obj_t* sz_lbl = tritium_theme::createLabel(row, size_str, true);
            lv_obj_set_style_text_color(sz_lbl, T_GHOST, 0);
        }

        // Delete button
        lv_obj_t* del_btn = lv_btn_create(row);
        lv_obj_set_size(del_btn, 24, 20);
        lv_obj_set_style_bg_opa(del_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(del_btn, T_MAGENTA, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(del_btn, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(del_btn, 0, 0);
        lv_obj_set_style_pad_all(del_btn, 0, 0);
        lv_obj_t* del_icon = lv_label_create(del_btn);
        lv_label_set_text(del_icon, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(del_icon, T_GHOST, 0);
        lv_obj_set_style_text_color(del_icon, T_MAGENTA, LV_STATE_PRESSED);
        lv_obj_center(del_icon);
        lv_obj_add_event_cb(del_btn, files_delete_cb, LV_EVENT_CLICKED, path_buf[pi]);

        // Row click navigates/views
        lv_obj_add_event_cb(row, files_item_cb, LV_EVENT_CLICKED, path_buf[pi]);
    }
    closedir(dir);

    if (file_count == 0) {
        lv_obj_t* empty = tritium_theme::createLabel(s_files_list, "  (empty directory)");
        lv_obj_set_style_text_color(empty, T_GHOST, 0);
    }

    files_update_info();
}

#else  // SIMULATOR

static void files_navigate(const char* path) { (void)path; }
static void files_show_viewer(const char* filepath, const char* name) {
    (void)filepath; (void)name;
}

#endif  // SIMULATOR

// ===========================================================================
//  Storage App — storage overview + file browser sub-view
// ===========================================================================

// State: are we in the overview or browsing a volume?
static bool s_storage_browsing = false;

// ---- Helper: create a usage bar inline ----
static lv_obj_t* create_usage_bar(lv_obj_t* parent, int pct) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 5);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, T_SURFACE3, LV_PART_MAIN);
    lv_color_t c = (pct < 70) ? T_CYAN : (pct < 90) ? T_YELLOW : T_MAGENTA;
    lv_obj_set_style_bg_color(bar, c, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
    return bar;
}

// ---- Helper: transparent row ----
static lv_obj_t* create_info_row(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

// Forward declarations
static void storage_show_overview();
static void storage_confirm_format(bool is_sd);
static void storage_show_sd_test();

// ---- Confirmation dialog for format ----
static void storage_confirm_format(bool is_sd) {
    lv_obj_t* vp = s_files_viewport;
    if (!vp) return;
    lv_obj_clean(vp);

    lv_obj_t* panel = tritium_theme::createPanel(vp, is_sd ? "FORMAT SD CARD" : "FORMAT INTERNAL FLASH");
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(panel, 28, 0);
    lv_obj_set_style_pad_gap(panel, 8, 0);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* warn_icon = lv_label_create(panel);
    lv_label_set_text(warn_icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(warn_icon, tritium_shell::uiHeadingFont(), 0);
    lv_obj_set_style_text_color(warn_icon, T_MAGENTA, 0);

    tritium_theme::createLabel(panel, "This will ERASE ALL DATA");
    lv_obj_t* note = tritium_theme::createLabel(panel,
        is_sd ? "The SD card will be reformatted as FAT32."
              : "Internal flash will be reformatted as LittleFS.");
    lv_obj_set_style_text_color(note, T_GHOST, 0);

    lv_obj_t* btn_row = create_info_row(panel);

    lv_obj_t* cancel = tritium_theme::createButton(btn_row, LV_SYMBOL_CLOSE " Cancel", T_CYAN);
    lv_obj_add_event_cb(cancel, [](lv_event_t* ev) {
        (void)ev;
        storage_show_overview();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* confirm = tritium_theme::createButton(btn_row,
        LV_SYMBOL_TRASH " Format Now", T_MAGENTA);
    lv_obj_add_event_cb(confirm, [](lv_event_t* ev) {
        bool fmt_sd = (bool)(intptr_t)lv_event_get_user_data(ev);
        // Show progress
        lv_obj_t* vp = s_files_viewport;
        lv_obj_clean(vp);
        lv_obj_t* lbl = tritium_theme::createLabel(vp, "Formatting...");
        lv_obj_set_style_text_color(lbl, T_YELLOW, 0);
        lv_refr_now(nullptr);

        bool ok = false;
        if (fmt_sd) {
            auto* sd = sd_hal();
            if (sd) ok = sd->format();
        } else {
#if STORAGE_SVC_AVAILABLE
            auto* fsh = fs_hal();
            if (fsh) ok = fsh->format();
#elif FILES_FS_AVAILABLE
            FsHAL fsh;
            ok = fsh.format();
#endif
        }

        lv_obj_clean(vp);
        if (ok) {
            lv_obj_t* ok_lbl = tritium_theme::createLabel(vp, LV_SYMBOL_OK " Format complete");
            lv_obj_set_style_text_color(ok_lbl, T_GREEN, 0);
        } else {
            lv_obj_t* fail_lbl = tritium_theme::createLabel(vp, LV_SYMBOL_CLOSE " Format FAILED");
            lv_obj_set_style_text_color(fail_lbl, T_MAGENTA, 0);
        }
        lv_obj_t* back = tritium_theme::createButton(vp, LV_SYMBOL_LEFT " Back", T_CYAN);
        lv_obj_add_event_cb(back, [](lv_event_t* e2) {
            (void)e2;
            storage_show_overview();
        }, LV_EVENT_CLICKED, nullptr);
    }, LV_EVENT_CLICKED, (void*)(intptr_t)is_sd);
}

// ---- SD card test/diagnostics panel ----
static void storage_show_sd_test() {
    lv_obj_t* vp = s_files_viewport;
    if (!vp) return;
    lv_obj_clean(vp);

    // Top bar: back to overview
    lv_obj_t* top = create_info_row(vp);
    lv_obj_t* back = tritium_theme::createButton(top, LV_SYMBOL_LEFT " Storage");
    lv_obj_add_event_cb(back, [](lv_event_t* ev) {
        (void)ev;
        storage_show_overview();
    }, LV_EVENT_CLICKED, nullptr);
    tritium_theme::createLabel(top, "SD CARD DIAGNOSTICS");

    auto* sd = sd_hal();
    if (!sd || !sd->isMounted()) {
        tritium_theme::createLabel(vp, "SD card not mounted");
        return;
    }

    // Card info panel
    lv_obj_t* info_panel = tritium_theme::createPanel(vp, "CARD INFO");
    lv_obj_set_width(info_panel, lv_pct(100));
    lv_obj_set_height(info_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(info_panel, 24, 0);
    lv_obj_set_style_pad_gap(info_panel, 2, 0);

    char buf[80];
    char used_str[16], total_str[16], free_str[16];
    format_size(used_str, sizeof(used_str), (size_t)sd->usedBytes());
    format_size(total_str, sizeof(total_str), (size_t)sd->totalBytes());
    format_size(free_str, sizeof(free_str), (size_t)(sd->totalBytes() - sd->usedBytes()));

    snprintf(buf, sizeof(buf), "Type: %s", sd->getFilesystemType());
    tritium_theme::createLabel(info_panel, buf, true);
    snprintf(buf, sizeof(buf), "Total: %s", total_str);
    tritium_theme::createLabel(info_panel, buf, true);
    snprintf(buf, sizeof(buf), "Used:  %s", used_str);
    tritium_theme::createLabel(info_panel, buf, true);
    snprintf(buf, sizeof(buf), "Free:  %s", free_str);
    tritium_theme::createLabel(info_panel, buf, true);

    uint64_t total_b = sd->totalBytes();
    int pct = total_b ? (int)(sd->usedBytes() * 100 / total_b) : 0;
    create_usage_bar(info_panel, pct);

    // Run test button + results area
    lv_obj_t* test_panel = tritium_theme::createPanel(vp, "READ/WRITE TEST");
    lv_obj_set_width(test_panel, lv_pct(100));
    lv_obj_set_flex_grow(test_panel, 1);
    lv_obj_set_flex_flow(test_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(test_panel, 24, 0);
    lv_obj_set_style_pad_gap(test_panel, 4, 0);

    static lv_obj_t* s_test_results = nullptr;
    s_test_results = lv_obj_create(test_panel);
    lv_obj_set_size(s_test_results, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_test_results, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_test_results, 0, 0);
    lv_obj_set_style_pad_all(s_test_results, 0, 0);
    lv_obj_set_flex_flow(s_test_results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_test_results, 2, 0);

    tritium_theme::createLabel(s_test_results,
        "Tap Run to test read/write speed\nand data integrity.", false);

    lv_obj_t* run_btn = tritium_theme::createButton(test_panel,
        LV_SYMBOL_CHARGE " Run Test", T_GREEN);
    lv_obj_add_event_cb(run_btn, [](lv_event_t* ev) {
        (void)ev;
        if (!s_test_results) return;
        lv_obj_clean(s_test_results);

        lv_obj_t* prog_lbl = tritium_theme::createLabel(s_test_results, "Running test...");
        lv_obj_set_style_text_color(prog_lbl, T_YELLOW, 0);
        lv_refr_now(nullptr);

        auto* sd = sd_hal();
        if (!sd || !sd->isMounted()) {
            lv_obj_clean(s_test_results);
            lv_obj_t* err = tritium_theme::createLabel(s_test_results, "SD not mounted");
            lv_obj_set_style_text_color(err, T_MAGENTA, 0);
            return;
        }

        auto r = sd->runTest(5, 4096);

        lv_obj_clean(s_test_results);

        char line[80];
        bool pass = r.mount_ok && r.write_ok && r.read_ok && r.verify_ok;

        lv_obj_t* status = tritium_theme::createLabel(s_test_results,
            pass ? LV_SYMBOL_OK " ALL TESTS PASSED" : LV_SYMBOL_CLOSE " TEST FAILED");
        lv_obj_set_style_text_color(status, pass ? T_GREEN : T_MAGENTA, 0);

        snprintf(line, sizeof(line), "Write: %u KB/s", r.write_speed_kbps);
        lv_obj_t* w_lbl = tritium_theme::createLabel(s_test_results, line, true);
        lv_obj_set_style_text_color(w_lbl, T_CYAN, 0);

        snprintf(line, sizeof(line), "Read:  %u KB/s", r.read_speed_kbps);
        lv_obj_t* r_lbl = tritium_theme::createLabel(s_test_results, line, true);
        lv_obj_set_style_text_color(r_lbl, T_CYAN, 0);

        snprintf(line, sizeof(line), "Duration: %u ms  Cycles: %d", r.test_duration_ms, r.cycles_completed);
        tritium_theme::createLabel(s_test_results, line, true);

        snprintf(line, sizeof(line), "Mount:%s  Write:%s  Read:%s  Verify:%s",
            r.mount_ok ? "OK" : "FAIL", r.write_ok ? "OK" : "FAIL",
            r.read_ok ? "OK" : "FAIL", r.verify_ok ? "OK" : "FAIL");
        tritium_theme::createLabel(s_test_results, line, true);

        snprintf(line, sizeof(line), "Mkdir:%s  Delete:%s",
            r.mkdir_ok ? "OK" : "FAIL", r.delete_ok ? "OK" : "FAIL");
        tritium_theme::createLabel(s_test_results, line, true);
    }, LV_EVENT_CLICKED, nullptr);
}

// ---- Browse volume callback ----
// Helper: create mkdir callback for current directory
static void files_mkdir_cb(lv_event_t* e) {
    (void)e;
    // Create a "new_dir" folder in the current directory
    const char* mount = s_files_use_sd ? "/sdcard" : "/spiffs";
    char fullpath[200];
    // Find a unique name
    for (int i = 0; i < 100; i++) {
        if (i == 0)
            snprintf(fullpath, sizeof(fullpath), "%s%snew_folder", mount,
                     strcmp(s_files_cwd, "/") == 0 ? "/" : s_files_cwd);
        else
            snprintf(fullpath, sizeof(fullpath), "%s%snew_folder_%d", mount,
                     strcmp(s_files_cwd, "/") == 0 ? "/" : s_files_cwd, i);
        struct stat st;
        if (stat(fullpath, &st) != 0) break;  // Doesn't exist, use this name
    }
    // Use POSIX mkdir since we have the full VFS path
    mkdir(fullpath, 0755);
    files_navigate(s_files_cwd);
}

static void storage_browse_sd_cb(lv_event_t* e) {
    (void)e;
    s_files_use_sd = true;
    s_storage_browsing = true;
    strcpy(s_files_cwd, "/");

    lv_obj_t* vp = s_files_viewport;
    lv_obj_clean(vp);

    // Top bar: back + up + path + mkdir
    lv_obj_t* top = create_info_row(vp);

    lv_obj_t* back = tritium_theme::createButton(top, LV_SYMBOL_LEFT " Storage");
    lv_obj_add_event_cb(back, [](lv_event_t* ev) {
        (void)ev;
        s_storage_browsing = false;
        storage_show_overview();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* up_btn = tritium_theme::createButton(top, LV_SYMBOL_UP);
    lv_obj_add_event_cb(up_btn, files_back_cb, LV_EVENT_CLICKED, nullptr);

    s_files_path_lbl = tritium_theme::createLabel(top, "/", true);
    lv_obj_set_flex_grow(s_files_path_lbl, 1);
    lv_obj_set_style_text_color(s_files_path_lbl, T_CYAN, 0);

    lv_obj_t* mkdir_btn = tritium_theme::createButton(top, LV_SYMBOL_PLUS);
    lv_obj_add_event_cb(mkdir_btn, files_mkdir_cb, LV_EVENT_CLICKED, nullptr);

    // File list panel
    lv_obj_t* list_panel = tritium_theme::createPanel(vp, "SD CARD");
    lv_obj_set_width(list_panel, lv_pct(100));
    lv_obj_set_flex_grow(list_panel, 1);
    lv_obj_set_flex_flow(list_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(list_panel, 24, 0);
    lv_obj_set_style_pad_gap(list_panel, 2, 0);

    s_files_list = lv_obj_create(list_panel);
    lv_obj_set_size(s_files_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_files_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_files_list, 0, 0);
    lv_obj_set_style_pad_all(s_files_list, 0, 0);
    lv_obj_set_flex_flow(s_files_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_files_list, 1, 0);

    // Storage info footer
    s_files_storage_bar = create_usage_bar(vp, 0);
    s_files_info_lbl = tritium_theme::createLabel(vp, "SD: --", true);
    lv_obj_set_style_text_color(s_files_info_lbl, T_GHOST, 0);

    files_navigate("/");
}

static void storage_browse_lfs_cb(lv_event_t* e) {
    (void)e;
    s_files_use_sd = false;
    s_storage_browsing = true;
    strcpy(s_files_cwd, "/");

    lv_obj_t* vp = s_files_viewport;
    lv_obj_clean(vp);

    lv_obj_t* top = create_info_row(vp);

    lv_obj_t* back = tritium_theme::createButton(top, LV_SYMBOL_LEFT " Storage");
    lv_obj_add_event_cb(back, [](lv_event_t* ev) {
        (void)ev;
        s_storage_browsing = false;
        storage_show_overview();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* up_btn = tritium_theme::createButton(top, LV_SYMBOL_UP);
    lv_obj_add_event_cb(up_btn, files_back_cb, LV_EVENT_CLICKED, nullptr);

    s_files_path_lbl = tritium_theme::createLabel(top, "/", true);
    lv_obj_set_flex_grow(s_files_path_lbl, 1);
    lv_obj_set_style_text_color(s_files_path_lbl, T_CYAN, 0);

    lv_obj_t* mkdir_btn = tritium_theme::createButton(top, LV_SYMBOL_PLUS);
    lv_obj_add_event_cb(mkdir_btn, files_mkdir_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* list_panel = tritium_theme::createPanel(vp, "INTERNAL FLASH");
    lv_obj_set_width(list_panel, lv_pct(100));
    lv_obj_set_flex_grow(list_panel, 1);
    lv_obj_set_flex_flow(list_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(list_panel, 24, 0);
    lv_obj_set_style_pad_gap(list_panel, 2, 0);

    s_files_list = lv_obj_create(list_panel);
    lv_obj_set_size(s_files_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_files_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_files_list, 0, 0);
    lv_obj_set_style_pad_all(s_files_list, 0, 0);
    lv_obj_set_flex_flow(s_files_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_files_list, 1, 0);

    s_files_storage_bar = create_usage_bar(vp, 0);
    s_files_info_lbl = tritium_theme::createLabel(vp, "LFS: --", true);
    lv_obj_set_style_text_color(s_files_info_lbl, T_GHOST, 0);

    files_navigate("/");
}

// ---- Storage overview: shows all memory regions ----
static void storage_show_overview() {
    lv_obj_t* vp = s_files_viewport;
    if (!vp) return;
    lv_obj_clean(vp);

    s_files_list = nullptr;
    s_files_path_lbl = nullptr;
    s_files_info_lbl = nullptr;
    s_files_storage_bar = nullptr;

#ifndef SIMULATOR
    // ---- MEMORY section ----
    lv_obj_t* mem_panel = tritium_theme::createPanel(vp, "MEMORY");
    lv_obj_set_width(mem_panel, lv_pct(100));
    lv_obj_set_height(mem_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mem_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(mem_panel, 24, 0);
    lv_obj_set_style_pad_gap(mem_panel, 3, 0);

    // Internal SRAM
    {
        size_t free_heap = esp_get_free_heap_size();
        size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t used_heap = total_heap - free_heap;
        char buf[80];
        char used_str[16], total_str[16];
        format_size(used_str, sizeof(used_str), used_heap);
        format_size(total_str, sizeof(total_str), total_heap);
        snprintf(buf, sizeof(buf), "Internal SRAM   %s / %s", used_str, total_str);
        lv_obj_t* lbl = tritium_theme::createLabel(mem_panel, buf, true);
        lv_obj_set_style_text_color(lbl, T_TEXT, 0);
        int pct = total_heap ? (int)(used_heap * 100 / total_heap) : 0;
        create_usage_bar(mem_panel, pct);
    }

    // PSRAM
    {
        size_t total_ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t used_ps = total_ps - free_ps;
        char buf[80];
        char used_str[16], total_str[16];
        format_size(used_str, sizeof(used_str), used_ps);
        format_size(total_str, sizeof(total_str), total_ps);
        snprintf(buf, sizeof(buf), "PSRAM           %s / %s", used_str, total_str);
        lv_obj_t* lbl = tritium_theme::createLabel(mem_panel, buf, true);
        lv_obj_set_style_text_color(lbl, T_TEXT, 0);
        int pct = total_ps ? (int)(used_ps * 100 / total_ps) : 0;
        create_usage_bar(mem_panel, pct);
    }

    // Flash chip
    {
        uint32_t flash_sz_u32 = 0;
        esp_flash_get_size(NULL, &flash_sz_u32);
        size_t flash_sz = flash_sz_u32;
        const esp_partition_t* running = esp_ota_get_running_partition();
        size_t sketch_sz = running ? running->size : 0;
        char buf[80];
        char used_str[16], total_str[16];
        format_size(used_str, sizeof(used_str), sketch_sz);
        format_size(total_str, sizeof(total_str), flash_sz);
        snprintf(buf, sizeof(buf), "Flash chip      %s / %s", used_str, total_str);
        lv_obj_t* lbl = tritium_theme::createLabel(mem_panel, buf, true);
        lv_obj_set_style_text_color(lbl, T_TEXT, 0);
        int pct = flash_sz ? (int)(sketch_sz * 100 / flash_sz) : 0;
        create_usage_bar(mem_panel, pct);
    }

    // NVS (settings store)
    {
        // NVS stats via esp_partition
        const esp_partition_t* nvs_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        if (nvs_part) {
            char buf[80];
            char sz_str[16];
            format_size(sz_str, sizeof(sz_str), nvs_part->size);
            snprintf(buf, sizeof(buf), "NVS partition   %s", sz_str);
            lv_obj_t* lbl = tritium_theme::createLabel(mem_panel, buf, true);
            lv_obj_set_style_text_color(lbl, T_GHOST, 0);
        }
    }

    // ---- VOLUMES section ----
    lv_obj_t* vol_panel = tritium_theme::createPanel(vp, "VOLUMES");
    lv_obj_set_width(vol_panel, lv_pct(100));
    lv_obj_set_flex_grow(vol_panel, 1);
    lv_obj_set_flex_flow(vol_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(vol_panel, 24, 0);
    lv_obj_set_style_pad_gap(vol_panel, 4, 0);

    // ---- SD Card volume ----
    {
        auto* sd = sd_hal();
        if (!sd) goto skip_sd;

        // Try to mount if not already
        if (!sd->isMounted()) {
            sd->init();
        }

        lv_obj_t* sd_row = lv_obj_create(vol_panel);
        lv_obj_set_size(sd_row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(sd_row, T_SURFACE2, 0);
        lv_obj_set_style_bg_opa(sd_row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(sd_row, sd->isMounted() ? T_CYAN : T_SURFACE3, 0);
        lv_obj_set_style_border_width(sd_row, 1, 0);
        lv_obj_set_style_radius(sd_row, 4, 0);
        lv_obj_set_style_pad_all(sd_row, 6, 0);
        lv_obj_set_style_pad_gap(sd_row, 3, 0);
        lv_obj_set_flex_flow(sd_row, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(sd_row, LV_OBJ_FLAG_SCROLLABLE);

        // Title row: icon + name + status
        lv_obj_t* title_row = create_info_row(sd_row);
        lv_obj_t* title = tritium_theme::createLabel(title_row, LV_SYMBOL_SD_CARD " SD Card");
        lv_obj_set_style_text_color(title, T_CYAN, 0);

        if (sd->isMounted()) {
            char info[64];
            char used_str[16], total_str[16];
            format_size(used_str, sizeof(used_str), (size_t)sd->usedBytes());
            format_size(total_str, sizeof(total_str), (size_t)sd->totalBytes());
            snprintf(info, sizeof(info), "%s  %s / %s",
                     sd->getFilesystemType(), used_str, total_str);
            lv_obj_t* info_lbl = tritium_theme::createLabel(title_row, info, true);
            lv_obj_set_style_text_color(info_lbl, T_TEXT, 0);

            uint64_t total_b = sd->totalBytes();
            int pct = total_b ? (int)(sd->usedBytes() * 100 / total_b) : 0;
            create_usage_bar(sd_row, pct);

            // Action buttons row 1: Browse, Eject
            lv_obj_t* btn_row = create_info_row(sd_row);
            lv_obj_t* browse_btn = tritium_theme::createButton(btn_row,
                LV_SYMBOL_DIRECTORY " Browse", T_CYAN);
            lv_obj_add_event_cb(browse_btn, storage_browse_sd_cb,
                                LV_EVENT_CLICKED, nullptr);

            lv_obj_t* test_btn = tritium_theme::createButton(btn_row,
                LV_SYMBOL_CHARGE " Test", T_GREEN);
            lv_obj_add_event_cb(test_btn, [](lv_event_t* ev) {
                (void)ev;
                storage_show_sd_test();
            }, LV_EVENT_CLICKED, nullptr);

            lv_obj_t* eject_btn = tritium_theme::createButton(btn_row,
                LV_SYMBOL_EJECT " Eject", T_YELLOW);
            lv_obj_add_event_cb(eject_btn, [](lv_event_t* ev) {
                (void)ev;
                auto* s = sd_hal();
                if (s) s->deinit();
                storage_show_overview();
            }, LV_EVENT_CLICKED, nullptr);

            // Action buttons row 2: Format (with confirmation)
            lv_obj_t* btn_row2 = create_info_row(sd_row);
            lv_obj_t* fmt_btn = tritium_theme::createButton(btn_row2,
                LV_SYMBOL_TRASH " Format SD", T_MAGENTA);
            lv_obj_add_event_cb(fmt_btn, [](lv_event_t* ev) {
                (void)ev;
                storage_confirm_format(true);
            }, LV_EVENT_CLICKED, nullptr);
        } else {
            lv_obj_t* status = tritium_theme::createLabel(title_row,
                "Not detected", true);
            lv_obj_set_style_text_color(status, T_MAGENTA, 0);

            lv_obj_t* btn_row = create_info_row(sd_row);
            lv_obj_t* mount_btn = tritium_theme::createButton(btn_row,
                LV_SYMBOL_REFRESH " Mount", T_CYAN);
            lv_obj_add_event_cb(mount_btn, [](lv_event_t* ev) {
                (void)ev;
                auto* s = sd_hal();
                if (s) s->init();
                storage_show_overview();
            }, LV_EVENT_CLICKED, nullptr);

            // Try format if mount fails (card may have corrupted filesystem)
            lv_obj_t* fmt_btn = tritium_theme::createButton(btn_row,
                LV_SYMBOL_TRASH " Format", T_MAGENTA);
            lv_obj_add_event_cb(fmt_btn, [](lv_event_t* ev) {
                (void)ev;
                storage_confirm_format(true);
            }, LV_EVENT_CLICKED, nullptr);
        }
    }
    skip_sd:

    // ---- LittleFS volume ----
    {
        bool fs_ready = false;
#if STORAGE_SVC_AVAILABLE
        auto* fsh = fs_hal();
        if (fsh) fs_ready = fsh->isReady();
#elif FILES_FS_AVAILABLE
        FsHAL fsh_local;
        auto* fsh = &fsh_local;
        fs_ready = fsh->isReady();
#else
        void* fsh = nullptr;
#endif

        lv_obj_t* lfs_row = lv_obj_create(vol_panel);
        lv_obj_set_size(lfs_row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(lfs_row, T_SURFACE2, 0);
        lv_obj_set_style_bg_opa(lfs_row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(lfs_row, fs_ready ? T_CYAN : T_SURFACE3, 0);
        lv_obj_set_style_border_width(lfs_row, 1, 0);
        lv_obj_set_style_radius(lfs_row, 4, 0);
        lv_obj_set_style_pad_all(lfs_row, 6, 0);
        lv_obj_set_style_pad_gap(lfs_row, 3, 0);
        lv_obj_set_flex_flow(lfs_row, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(lfs_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* title_row = create_info_row(lfs_row);
        lv_obj_t* title = tritium_theme::createLabel(title_row,
            LV_SYMBOL_DRIVE " Internal Flash");
        lv_obj_set_style_text_color(title, T_CYAN, 0);

#if STORAGE_SVC_AVAILABLE || FILES_FS_AVAILABLE
        if (fs_ready && fsh) {
            char info[64];
            char used_str[16], total_str[16];
            format_size(used_str, sizeof(used_str), (size_t)((FsHAL*)fsh)->usedBytes());
            format_size(total_str, sizeof(total_str), (size_t)((FsHAL*)fsh)->totalBytes());
            snprintf(info, sizeof(info), "LittleFS  %s / %s", used_str, total_str);
            lv_obj_t* info_lbl = tritium_theme::createLabel(title_row, info, true);
            lv_obj_set_style_text_color(info_lbl, T_TEXT, 0);

            uint64_t total_b = ((FsHAL*)fsh)->totalBytes();
            int pct = total_b ? (int)(((FsHAL*)fsh)->usedBytes() * 100 / total_b) : 0;
            create_usage_bar(lfs_row, pct);

            lv_obj_t* btn_row = create_info_row(lfs_row);
            lv_obj_t* browse_btn = tritium_theme::createButton(btn_row,
                LV_SYMBOL_DIRECTORY " Browse", T_CYAN);
            lv_obj_add_event_cb(browse_btn, storage_browse_lfs_cb,
                                LV_EVENT_CLICKED, nullptr);

            lv_obj_t* fmt_btn = tritium_theme::createButton(btn_row,
                LV_SYMBOL_TRASH " Format", T_MAGENTA);
            lv_obj_add_event_cb(fmt_btn, [](lv_event_t* ev) {
                (void)ev;
                storage_confirm_format(false);
            }, LV_EVENT_CLICKED, nullptr);
        } else {
            lv_obj_t* status = tritium_theme::createLabel(title_row,
                "Not mounted", true);
            lv_obj_set_style_text_color(status, T_GHOST, 0);
        }
#else
        lv_obj_t* status = tritium_theme::createLabel(title_row,
            "Not available", true);
        lv_obj_set_style_text_color(status, T_GHOST, 0);
#endif
    }

#else  // SIMULATOR
    tritium_theme::createLabel(vp, "Storage not available in simulator");
#endif
}

void files_app_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 6, 0);
    lv_obj_set_style_pad_gap(viewport, 4, 0);
    s_files_viewport = viewport;
    s_storage_browsing = false;

    storage_show_overview();
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
    lv_obj_set_style_text_font(s_power_icon_lbl, tritium_shell::uiHeadingFont(), 0);
    lv_obj_set_style_text_color(s_power_icon_lbl, T_GREEN, 0);

    // Large percentage
    s_power_pct_lbl = lv_label_create(batt_panel);
    lv_label_set_text(s_power_pct_lbl, "--%");
    lv_obj_set_style_text_font(s_power_pct_lbl, tritium_shell::uiHeadingFont(), 0);
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
//  BLE Scanner App
// ===========================================================================

static lv_obj_t* s_ble_device_list = nullptr;
static lv_obj_t* s_ble_count_lbl   = nullptr;
static lv_obj_t* s_ble_status_lbl  = nullptr;
static lv_timer_t* s_ble_timer     = nullptr;

#if BLE_APP_AVAILABLE

static void ble_refresh(lv_timer_t* t) {
    (void)t;
    if (!s_ble_device_list) return;

    BleDevice devs[BLE_SCANNER_MAX_DEVICES];
    int count = hal_ble_scanner::get_devices(devs, BLE_SCANNER_MAX_DEVICES);
    int known = 0;
    for (int i = 0; i < count; i++) if (devs[i].is_known) known++;

    char buf[64];
    snprintf(buf, sizeof(buf), "Devices: %d  Known: %d", count, known);
    lv_label_set_text(s_ble_count_lbl, buf);

    lv_label_set_text(s_ble_status_lbl,
        hal_ble_scanner::is_active() ? "Scanner: ACTIVE" : "Scanner: INACTIVE");
    lv_obj_set_style_text_color(s_ble_status_lbl,
        hal_ble_scanner::is_active() ? T_GREEN : T_MAGENTA, 0);

    lv_obj_clean(s_ble_device_list);

    // Sort by RSSI (strongest first)
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (devs[j].rssi > devs[i].rssi) {
                BleDevice tmp = devs[i];
                devs[i] = devs[j];
                devs[j] = tmp;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t* row = lv_obj_create(s_ble_device_list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, T_SURFACE2, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Top row: name/MAC + RSSI
        lv_obj_t* top = lv_obj_create(row);
        lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(top, 0, 0);
        lv_obj_set_style_pad_all(top, 0, 0);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

        // Device name or MAC
        char name_buf[48];
        if (devs[i].name[0]) {
            snprintf(name_buf, sizeof(name_buf), "%s%s",
                     devs[i].is_known ? LV_SYMBOL_OK " " : "",
                     devs[i].name);
        } else {
            snprintf(name_buf, sizeof(name_buf), "%s%02X:%02X:%02X:%02X:%02X:%02X",
                     devs[i].is_known ? LV_SYMBOL_OK " " : "",
                     devs[i].addr[0], devs[i].addr[1], devs[i].addr[2],
                     devs[i].addr[3], devs[i].addr[4], devs[i].addr[5]);
        }
        lv_obj_t* name_lbl = tritium_theme::createLabel(top, name_buf);
        lv_obj_set_flex_grow(name_lbl, 1);
        lv_obj_set_style_text_color(name_lbl, devs[i].is_known ? T_CYAN : T_TEXT, 0);

        char rssi_buf[16];
        snprintf(rssi_buf, sizeof(rssi_buf), "%ddBm", (int)devs[i].rssi);
        lv_obj_t* rssi_lbl = tritium_theme::createLabel(top, rssi_buf, true);
        int rpct = (devs[i].rssi + 100);
        if (rpct < 0) rpct = 0;
        if (rpct > 100) rpct = 100;
        lv_color_t rc = (rpct > 60) ? T_GREEN : (rpct > 30) ? T_YELLOW : T_MAGENTA;
        lv_obj_set_style_text_color(rssi_lbl, rc, 0);

        // RSSI bar
        lv_obj_t* bar = lv_bar_create(row);
        lv_obj_set_width(bar, lv_pct(100));
        lv_obj_set_height(bar, 4);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, rpct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, T_SURFACE3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, rc, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);

        // Bottom row: MAC (if name shown) + seen count + age
        char detail_buf[64];
        uint32_t age_s = (millis() - devs[i].last_seen) / 1000;
        if (devs[i].name[0]) {
            snprintf(detail_buf, sizeof(detail_buf),
                     "%02X:%02X:%02X:%02X:%02X:%02X  seen:%u  %us ago",
                     devs[i].addr[0], devs[i].addr[1], devs[i].addr[2],
                     devs[i].addr[3], devs[i].addr[4], devs[i].addr[5],
                     (unsigned)devs[i].seen_count, (unsigned)age_s);
        } else {
            snprintf(detail_buf, sizeof(detail_buf),
                     "Type: %s  seen:%u  %us ago",
                     devs[i].addr_type == 0 ? "public" : "random",
                     (unsigned)devs[i].seen_count, (unsigned)age_s);
        }
        lv_obj_t* det = tritium_theme::createLabel(row, detail_buf, true);
        lv_obj_set_style_text_font(det, tritium_shell::uiSmallFont(), 0);
        lv_obj_set_style_text_color(det, T_GHOST, 0);
    }

    if (count == 0) {
        tritium_theme::createLabel(s_ble_device_list, "No devices detected", true);
    }
}

#endif  // BLE_APP_AVAILABLE

void ble_app_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 8, 0);
    lv_obj_set_style_pad_gap(viewport, 6, 0);

#if !BLE_APP_AVAILABLE
    tritium_theme::createLabel(viewport, "BLE Scanner not available in this build");
    tritium_theme::createLabel(viewport, "Enable with -DENABLE_BLE_SCANNER", true);
    return;
#else
    // Header
    lv_obj_t* hdr = lv_obj_create(viewport);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    s_ble_status_lbl = tritium_theme::createLabel(hdr, "Scanner: --");
    s_ble_count_lbl  = tritium_theme::createLabel(hdr, "Devices: --", true);

    // Device list (scrollable)
    lv_obj_t* list_panel = tritium_theme::createPanel(viewport, "BLE DEVICES");
    lv_obj_set_width(list_panel, lv_pct(100));
    lv_obj_set_flex_grow(list_panel, 1);
    lv_obj_set_flex_flow(list_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(list_panel, 24, 0);
    lv_obj_set_style_pad_gap(list_panel, 4, 0);

    s_ble_device_list = lv_obj_create(list_panel);
    lv_obj_set_size(s_ble_device_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_ble_device_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ble_device_list, 0, 0);
    lv_obj_set_style_pad_all(s_ble_device_list, 0, 0);
    lv_obj_set_flex_flow(s_ble_device_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_ble_device_list, 4, 0);

    tritium_theme::createLabel(s_ble_device_list, "Scanning...", true);

    // Initial update + timer (refresh every 5s)
    ble_refresh(nullptr);
    s_ble_timer = lv_timer_create(ble_refresh, 5000, nullptr);
#endif
}

// ===========================================================================
//  Terminal App — on-device serial console
// ===========================================================================

static lv_obj_t*   s_term_output  = nullptr;   // Scrollable label for output
static lv_obj_t*   s_term_input   = nullptr;   // Text area for command input
static lv_timer_t* s_term_timer   = nullptr;
static lv_obj_t*   s_term_kb      = nullptr;   // On-screen keyboard

// Ring buffer of displayed lines (to avoid re-fetching everything)
static constexpr int TERM_MAX_LINES = 100;
static char s_term_buf[4096] = {};  // Accumulated output text

#if TERMINAL_AVAILABLE
static void term_append_line(const char* line) {
    size_t cur = strlen(s_term_buf);
    size_t ll = strlen(line);
    if (cur + ll + 2 >= sizeof(s_term_buf)) {
        // Shift out first half
        const char* mid = s_term_buf + sizeof(s_term_buf) / 2;
        const char* nl = strchr(mid, '\n');
        if (nl) { nl++; size_t keep = cur - (nl - s_term_buf); memmove(s_term_buf, nl, keep); cur = keep; }
        else { cur = 0; }
        s_term_buf[cur] = '\0';
    }
    memcpy(s_term_buf + cur, line, ll);
    cur += ll;
    s_term_buf[cur++] = '\n';
    s_term_buf[cur] = '\0';
}

static void term_send_cmd(const char* cmd) {
    if (!cmd || !cmd[0]) return;

    // Echo the command
    char echo[300];
    snprintf(echo, sizeof(echo), "> %s", cmd);
    term_append_line(echo);

    // Parse verb + args
    char buf[256];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* space = strchr(buf, ' ');
    const char* verb = buf;
    const char* args = nullptr;
    if (space) { *space = '\0'; args = space + 1; }

    if (strcmp(verb, "IDENTIFY") == 0) {
        char r[128];
        snprintf(r, sizeof(r), "{\"board\":\"esp32-s3\",\"display\":\"%dx%d\"}",
                 display_get_width(), display_get_height());
        term_append_line(r);
    } else if (strcmp(verb, "SERVICES") == 0) {
#if __has_include("service_registry.h")
        char line[128];
        snprintf(line, sizeof(line), "[svc] %d services:", ServiceRegistry::count());
        term_append_line(line);
        for (int i = 0; i < ServiceRegistry::count(); i++) {
            auto* s = ServiceRegistry::at(i);
            if (s) {
                snprintf(line, sizeof(line), "  %-16s pri=%3d cap=%02X",
                         s->name(), s->initPriority(), s->capabilities());
                term_append_line(line);
            }
        }
#endif
    } else if (strcmp(verb, "HELP") == 0) {
        term_append_line("Commands: IDENTIFY, SERVICES, HELP, STATUS, HEAP");
    } else if (strcmp(verb, "STATUS") == 0) {
        char r[256];
#ifndef SIMULATOR
        uint32_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        snprintf(r, sizeof(r), "Heap: %luKB free, %luKB PSRAM, Uptime: %lus",
                 (unsigned long)(heap_free / 1024),
                 (unsigned long)(psram_free / 1024),
                 (unsigned long)(millis() / 1000));
#else
        snprintf(r, sizeof(r), "Simulator mode");
#endif
        term_append_line(r);
    } else if (strcmp(verb, "HEAP") == 0) {
#ifndef SIMULATOR
        char r[256];
        uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        int frag = (free_heap > 0) ? 100 - (int)(largest * 100 / free_heap) : 0;
        snprintf(r, sizeof(r), "Free: %luKB  Largest: %luKB  Frag: %d%%  PSRAM: %luKB",
                 (unsigned long)(free_heap / 1024), (unsigned long)(largest / 1024), frag,
                 (unsigned long)(psram_free / 1024));
        term_append_line(r);
#endif
    } else {
#if __has_include("service_registry.h")
        if (!ServiceRegistry::dispatchCommand(verb, args)) {
            char err[128];
            snprintf(err, sizeof(err), "[cmd] Unknown: %s", verb);
            term_append_line(err);
        }
#else
        char err[128];
        snprintf(err, sizeof(err), "[cmd] Unknown: %s", verb);
        term_append_line(err);
#endif
    }

    // Update display immediately
    if (s_term_output) {
        lv_label_set_text(s_term_output, s_term_buf);
        lv_obj_t* parent = lv_obj_get_parent(s_term_output);
        if (parent) lv_obj_scroll_to_y(parent, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void term_send_cb(lv_event_t* e) {
    (void)e;
    if (!s_term_input) return;
    const char* txt = lv_textarea_get_text(s_term_input);
    if (txt && txt[0]) {
        term_send_cmd(txt);
        lv_textarea_set_text(s_term_input, "");
    }
}

static void term_quick_cmd_cb(lv_event_t* e) {
    const char* cmd = (const char*)lv_event_get_user_data(e);
    if (cmd) term_send_cmd(cmd);
}

static void term_clear_cb(lv_event_t* e) {
    (void)e;
    s_term_buf[0] = '\0';
    if (s_term_output) lv_label_set_text(s_term_output, "");
}

static void term_input_focus_cb(lv_event_t* e) {
    (void)e;
    if (s_term_kb) lv_keyboard_set_textarea(s_term_kb, s_term_input);
}

static void term_refresh(lv_timer_t* t) {
    (void)t;
    if (!s_term_output) return;

    // Fetch latest lines from serial capture
    char fetch_buf[2048];
    int n = serial_capture::getLines(fetch_buf, sizeof(fetch_buf), 20);
    if (n <= 0) return;

    // Append new lines to our buffer
    size_t cur_len = strlen(s_term_buf);
    const char* p = fetch_buf;
    for (int i = 0; i < n; i++) {
        size_t line_len = strlen(p);
        if (cur_len + line_len + 2 >= sizeof(s_term_buf)) {
            // Buffer full — shift out first half
            const char* mid = s_term_buf + sizeof(s_term_buf) / 2;
            const char* nl = strchr(mid, '\n');
            if (nl) {
                nl++;
                size_t keep = cur_len - (nl - s_term_buf);
                memmove(s_term_buf, nl, keep);
                cur_len = keep;
                s_term_buf[cur_len] = '\0';
            } else {
                cur_len = 0;
                s_term_buf[0] = '\0';
            }
        }
        // Strip ANSI escape codes for clean display
        for (size_t j = 0; j < line_len && cur_len < sizeof(s_term_buf) - 2; j++) {
            if (p[j] == '\033') {
                // Skip ESC [ ... m sequences
                if (j + 1 < line_len && p[j + 1] == '[') {
                    j += 2;
                    while (j < line_len && p[j] != 'm') j++;
                    continue;
                }
            }
            s_term_buf[cur_len++] = p[j];
        }
        s_term_buf[cur_len++] = '\n';
        s_term_buf[cur_len] = '\0';
        p += line_len + 1;
    }

    lv_label_set_text(s_term_output, s_term_buf);

    // Auto-scroll to bottom
    lv_obj_t* parent = lv_obj_get_parent(s_term_output);
    if (parent) lv_obj_scroll_to_y(parent, LV_COORD_MAX, LV_ANIM_OFF);
}
#endif

void terminal_app_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(viewport, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(viewport, 4, 0);
    lv_obj_set_style_pad_gap(viewport, 4, 0);

#if !TERMINAL_AVAILABLE
    tritium_theme::createLabel(viewport, "Terminal not available");
    return;
#else
    // Quick command bar
    lv_obj_t* cmd_bar = lv_obj_create(viewport);
    lv_obj_set_size(cmd_bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cmd_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cmd_bar, 0, 0);
    lv_obj_set_style_pad_all(cmd_bar, 0, 0);
    lv_obj_set_flex_flow(cmd_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(cmd_bar, 4, 0);
    lv_obj_set_flex_align(cmd_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Quick command buttons
    static const char* quick_cmds[] = {
        "IDENTIFY", "SERVICES", "HELP", nullptr
    };
    for (int i = 0; quick_cmds[i]; i++) {
        lv_obj_t* btn = tritium_theme::createButton(cmd_bar, quick_cmds[i], T_CYAN);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_12, 0);
        lv_obj_add_event_cb(btn, term_quick_cmd_cb, LV_EVENT_CLICKED, (void*)quick_cmds[i]);
    }
    // Clear button
    lv_obj_t* clr_btn = tritium_theme::createButton(cmd_bar, LV_SYMBOL_TRASH, T_MAGENTA);
    lv_obj_set_style_pad_all(clr_btn, 4, 0);
    lv_obj_add_event_cb(clr_btn, term_clear_cb, LV_EVENT_CLICKED, nullptr);

    // Output area (scrollable)
    lv_obj_t* output_panel = lv_obj_create(viewport);
    lv_obj_set_size(output_panel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(output_panel, 1);
    lv_obj_set_style_bg_color(output_panel, T_VOID, 0);
    lv_obj_set_style_bg_opa(output_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(output_panel, T_CYAN, 0);
    lv_obj_set_style_border_opa(output_panel, LV_OPA_20, 0);
    lv_obj_set_style_border_width(output_panel, 1, 0);
    lv_obj_set_style_radius(output_panel, 4, 0);
    lv_obj_set_style_pad_all(output_panel, 4, 0);
    lv_obj_set_scrollbar_mode(output_panel, LV_SCROLLBAR_MODE_AUTO);

    s_term_output = lv_label_create(output_panel);
    lv_label_set_text(s_term_output, "Tritium Terminal v1.0\n> Type commands below\n");
    lv_obj_set_width(s_term_output, lv_pct(100));
    lv_label_set_long_mode(s_term_output, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_term_output, T_GREEN, 0);
    lv_obj_set_style_text_font(s_term_output, &lv_font_montserrat_12, 0);

    // Init display buffer with welcome text
    snprintf(s_term_buf, sizeof(s_term_buf), "Tritium Terminal v1.0\n> Type commands below\n");

    // Input row: text area + send button
    lv_obj_t* input_row = lv_obj_create(viewport);
    lv_obj_set_size(input_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(input_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_row, 0, 0);
    lv_obj_set_style_pad_all(input_row, 0, 0);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(input_row, 4, 0);

    s_term_input = lv_textarea_create(input_row);
    lv_textarea_set_one_line(s_term_input, true);
    lv_textarea_set_placeholder_text(s_term_input, "Command...");
    lv_obj_set_flex_grow(s_term_input, 1);
    lv_obj_set_style_bg_color(s_term_input, T_SURFACE3, 0);
    lv_obj_set_style_text_color(s_term_input, T_TEXT, 0);
    lv_obj_set_style_border_color(s_term_input, T_CYAN, 0);
    lv_obj_set_style_border_opa(s_term_input, LV_OPA_20, 0);
    lv_obj_set_style_text_font(s_term_input, &lv_font_montserrat_12, 0);
    lv_obj_add_event_cb(s_term_input, term_input_focus_cb, LV_EVENT_FOCUSED, nullptr);

    lv_obj_t* send_btn = tritium_theme::createButton(input_row, LV_SYMBOL_OK, T_GREEN);
    lv_obj_set_style_pad_all(send_btn, 6, 0);
    lv_obj_add_event_cb(send_btn, term_send_cb, LV_EVENT_CLICKED, nullptr);

    // On-screen keyboard
    s_term_kb = lv_keyboard_create(viewport);
    lv_obj_set_size(s_term_kb, lv_pct(100), 160);
    lv_keyboard_set_textarea(s_term_kb, s_term_input);
    lv_obj_set_style_bg_color(s_term_kb, T_SURFACE2, 0);
    lv_obj_set_style_text_color(s_term_kb, T_TEXT, 0);

    // Refresh timer — poll for new serial output every 500ms
    term_refresh(nullptr);
    s_term_timer = lv_timer_create(term_refresh, 500, nullptr);
#endif
}

// ===========================================================================
//  Cleanup — delete active LVGL timers before switching apps
// ===========================================================================

void cleanup_timers() {
    if (s_sysmon_timer) { lv_timer_delete(s_sysmon_timer); s_sysmon_timer = nullptr; }
    if (s_wifi_timer)   { lv_timer_delete(s_wifi_timer);   s_wifi_timer   = nullptr; }
    if (s_mesh_timer)   { lv_timer_delete(s_mesh_timer);   s_mesh_timer   = nullptr; }
    if (s_power_timer)  { lv_timer_delete(s_power_timer);  s_power_timer  = nullptr; }
    if (s_ble_timer)    { lv_timer_delete(s_ble_timer);    s_ble_timer    = nullptr; }
    if (s_term_timer)   { lv_timer_delete(s_term_timer);   s_term_timer   = nullptr; }
    if (s_clock_timer)  { lv_timer_delete(s_clock_timer);  s_clock_timer  = nullptr; }
    if (s_dev_timer)    { lv_timer_delete(s_dev_timer);    s_dev_timer    = nullptr; }
}

//  Register all new apps
// ===========================================================================

void register_all_apps() {
    // WiFi, Power, Brightness, About are now sub-panels inside Settings.
    tritium_shell::registerApp({"Monitor", "System health",    LV_SYMBOL_EYE_OPEN,     true, sysmon_app_create});
    tritium_shell::registerApp({"Mesh",    "P2P network",      LV_SYMBOL_SHUFFLE,      true, mesh_app_create});
    tritium_shell::registerApp({"Storage", "Storage manager",  LV_SYMBOL_DRIVE,         true, files_app_create});
    tritium_shell::registerApp({"BLE",     "BLE scanner",      LV_SYMBOL_BLUETOOTH,    true, ble_app_create});
    tritium_shell::registerApp({"Terminal","Serial console",   LV_SYMBOL_KEYBOARD,     true, terminal_app_create});
}

}  // namespace shell_apps
