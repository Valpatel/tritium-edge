/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "shell_apps.h"
#include "shell_theme.h"
#include "lock_screen.h"
#include "display.h"
#include "tritium_splash.h"  // TRITIUM_VERSION
#include "os_settings.h"     // TritiumSettings, SettingsDomain
#include <cstdio>
#include <cctype>

#ifndef SIMULATOR
#include "tritium_compat.h"
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>
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

#if __has_include("hal_sighting_logger.h")
#include "hal_sighting_logger.h"
#define SIGHTING_LOGGER_AVAILABLE 1
#else
#define SIGHTING_LOGGER_AVAILABLE 0
#endif

#if defined(ENABLE_MQTT) && __has_include("mqtt_service.h")
#include "mqtt_service.h"
#define MQTT_BRIDGE_AVAILABLE 1
#else
#define MQTT_BRIDGE_AVAILABLE 0
#endif

#if __has_include("mbtiles_reader.h")
#include "mbtiles_reader.h"
#define MAP_APP_AVAILABLE 1
#else
#define MAP_APP_AVAILABLE 0
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
static lv_timer_t* s_chat_timer      = nullptr;  // Chat app — defined here for cleanup_timers()
static lv_timer_t* s_term_timer      = nullptr;  // Terminal app — defined here for cleanup_timers()
static lv_timer_t* s_map_timer       = nullptr;  // Map app — defined here for cleanup_timers()

#if WIFI_APP_AVAILABLE

static lv_obj_t* s_wifi_detail_lbl = nullptr;

static void wifi_refresh_status() {
    auto* wm = WifiManager::_instance;
    if (!wm) return;
    WifiStatus st = wm->getStatus();
    char buf[96];

    if (st.connected) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s  (%ddBm)", st.ssid, (int)st.rssi);
        lv_label_set_text(s_wifi_status_ssid, buf);
        snprintf(buf, sizeof(buf), "IP: %s  CH: %d", st.ip, st.channel);
        lv_label_set_text(s_wifi_status_ip, buf);
        int pct = (st.rssi + 100);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_wifi_rssi_bar, pct, LV_ANIM_ON);
        lv_color_t c = (st.rssi > -50) ? T_GREEN : (st.rssi > -70) ? T_YELLOW : T_MAGENTA;
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

            pos += snprintf(detail + pos, sizeof(detail) - pos,
                "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                st.mac[0], st.mac[1], st.mac[2],
                st.mac[3], st.mac[4], st.mac[5]);
            if (st.connected_since > 0) {
                uint32_t conn_s = (millis() - st.connected_since) / 1000;
                uint32_t conn_m = conn_s / 60;
                uint32_t conn_h = conn_m / 60;
                pos += snprintf(detail + pos, sizeof(detail) - pos,
                    "\nConnected: %uh %um %us",
                    (unsigned)conn_h, (unsigned)(conn_m % 60), (unsigned)(conn_s % 60));
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
// Settings Hub — tabbed interface with 10 categories
// ---------------------------------------------------------------------------

static lv_obj_t* s_settings_content = nullptr;
static lv_obj_t* s_settings_tab_btns[11] = {};
static int s_settings_active_tab = 0;

// Forward declarations for tab content builders
static void settings_build_display(lv_obj_t* cont);
static void settings_build_wifi(lv_obj_t* cont);
static void settings_build_ble(lv_obj_t* cont);
static void settings_build_mesh(lv_obj_t* cont);
static void settings_build_monitor(lv_obj_t* cont);
static void settings_build_storage(lv_obj_t* cont);
static void settings_build_tracking(lv_obj_t* cont);
static void settings_build_power(lv_obj_t* cont);
static void settings_build_screensaver(lv_obj_t* cont);
static void settings_build_security(lv_obj_t* cont);
static void settings_build_system(lv_obj_t* cont);

static void settings_select_tab(int idx) {
    if (!s_settings_content) return;
    s_settings_active_tab = idx;

    // Update tab button styling
    for (int i = 0; i < 11; i++) {
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
        case 2: settings_build_ble(s_settings_content); break;
        case 3: settings_build_mesh(s_settings_content); break;
        case 4: settings_build_monitor(s_settings_content); break;
        case 5: settings_build_storage(s_settings_content); break;
        case 6: settings_build_tracking(s_settings_content); break;
        case 7: settings_build_power(s_settings_content); break;
        case 8: settings_build_screensaver(s_settings_content); break;
        case 9: settings_build_security(s_settings_content); break;
        case 10: settings_build_system(s_settings_content); break;
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

    // --- Tab bar: 10 icon buttons (scrollable) ---
    lv_obj_t* tab_bar = lv_obj_create(viewport);
    lv_obj_set_width(tab_bar, lv_pct(100));
    lv_obj_set_height(tab_bar, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 2, 0);
    lv_obj_set_style_pad_gap(tab_bar, 4, 0);
    lv_obj_set_flex_flow(tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_snap_x(tab_bar, LV_SCROLL_SNAP_CENTER);

    static const char* tab_icons[] = {
        LV_SYMBOL_EYE_OPEN,       // Display
        LV_SYMBOL_WIFI,            // WiFi
        LV_SYMBOL_BLUETOOTH,       // BLE
        LV_SYMBOL_SHUFFLE,         // Mesh
        LV_SYMBOL_CHARGE,          // Monitor
        LV_SYMBOL_DRIVE,           // Storage
        LV_SYMBOL_GPS,             // Tracking
        LV_SYMBOL_BATTERY_FULL,    // Power
        LV_SYMBOL_IMAGE,           // Screensaver
        LV_SYMBOL_WARNING,         // Security
        LV_SYMBOL_SETTINGS,        // System
    };
    for (int i = 0; i < 11; i++) {
        lv_obj_t* btn = lv_btn_create(tab_bar);
        int tab_h = (tritium_shell::uiConfig().screen_height > 400) ? 48 : 36;
        lv_obj_set_width(btn, tab_h + 8);
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
// Settings Tab: SECURITY (lock screen PIN management)
// ---------------------------------------------------------------------------

static lv_obj_t* s_pin_ta = nullptr;
static lv_obj_t* s_pin_status = nullptr;

static void pin_save_cb(lv_event_t* e) {
    (void)e;
    if (!s_pin_ta || !s_pin_status) return;
    const char* pin = lv_textarea_get_text(s_pin_ta);
    if (!pin || pin[0] == '\0') {
        // Clear PIN — disable lock screen
        lock_screen::set_pin("");
        lv_label_set_text(s_pin_status, "Lock screen disabled");
        lv_obj_set_style_text_color(s_pin_status, T_CYAN, 0);
    } else if (strlen(pin) < 4) {
        lv_label_set_text(s_pin_status, "PIN must be 4-8 digits");
        lv_obj_set_style_text_color(s_pin_status, T_MAGENTA, 0);
        return;
    } else {
        lock_screen::set_pin(pin);
        lv_label_set_text(s_pin_status, "PIN saved");
        lv_obj_set_style_text_color(s_pin_status, T_GREEN, 0);
    }
    lv_textarea_set_text(s_pin_ta, "");
}

static void pin_clear_cb(lv_event_t* e) {
    (void)e;
    lock_screen::set_pin("");
    if (s_pin_status) {
        lv_label_set_text(s_pin_status, "Lock screen disabled");
        lv_obj_set_style_text_color(s_pin_status, T_CYAN, 0);
    }
    if (s_pin_ta) lv_textarea_set_text(s_pin_ta, "");
}

static void pin_test_cb(lv_event_t* e) {
    (void)e;
    if (lock_screen::is_enabled()) {
        lock_screen::show();
    } else if (s_pin_status) {
        lv_label_set_text(s_pin_status, "No PIN set — nothing to test");
        lv_obj_set_style_text_color(s_pin_status, T_GHOST, 0);
    }
}

static void settings_build_security(lv_obj_t* cont) {
    // Status
    lv_obj_t* panel = tritium_theme::createPanel(cont, "LOCK SCREEN");
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(panel, 24, 0);
    lv_obj_set_style_pad_gap(panel, 6, 0);

    bool enabled = lock_screen::is_enabled();
    char buf[48];
    snprintf(buf, sizeof(buf), "Status: %s", enabled ? "ENABLED" : "Disabled");
    lv_obj_t* status_lbl = tritium_theme::createLabel(panel, buf, true);
    lv_obj_set_style_text_color(status_lbl, enabled ? T_GREEN : T_GHOST, 0);

    // PIN entry
    lv_obj_t* pin_panel = tritium_theme::createPanel(cont, "SET / CHANGE PIN");
    lv_obj_set_width(pin_panel, lv_pct(100));
    lv_obj_set_height(pin_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pin_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(pin_panel, 24, 0);
    lv_obj_set_style_pad_gap(pin_panel, 6, 0);

    tritium_theme::createLabel(pin_panel, "Enter 4-8 digit PIN:", true);

    s_pin_ta = lv_textarea_create(pin_panel);
    lv_textarea_set_max_length(s_pin_ta, 8);
    lv_textarea_set_password_mode(s_pin_ta, true);
    lv_textarea_set_one_line(s_pin_ta, true);
    lv_textarea_set_accepted_chars(s_pin_ta, "0123456789");
    lv_textarea_set_placeholder_text(s_pin_ta, "PIN");
    lv_obj_set_width(s_pin_ta, lv_pct(80));
    lv_obj_set_height(s_pin_ta, 36);
    lv_obj_set_style_bg_color(s_pin_ta, T_SURFACE2, 0);
    lv_obj_set_style_text_color(s_pin_ta, T_CYAN, 0);
    lv_obj_set_style_border_color(s_pin_ta, T_CYAN, 0);
    lv_obj_set_style_border_width(s_pin_ta, 1, 0);

    // Buttons row
    lv_obj_t* btn_row = lv_obj_create(pin_panel);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);

    lv_obj_t* save_btn = tritium_theme::createButton(btn_row, "Save PIN");
    lv_obj_add_event_cb(save_btn, pin_save_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* clear_btn = tritium_theme::createButton(btn_row, "Clear PIN");
    lv_obj_add_event_cb(clear_btn, pin_clear_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* test_btn = tritium_theme::createButton(btn_row, "Test Lock");
    lv_obj_add_event_cb(test_btn, pin_test_cb, LV_EVENT_CLICKED, nullptr);

    // Status feedback
    s_pin_status = lv_label_create(pin_panel);
    lv_label_set_text(s_pin_status, "");
    lv_obj_set_style_text_font(s_pin_status, tritium_shell::uiSmallFont(), 0);
    lv_obj_set_style_text_color(s_pin_status, T_GHOST, 0);
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

    const display_health_t* dh = display_get_health();
    snprintf(buf, sizeof(buf), "Board: %s", dh ? dh->board_name : "Unknown");
    tritium_theme::createLabel(info, buf, true);

    snprintf(buf, sizeof(buf), "Display: %s %dx%d",
             dh ? dh->driver : "?",
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
static lv_obj_t* s_sysmon_heap_min   = nullptr;
static lv_obj_t* s_sysmon_psram_bar  = nullptr;
static lv_obj_t* s_sysmon_psram_lbl  = nullptr;
static lv_obj_t* s_sysmon_loop_lbl   = nullptr;
static lv_obj_t* s_sysmon_uptime_lbl = nullptr;
static lv_obj_t* s_sysmon_temp_lbl   = nullptr;
static lv_obj_t* s_sysmon_wifi_lbl   = nullptr;
static lv_obj_t* s_sysmon_tasks_lbl  = nullptr;
static lv_obj_t* s_sysmon_storage_lbl = nullptr;
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

    // Heap watermark
    if (s_sysmon_heap_min) {
        uint32_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snprintf(buf, sizeof(buf), "Min free: %uKB", (unsigned)(min_heap / 1024));
        lv_label_set_text(s_sysmon_heap_min, buf);
    }

    // Task count
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    snprintf(buf, sizeof(buf), "Tasks: %u", (unsigned)task_count);
    lv_label_set_text(s_sysmon_tasks_lbl, buf);

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

    static char text_buf[4096];
    size_t read_len = fread(text_buf, 1, sizeof(text_buf) - 1, f);
    text_buf[read_len] = '\0';
    fclose(f);

    // Text content in mono label
    lv_obj_t* content = lv_label_create(s_files_list);
    lv_label_set_text(content, text_buf);
    lv_label_set_long_mode(content, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_style_text_color(content, T_TEXT, 0);
    lv_obj_set_style_text_font(content, tritium_shell::uiSmallFont(), 0);

    if (fsize > sizeof(text_buf) - 1) {
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

static lv_obj_t* s_tracking_stats_lbl = nullptr;
static lv_timer_t* s_tracking_timer   = nullptr;

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

// ---------------------------------------------------------------------------
// Settings Tab: BLE (wrapper around BLE scanner content)
// ---------------------------------------------------------------------------

static void settings_build_ble(lv_obj_t* cont) {
#if !BLE_APP_AVAILABLE
    tritium_theme::createLabel(cont, "BLE Scanner not available in this build");
    return;
#else
    // Reuse the BLE app header
    lv_obj_t* hdr = lv_obj_create(cont);
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

    lv_obj_t* list_panel = tritium_theme::createPanel(cont, "BLE DEVICES");
    lv_obj_set_width(list_panel, lv_pct(100));
    lv_obj_set_height(list_panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(list_panel, 300, 0);
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
    ble_refresh(nullptr);
    s_ble_timer = lv_timer_create(ble_refresh, 5000, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// Settings Tab: MESH (wrapper around Mesh content)
// ---------------------------------------------------------------------------

static void settings_build_mesh(lv_obj_t* cont) {
#if !MESH_APP_AVAILABLE
    tritium_theme::createLabel(cont, "Mesh HAL not available");
    return;
#else
    lv_obj_t* status_panel = tritium_theme::createPanel(cont, "MESH STATUS");
    lv_obj_set_width(status_panel, lv_pct(100));
    lv_obj_set_height(status_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(status_panel, 24, 0);
    lv_obj_set_style_pad_gap(status_panel, 4, 0);

    s_mesh_role_lbl  = tritium_theme::createLabel(status_panel, "Role: --", true);
    s_mesh_peers_lbl = tritium_theme::createLabel(status_panel, "Peers: 0", true);

    lv_obj_t* peer_panel = tritium_theme::createPanel(cont, "PEERS");
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

    lv_obj_t* bcast_panel = tritium_theme::createPanel(cont, "BROADCAST");
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

    s_mesh_stats_lbl = tritium_theme::createLabel(cont, "TX:0 RX:0 Relay:0 Drop:0", true);
    lv_obj_set_style_text_color(s_mesh_stats_lbl, T_GHOST, 0);

    mesh_refresh(nullptr);
    s_mesh_timer = lv_timer_create(mesh_refresh, 3000, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// Settings Tab: MONITOR (wrapper around System Monitor content)
// ---------------------------------------------------------------------------

static void settings_build_monitor(lv_obj_t* cont) {
#ifdef SIMULATOR
    tritium_theme::createLabel(cont, "System Monitor not available in simulator");
    return;
#else
    // Memory panel
    lv_obj_t* mem_panel = tritium_theme::createPanel(cont, "MEMORY");
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

    // CPU panel
    lv_obj_t* cpu_panel = tritium_theme::createPanel(cont, "CPU");
    lv_obj_set_width(cpu_panel, lv_pct(100));
    lv_obj_set_height(cpu_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cpu_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(cpu_panel, 24, 0);
    lv_obj_set_style_pad_gap(cpu_panel, 4, 0);

    s_sysmon_loop_lbl   = tritium_theme::createLabel(cpu_panel, "Loop: --", true);
    s_sysmon_temp_lbl   = tritium_theme::createLabel(cpu_panel, "CPU Temp: --", true);
    s_sysmon_uptime_lbl = tritium_theme::createLabel(cpu_panel, "Uptime: --", true);
    s_sysmon_tasks_lbl  = tritium_theme::createLabel(cpu_panel, "Tasks: --", true);

    // Network panel
    lv_obj_t* net_panel = tritium_theme::createPanel(cont, "NETWORK");
    lv_obj_set_width(net_panel, lv_pct(100));
    lv_obj_set_height(net_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(net_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(net_panel, 24, 0);
    lv_obj_set_style_pad_gap(net_panel, 4, 0);

    s_sysmon_wifi_lbl = tritium_theme::createLabel(net_panel, "WiFi: --", true);

    // Storage panel
    lv_obj_t* stor_panel = tritium_theme::createPanel(cont, "STORAGE");
    lv_obj_set_width(stor_panel, lv_pct(100));
    lv_obj_set_height(stor_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stor_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(stor_panel, 24, 0);
    lv_obj_set_style_pad_gap(stor_panel, 4, 0);

    s_sysmon_storage_lbl = tritium_theme::createLabel(stor_panel, "Storage: --", true);

    sysmon_update(nullptr);
    s_sysmon_timer = lv_timer_create(sysmon_update, 2000, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// Settings Tab: STORAGE (simplified file browser)
// ---------------------------------------------------------------------------

static void settings_build_storage(lv_obj_t* cont) {
#ifdef SIMULATOR
    tritium_theme::createLabel(cont, "Storage not available in simulator");
    return;
#else
    lv_obj_t* panel = tritium_theme::createPanel(cont, "STORAGE");
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(panel, 24, 0);
    lv_obj_set_style_pad_gap(panel, 4, 0);

    // Show storage stats
    struct stat st;
    bool sd_ok = (stat("/sdcard", &st) == 0);
    bool fs_ok = (stat("/littlefs", &st) == 0);

    char buf[128];
    snprintf(buf, sizeof(buf), "SD Card: %s", sd_ok ? "mounted" : "not available");
    tritium_theme::createLabel(panel, buf, true);

    snprintf(buf, sizeof(buf), "LittleFS: %s", fs_ok ? "mounted" : "not available");
    tritium_theme::createLabel(panel, buf, true);

    // File browser panel - simplified directory listing of /sdcard
    lv_obj_t* files_panel = tritium_theme::createPanel(cont, "SD CARD FILES");
    lv_obj_set_width(files_panel, lv_pct(100));
    lv_obj_set_height(files_panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(files_panel, 300, 0);
    lv_obj_set_flex_flow(files_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(files_panel, 24, 0);
    lv_obj_set_style_pad_gap(files_panel, 2, 0);

    if (sd_ok) {
        DIR* dir = opendir("/sdcard");
        if (dir) {
            struct dirent* entry;
            int count = 0;
            while ((entry = readdir(dir)) != nullptr && count < 20) {
                if (entry->d_name[0] == '.') continue;
                bool is_dir = (entry->d_type == DT_DIR);
                snprintf(buf, sizeof(buf), "%s %s", is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, entry->d_name);
                lv_obj_t* lbl = tritium_theme::createLabel(files_panel, buf, true);
                lv_obj_set_style_text_color(lbl, is_dir ? T_CYAN : T_TEXT, 0);
                count++;
            }
            closedir(dir);
            if (count == 0) {
                tritium_theme::createLabel(files_panel, "Empty", true);
            }
        }
    } else {
        tritium_theme::createLabel(files_panel, "Insert SD card to browse files", true);
    }
#endif
}

// ---------------------------------------------------------------------------
// Settings Tab: TRACKING (sighting logger)
// ---------------------------------------------------------------------------

static void tracking_toggle_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
#if SIGHTING_LOGGER_AVAILABLE
    if (on) hal_sighting_logger::enable();
    else hal_sighting_logger::disable();
#endif
    (void)on;
}

static void tracking_refresh(lv_timer_t*) {
#if SIGHTING_LOGGER_AVAILABLE
    if (!s_tracking_stats_lbl) return;
    auto stats = hal_sighting_logger::get_stats();
    char buf[128];
    snprintf(buf, sizeof(buf), "BLE: %lu  WiFi: %lu  Total: %lu  DB: %lu KB",
             (unsigned long)stats.ble_logged,
             (unsigned long)stats.wifi_logged,
             (unsigned long)stats.total_rows,
             (unsigned long)(stats.db_size_bytes / 1024));
    lv_label_set_text(s_tracking_stats_lbl, buf);
#endif
}

static void settings_build_tracking(lv_obj_t* cont) {
#if !SIGHTING_LOGGER_AVAILABLE
    tritium_theme::createLabel(cont, "Sighting Logger not available");
    tritium_theme::createLabel(cont, "Enable with -DENABLE_SIGHTING_LOGGER", true);
    return;
#else
    // Enable/disable toggle
    lv_obj_t* panel = tritium_theme::createPanel(cont, "SIGHTING LOGGER");
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(panel, 24, 0);
    lv_obj_set_style_pad_gap(panel, 8, 0);

    tritium_theme::createLabel(panel, "Log BLE + WiFi sightings to SD card");
    lv_obj_set_style_text_color(lv_obj_get_child(panel, -1), T_GHOST, 0);

    // Toggle row
    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    tritium_theme::createLabel(row, "Enable Logging");

    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_set_style_bg_color(sw, T_SURFACE3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, T_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, T_CYAN, LV_PART_KNOB);
    if (hal_sighting_logger::is_active()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, tracking_toggle_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Stats panel
    lv_obj_t* stats_panel = tritium_theme::createPanel(cont, "STATISTICS");
    lv_obj_set_width(stats_panel, lv_pct(100));
    lv_obj_set_height(stats_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stats_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(stats_panel, 24, 0);
    lv_obj_set_style_pad_gap(stats_panel, 4, 0);

    s_tracking_stats_lbl = tritium_theme::createLabel(stats_panel, "BLE: 0  WiFi: 0  Total: 0", true);

    auto stats = hal_sighting_logger::get_stats();
    char db_info[64];
    snprintf(db_info, sizeof(db_info), "Database: %s", stats.db_open ? "open" : "closed");
    tritium_theme::createLabel(stats_panel, db_info, true);

    // Auto-refresh
    tracking_refresh(nullptr);
    s_tracking_timer = lv_timer_create(tracking_refresh, 3000, nullptr);
#endif
}

// ===========================================================================
//  Cleanup — delete active LVGL timers before switching apps
// ===========================================================================

void cleanup_timers() {
    if (s_sysmon_timer)    { lv_timer_delete(s_sysmon_timer);    s_sysmon_timer    = nullptr; }
    if (s_wifi_timer)      { lv_timer_delete(s_wifi_timer);      s_wifi_timer      = nullptr; }
    if (s_mesh_timer)      { lv_timer_delete(s_mesh_timer);      s_mesh_timer      = nullptr; }
    if (s_power_timer)     { lv_timer_delete(s_power_timer);     s_power_timer     = nullptr; }
    if (s_ble_timer)       { lv_timer_delete(s_ble_timer);       s_ble_timer       = nullptr; }
    if (s_tracking_timer)  { lv_timer_delete(s_tracking_timer);  s_tracking_timer  = nullptr; }
    if (s_chat_timer)      { lv_timer_delete(s_chat_timer);      s_chat_timer      = nullptr; }
    if (s_term_timer)      { lv_timer_delete(s_term_timer);      s_term_timer      = nullptr; }
    if (s_map_timer)       { lv_timer_delete(s_map_timer);       s_map_timer       = nullptr; }
}

// ===========================================================================
//  Mesh Chat App — P2P messaging over ESP-NOW
// ===========================================================================

static lv_obj_t* s_chat_log     = nullptr;
static lv_obj_t* s_chat_input   = nullptr;
static lv_obj_t* s_chat_peers   = nullptr;

// Chat message ring buffer (lazy-allocated in PSRAM)
static constexpr int CHAT_MAX_MSGS  = 32;
static constexpr int CHAT_MSG_LEN   = 200;

struct ChatMessage {
    char sender[18];   // MAC string (short: XX:XX:XX)
    char text[CHAT_MSG_LEN];
    bool is_self;
};

static ChatMessage* s_chat_msgs = nullptr;
static int s_chat_msg_head = 0;
static int s_chat_msg_count = 0;
static bool s_chat_dirty = false;

static void chat_ensure_buf() {
    if (s_chat_msgs) return;
    s_chat_msgs = (ChatMessage*)heap_caps_malloc(
        CHAT_MAX_MSGS * sizeof(ChatMessage), MALLOC_CAP_SPIRAM);
    if (!s_chat_msgs)
        s_chat_msgs = (ChatMessage*)malloc(CHAT_MAX_MSGS * sizeof(ChatMessage));
    if (s_chat_msgs)
        memset(s_chat_msgs, 0, CHAT_MAX_MSGS * sizeof(ChatMessage));
}

static void chat_add_msg(const char* sender, const char* text, bool is_self) {
    chat_ensure_buf();
    if (!s_chat_msgs) return;
    ChatMessage& m = s_chat_msgs[s_chat_msg_head];
    strncpy(m.sender, sender, sizeof(m.sender) - 1);
    m.sender[sizeof(m.sender) - 1] = '\0';
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = '\0';
    m.is_self = is_self;
    s_chat_msg_head = (s_chat_msg_head + 1) % CHAT_MAX_MSGS;
    if (s_chat_msg_count < CHAT_MAX_MSGS) s_chat_msg_count++;
    s_chat_dirty = true;
}

#if MESH_APP_AVAILABLE

// Callback registered with MeshManager to receive chat messages
static void chat_rx_callback(const MeshHeaderEx& hdr,
                              const uint8_t* payload, void* /*ud*/) {
    // Only handle broadcast and addressed messages
    if (hdr.type != MESH_EX_BROADCAST && hdr.type != MESH_EX_ADDRESSED) return;
    if (hdr.payload_len == 0 || hdr.payload_len > CHAT_MSG_LEN - 1) return;

    // Check if this looks like a chat message (starts with "CHAT:" prefix)
    if (hdr.payload_len < 6) return;
    if (memcmp(payload, "CHAT:", 5) != 0) return;

    // Extract the text after "CHAT:"
    char text[CHAT_MSG_LEN];
    int text_len = hdr.payload_len - 5;
    memcpy(text, payload + 5, text_len);
    text[text_len] = '\0';

    // Format sender MAC (short form)
    char sender[18];
    snprintf(sender, sizeof(sender), "%02X:%02X:%02X",
             hdr.src_mac[3], hdr.src_mac[4], hdr.src_mac[5]);

    // Look up device name from peer list
    const MeshPeerInfo* peer = MeshManager::instance().findPeer(hdr.src_mac);
    if (peer && peer->device_name[0]) {
        strncpy(sender, peer->device_name, sizeof(sender) - 1);
        sender[sizeof(sender) - 1] = '\0';
    }

    chat_add_msg(sender, text, false);

    // Bridge received chat to MQTT
#if MQTT_BRIDGE_AVAILABLE
    {
        auto* mqtt_svc = ServiceRegistry::getAs<MqttService>("mqtt");
        if (mqtt_svc) {
            char json[256];
            snprintf(json, sizeof(json), "{\"from\":\"%s\",\"text\":\"%s\"}", sender, text);
            mqtt_svc->publish("chat", json);
        }
    }
#endif

    // Show toast for new messages
    char toast_buf[80];
    snprintf(toast_buf, sizeof(toast_buf), "%s: %s",
             sender, text_len > 40 ? "..." : text);
    tritium_shell::toast(toast_buf, tritium_shell::NOTIFY_INFO, 3000);
}

static void chat_send_cb(lv_event_t* /*e*/) {
    auto& mm = MeshManager::instance();
    if (!mm.isReady() || !s_chat_input) return;
    const char* text = lv_textarea_get_text(s_chat_input);
    if (!text || text[0] == '\0') return;

    // Prefix with "CHAT:" to distinguish from other mesh messages
    char payload[CHAT_MSG_LEN + 6];
    int len = snprintf(payload, sizeof(payload), "CHAT:%s", text);

    if (mm.broadcast((const uint8_t*)payload, len)) {
        chat_add_msg("You", text, true);
#if MQTT_BRIDGE_AVAILABLE
        {
            auto* mqtt_svc = ServiceRegistry::getAs<MqttService>("mqtt");
            if (mqtt_svc) {
                char json[256];
                snprintf(json, sizeof(json), "{\"from\":\"self\",\"text\":\"%s\"}", text);
                mqtt_svc->publish("chat", json);
            }
        }
#endif
        lv_textarea_set_text(s_chat_input, "");
    } else {
        tritium_shell::toast("Send failed", tritium_shell::NOTIFY_ERROR, 2000);
    }
}

static void chat_refresh(lv_timer_t* /*t*/) {
    if (!s_chat_log || !s_chat_dirty || !s_chat_msgs) return;
    s_chat_dirty = false;

    // Rebuild chat display
    lv_obj_clean(s_chat_log);

    int start = (s_chat_msg_count < CHAT_MAX_MSGS) ? 0 : s_chat_msg_head;
    for (int i = 0; i < s_chat_msg_count; i++) {
        int idx = (start + i) % CHAT_MAX_MSGS;
        ChatMessage& m = s_chat_msgs[idx];

        lv_obj_t* row = lv_obj_create(s_chat_log);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Sender label
        lv_obj_t* sender_lbl = lv_label_create(row);
        lv_label_set_text(sender_lbl, m.sender);
        lv_obj_set_style_text_color(sender_lbl, m.is_self ? T_CYAN : T_GREEN, 0);
        lv_obj_set_style_text_font(sender_lbl, tritium_shell::uiSmallFont(), 0);

        // Message text
        lv_obj_t* text_lbl = lv_label_create(row);
        lv_label_set_text(text_lbl, m.text);
        lv_label_set_long_mode(text_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(text_lbl, lv_pct(100));
        lv_obj_set_style_text_color(text_lbl, T_TEXT, 0);
        lv_obj_set_style_text_font(text_lbl, tritium_shell::uiSmallFont(), 0);
    }

    // Auto-scroll to bottom
    lv_obj_scroll_to_y(s_chat_log, LV_COORD_MAX, LV_ANIM_OFF);

    // Update peer count
    if (s_chat_peers) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d peers online",
                 MeshManager::instance().peerCount());
        lv_label_set_text(s_chat_peers, buf);
    }
}

#endif  // MESH_APP_AVAILABLE

void mesh_chat_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(viewport, 4, 0);
    lv_obj_set_style_pad_gap(viewport, 4, 0);

#if !MESH_APP_AVAILABLE
    tritium_theme::createLabel(viewport, "Mesh HAL not available");
    return;
#else
    // Header row
    lv_obj_t* hdr = lv_obj_create(viewport);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, LV_SYMBOL_SHUFFLE " MESH CHAT");
    lv_obj_set_style_text_color(title, T_CYAN, 0);
    lv_obj_set_style_text_font(title, tritium_shell::uiFont(), 0);

    s_chat_peers = lv_label_create(hdr);
    lv_label_set_text(s_chat_peers, "0 peers");
    lv_obj_set_style_text_color(s_chat_peers, T_GHOST, 0);
    lv_obj_set_style_text_font(s_chat_peers, tritium_shell::uiSmallFont(), 0);

    // Message log area
    s_chat_log = lv_obj_create(viewport);
    lv_obj_set_width(s_chat_log, lv_pct(100));
    lv_obj_set_flex_grow(s_chat_log, 1);
    lv_obj_set_flex_flow(s_chat_log, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_chat_log, T_VOID, 0);
    lv_obj_set_style_bg_opa(s_chat_log, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_chat_log, T_CYAN, 0);
    lv_obj_set_style_border_opa(s_chat_log, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_chat_log, 1, 0);
    lv_obj_set_style_radius(s_chat_log, 4, 0);
    lv_obj_set_style_pad_all(s_chat_log, 4, 0);
    lv_obj_set_style_pad_gap(s_chat_log, 2, 0);
    lv_obj_set_scrollbar_mode(s_chat_log, LV_SCROLLBAR_MODE_AUTO);

    // Welcome message
    chat_add_msg("System", "Mesh Chat ready. Type a message to broadcast.", false);

    // Input row
    lv_obj_t* input_row = lv_obj_create(viewport);
    lv_obj_set_width(input_row, lv_pct(100));
    lv_obj_set_height(input_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(input_row, 0, 0);
    lv_obj_set_style_pad_gap(input_row, 4, 0);
    lv_obj_set_style_bg_opa(input_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_row, 0, 0);
    lv_obj_remove_flag(input_row, LV_OBJ_FLAG_SCROLLABLE);

    // Text input
    s_chat_input = lv_textarea_create(input_row);
    lv_obj_set_flex_grow(s_chat_input, 1);
    lv_obj_set_height(s_chat_input, 32);
    lv_textarea_set_one_line(s_chat_input, true);
    lv_textarea_set_placeholder_text(s_chat_input, "message...");
    lv_textarea_set_max_length(s_chat_input, CHAT_MSG_LEN - 10);
    lv_obj_set_style_bg_color(s_chat_input, T_SURFACE2, 0);
    lv_obj_set_style_bg_opa(s_chat_input, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_chat_input, T_TEXT, 0);
    lv_obj_set_style_text_font(s_chat_input, tritium_shell::uiSmallFont(), 0);
    lv_obj_set_style_border_color(s_chat_input, T_CYAN, 0);
    lv_obj_set_style_border_opa(s_chat_input, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_chat_input, 1, 0);
    lv_obj_set_style_radius(s_chat_input, 4, 0);
    lv_obj_add_event_cb(s_chat_input, chat_send_cb, LV_EVENT_READY, nullptr);

    // Send button
    lv_obj_t* send_btn = tritium_theme::createButton(input_row, LV_SYMBOL_RIGHT, T_GREEN);
    lv_obj_set_size(send_btn, 44, 32);
    lv_obj_add_event_cb(send_btn, [](lv_event_t* /*e*/) {
        lv_obj_send_event(s_chat_input, LV_EVENT_READY, nullptr);
    }, LV_EVENT_CLICKED, nullptr);

    // Register mesh message callback for incoming chat
    MeshManager::instance().onMessage(chat_rx_callback, nullptr);

    // Refresh timer (update peer count, render new messages)
    s_chat_timer = lv_timer_create(chat_refresh, 500, nullptr);

    // Initial refresh
    s_chat_dirty = true;
#endif  // MESH_APP_AVAILABLE
}

// ===========================================================================
//  Terminal App — on-device serial console
// ===========================================================================

static lv_obj_t* s_term_log     = nullptr;
static lv_obj_t* s_term_input   = nullptr;

// Circular log buffer for terminal output — lazy-allocated in PSRAM
static constexpr int TERM_LOG_LINES = 64;
static constexpr int TERM_LINE_LEN  = 120;
static char (*s_term_lines)[TERM_LINE_LEN] = nullptr;  // [TERM_LOG_LINES][TERM_LINE_LEN]
static int s_term_head = 0;    // next write position
static int s_term_count = 0;   // total lines stored
static bool s_term_dirty = false;

static void term_ensure_buf() {
    if (s_term_lines) return;
    s_term_lines = (char(*)[TERM_LINE_LEN])heap_caps_malloc(
        TERM_LOG_LINES * TERM_LINE_LEN, MALLOC_CAP_SPIRAM);
    if (!s_term_lines)
        s_term_lines = (char(*)[TERM_LINE_LEN])malloc(TERM_LOG_LINES * TERM_LINE_LEN);
    if (s_term_lines)
        memset(s_term_lines, 0, TERM_LOG_LINES * TERM_LINE_LEN);
}

// Public: append a line to the terminal log (called from Serial shim or services)
static void term_append(const char* line) {
    term_ensure_buf();
    if (!s_term_lines) return;
    strncpy(s_term_lines[s_term_head], line, TERM_LINE_LEN - 1);
    s_term_lines[s_term_head][TERM_LINE_LEN - 1] = '\0';
    s_term_head = (s_term_head + 1) % TERM_LOG_LINES;
    if (s_term_count < TERM_LOG_LINES) s_term_count++;
    s_term_dirty = true;
}

static void term_refresh(lv_timer_t* /*t*/) {
    if (!s_term_log || !s_term_dirty || !s_term_lines) return;
    s_term_dirty = false;

    // Build display text from circular buffer
    static char display_buf[TERM_LOG_LINES * TERM_LINE_LEN];
    int pos = 0;
    int start = (s_term_count < TERM_LOG_LINES) ? 0 :
                (s_term_head);  // oldest line
    for (int i = 0; i < s_term_count && pos < (int)sizeof(display_buf) - TERM_LINE_LEN; i++) {
        int idx = (start + i) % TERM_LOG_LINES;
        int len = strlen(s_term_lines[idx]);
        memcpy(display_buf + pos, s_term_lines[idx], len);
        pos += len;
        display_buf[pos++] = '\n';
    }
    if (pos > 0) pos--;  // remove trailing newline
    display_buf[pos] = '\0';

    lv_label_set_text(s_term_log, display_buf);
    // Auto-scroll to bottom
    lv_obj_scroll_to_y(lv_obj_get_parent(s_term_log), LV_COORD_MAX, LV_ANIM_OFF);
}

static void term_submit_cb(lv_event_t* e) {
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    const char* txt = lv_textarea_get_text(ta);
    if (!txt || txt[0] == '\0') return;

    // Echo command
    char echo[TERM_LINE_LEN];
    snprintf(echo, sizeof(echo), "> %s", txt);
    term_append(echo);

    // Dispatch to service registry
#if __has_include("service_registry.h")
    // Split cmd from args at first space
    char cmd_buf[128];
    strncpy(cmd_buf, txt, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';
    char* space = strchr(cmd_buf, ' ');
    const char* args = "";
    if (space) {
        *space = '\0';
        args = space + 1;
    }
    // Convert to uppercase for command matching
    for (char* p = cmd_buf; *p; p++) *p = toupper((unsigned char)*p);

    if (!ServiceRegistry::dispatchCommand(cmd_buf, args)) {
        term_append("[shell] Unknown command");
    } else {
        term_append("[shell] OK");
    }
#else
    term_append("[shell] No service registry");
#endif

    lv_textarea_set_text(ta, "");
}

void terminal_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);

    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(viewport, 4, 0);
    lv_obj_set_style_pad_gap(viewport, 4, 0);

    // Title
    lv_obj_t* title = lv_label_create(viewport);
    lv_label_set_text(title, LV_SYMBOL_KEYBOARD " TERMINAL");
    lv_obj_set_style_text_color(title, T_CYAN, 0);
    lv_obj_set_style_text_font(title, tritium_shell::uiFont(), 0);

    // Log area — scrollable container with monospace text
    lv_obj_t* log_container = lv_obj_create(viewport);
    lv_obj_set_width(log_container, lv_pct(100));
    lv_obj_set_flex_grow(log_container, 1);
    lv_obj_set_style_bg_color(log_container, T_VOID, 0);
    lv_obj_set_style_bg_opa(log_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(log_container, T_CYAN, 0);
    lv_obj_set_style_border_opa(log_container, LV_OPA_20, 0);
    lv_obj_set_style_border_width(log_container, 1, 0);
    lv_obj_set_style_radius(log_container, 4, 0);
    lv_obj_set_style_pad_all(log_container, 4, 0);
    lv_obj_set_scrollbar_mode(log_container, LV_SCROLLBAR_MODE_AUTO);

    s_term_log = lv_label_create(log_container);
    lv_label_set_text(s_term_log, "Tritium-OS Terminal\nType a command below.\n");
    lv_obj_set_width(s_term_log, lv_pct(100));
    lv_label_set_long_mode(s_term_log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_term_log, T_GREEN, 0);
    lv_obj_set_style_text_font(s_term_log, tritium_shell::uiSmallFont(), 0);

    // Input row
    lv_obj_t* input_row = lv_obj_create(viewport);
    lv_obj_set_width(input_row, lv_pct(100));
    lv_obj_set_height(input_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(input_row, 0, 0);
    lv_obj_set_style_pad_gap(input_row, 4, 0);
    lv_obj_set_style_bg_opa(input_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_row, 0, 0);
    lv_obj_remove_flag(input_row, LV_OBJ_FLAG_SCROLLABLE);

    // Prompt label
    lv_obj_t* prompt = lv_label_create(input_row);
    lv_label_set_text(prompt, ">");
    lv_obj_set_style_text_color(prompt, T_CYAN, 0);
    lv_obj_set_style_text_font(prompt, tritium_shell::uiFont(), 0);
    lv_obj_set_style_pad_top(prompt, 6, 0);

    // Text input
    s_term_input = lv_textarea_create(input_row);
    lv_obj_set_flex_grow(s_term_input, 1);
    lv_obj_set_height(s_term_input, 32);
    lv_textarea_set_one_line(s_term_input, true);
    lv_textarea_set_placeholder_text(s_term_input, "command...");
    lv_obj_set_style_bg_color(s_term_input, T_SURFACE2, 0);
    lv_obj_set_style_bg_opa(s_term_input, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_term_input, T_GREEN, 0);
    lv_obj_set_style_text_font(s_term_input, tritium_shell::uiSmallFont(), 0);
    lv_obj_set_style_border_color(s_term_input, T_CYAN, 0);
    lv_obj_set_style_border_opa(s_term_input, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_term_input, 1, 0);
    lv_obj_set_style_radius(s_term_input, 4, 0);
    lv_obj_add_event_cb(s_term_input, term_submit_cb, LV_EVENT_READY, nullptr);

    // Send button
    lv_obj_t* send_btn = tritium_theme::createButton(input_row, LV_SYMBOL_RIGHT);
    lv_obj_set_size(send_btn, 40, 32);
    lv_obj_add_event_cb(send_btn, [](lv_event_t* /*e*/) {
        // Simulate READY event on textarea to trigger submit
        lv_obj_send_event(s_term_input, LV_EVENT_READY, nullptr);
    }, LV_EVENT_CLICKED, nullptr);

    // Seed the log with boot info
    term_append("Tritium-OS Terminal v1.0");
    term_append("Type commands (e.g. HELP, STATUS, WIFI_STATUS)");
    term_append("---");

    // Refresh timer
    s_term_timer = lv_timer_create(term_refresh, 500, nullptr);
}

// ===========================================================================
//  Map Viewer App — offline map tiles from MBTiles on SD card
// ===========================================================================

// Map state
static lv_obj_t* s_map_canvas = nullptr;
static lv_obj_t* s_map_coord_lbl = nullptr;
static lv_obj_t* s_map_info_lbl = nullptr;
static double s_map_lat = 37.7749;   // Default: San Francisco
static double s_map_lon = -122.4194;
static uint8_t s_map_zoom = 12;
static bool s_map_dirty = true;
static bool s_map_opened = false;

#if MAP_APP_AVAILABLE

static void map_render_tile() {
    if (!s_map_canvas || !mbtiles::is_open()) return;

    uint32_t tile_x = mbtiles::lon_to_tile_x(s_map_lon, s_map_zoom);
    uint32_t tile_y = mbtiles::lat_to_tile_y(s_map_lat, s_map_zoom);

    // Update coordinate label
    if (s_map_coord_lbl) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%.4f, %.4f  Z%d  [%u,%u]",
                 s_map_lat, s_map_lon, s_map_zoom, tile_x, tile_y);
        lv_label_set_text(s_map_coord_lbl, buf);
    }

    // Load the tile from MBTiles
    size_t tile_len = 0;
    uint8_t* tile_data = mbtiles::get_tile(s_map_zoom, tile_x, tile_y, tile_len);

    if (!tile_data) {
        // Draw "no tile" placeholder
        lv_canvas_fill_bg(s_map_canvas, lv_color_hex(0x0a0a14), LV_OPA_COVER);
        lv_layer_t layer;
        lv_canvas_init_layer(s_map_canvas, &layer);
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = lv_color_hex(0x333355);
        label_dsc.text = "No tile data";
        label_dsc.font = tritium_shell::uiFont();
        label_dsc.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t area = {20, 100, 236, 140};
        lv_draw_label(&layer, &label_dsc, &area);
        lv_canvas_finish_layer(s_map_canvas, &layer);

        if (s_map_info_lbl) {
            lv_label_set_text(s_map_info_lbl, "No tile available at this location");
        }
        return;
    }

    // Use LVGL's image decoder to render the PNG tile to the canvas
    // Create an image descriptor pointing to the in-memory PNG data
    lv_image_dsc_t png_dsc;
    memset(&png_dsc, 0, sizeof(png_dsc));
    png_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    png_dsc.header.w = 256;
    png_dsc.header.h = 256;
    png_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    png_dsc.data = tile_data;
    png_dsc.data_size = tile_len;

    // For now, just fill the canvas with a color based on tile existence
    // and show a confirmation that the tile was loaded.
    // Full PNG-to-canvas rendering requires LVGL image decode pipeline.
    lv_canvas_fill_bg(s_map_canvas, lv_color_hex(0x0e1a0e), LV_OPA_COVER);

    // Draw a grid pattern to indicate tile position
    lv_layer_t layer;
    lv_canvas_init_layer(s_map_canvas, &layer);

    // Draw tile border
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_TRANSP;
    rect_dsc.border_color = lv_color_hex(0x00f0ff);
    rect_dsc.border_width = 1;
    rect_dsc.border_opa = LV_OPA_50;
    lv_area_t rect_area = {0, 0, 255, 255};
    lv_draw_rect(&layer, &rect_dsc, &rect_area);

    // Draw coordinate info on tile
    lv_draw_label_dsc_t lbl_dsc;
    lv_draw_label_dsc_init(&lbl_dsc);
    lbl_dsc.color = lv_color_hex(0x00f0ff);
    lbl_dsc.font = tritium_shell::uiSmallFont();
    lbl_dsc.align = LV_TEXT_ALIGN_CENTER;

    char info[64];
    snprintf(info, sizeof(info), "Tile %u/%u/%u\n%zu bytes (PNG)",
             s_map_zoom, tile_x, tile_y, tile_len);
    lbl_dsc.text = info;
    lv_area_t lbl_area = {10, 110, 246, 160};
    lv_draw_label(&layer, &lbl_dsc, &lbl_area);

    lv_canvas_finish_layer(s_map_canvas, &layer);

    if (s_map_info_lbl) {
        char ibuf[64];
        snprintf(ibuf, sizeof(ibuf), "Tile loaded: %zu bytes", tile_len);
        lv_label_set_text(s_map_info_lbl, ibuf);
    }

    free(tile_data);
}

static void map_refresh(lv_timer_t* /*t*/) {
    if (!s_map_dirty) return;
    s_map_dirty = false;
    map_render_tile();
}

static void map_zoom_in_cb(lv_event_t* /*e*/) {
    mbtiles::Metadata md;
    uint8_t max_z = 20;
    if (mbtiles::get_metadata(md)) max_z = md.max_zoom;
    if (s_map_zoom < max_z) { s_map_zoom++; s_map_dirty = true; }
}

static void map_zoom_out_cb(lv_event_t* /*e*/) {
    mbtiles::Metadata md;
    uint8_t min_z = 0;
    if (mbtiles::get_metadata(md)) min_z = md.min_zoom;
    if (s_map_zoom > min_z) { s_map_zoom--; s_map_dirty = true; }
}

// Pan by fraction of current tile span
static void map_pan(double dlat, double dlon) {
    // At current zoom, one tile spans this many degrees
    double lon_span = 360.0 / (1 << s_map_zoom);
    double lat_span = lon_span * 0.7;  // Approximation for Mercator
    s_map_lat += dlat * lat_span;
    s_map_lon += dlon * lon_span;
    // Clamp
    if (s_map_lat > 85.0) s_map_lat = 85.0;
    if (s_map_lat < -85.0) s_map_lat = -85.0;
    if (s_map_lon > 180.0) s_map_lon -= 360.0;
    if (s_map_lon < -180.0) s_map_lon += 360.0;
    s_map_dirty = true;
}

#endif  // MAP_APP_AVAILABLE

void map_app_create(lv_obj_t* viewport) {
    lv_obj_clean(viewport);
    lv_obj_set_flex_flow(viewport, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(viewport, 4, 0);
    lv_obj_set_style_pad_gap(viewport, 4, 0);

#if !MAP_APP_AVAILABLE
    tritium_theme::createLabel(viewport, "Map viewer not available (missing mbtiles_reader)");
    return;
#else
    // Try to open MBTiles if not already open
    if (!mbtiles::is_open()) {
        // Try common paths
        if (!mbtiles::open("/sdcard/data/map.mbtiles") &&
            !mbtiles::open("/sdcard/data/ca_tiles.mbtiles") &&
            !mbtiles::open("/sdcard/map.mbtiles")) {
            s_map_opened = false;
        } else {
            s_map_opened = true;
        }
    } else {
        s_map_opened = true;
    }

    // Header
    lv_obj_t* hdr = lv_obj_create(viewport);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, LV_SYMBOL_GPS " MAP");
    lv_obj_set_style_text_color(title, T_CYAN, 0);
    lv_obj_set_style_text_font(title, tritium_shell::uiFont(), 0);

    s_map_coord_lbl = lv_label_create(hdr);
    lv_label_set_text(s_map_coord_lbl, "---");
    lv_obj_set_style_text_color(s_map_coord_lbl, T_GHOST, 0);
    lv_obj_set_style_text_font(s_map_coord_lbl, tritium_shell::uiSmallFont(), 0);

    if (!s_map_opened) {
        tritium_theme::createLabel(viewport,
            "No map data found.\n\n"
            "Place an MBTiles file at:\n"
            "  /sdcard/data/map.mbtiles\n\n"
            "Use tools/download_tiles.py to\n"
            "generate offline map tiles.");

        // Show MBTiles metadata if available
        mbtiles::Metadata md;
        if (mbtiles::get_metadata(md)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s — %d tiles, zoom %d-%d",
                     md.name, md.tile_count, md.min_zoom, md.max_zoom);
            tritium_theme::createLabel(viewport, buf);
        }
        return;
    }

    // Center on MBTiles center if available
    mbtiles::Metadata md;
    if (mbtiles::get_metadata(md) && md.center_lat != 0.0) {
        s_map_lat = md.center_lat;
        s_map_lon = md.center_lon;
        s_map_zoom = md.center_zoom > 0 ? md.center_zoom : 12;
    }

    // Map canvas — 256x256 (one tile), allocated in PSRAM
    s_map_canvas = lv_canvas_create(viewport);
    static uint16_t* canvas_buf = nullptr;
    if (!canvas_buf) {
#ifndef SIMULATOR
        canvas_buf = (uint16_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
#endif
        if (!canvas_buf) canvas_buf = (uint16_t*)malloc(256 * 256 * 2);
    }
    if (!canvas_buf) {
        tritium_theme::createLabel(viewport, "Out of memory for map canvas");
        return;
    }
    lv_canvas_set_buffer(s_map_canvas, canvas_buf, 256, 256, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(s_map_canvas, lv_color_hex(0x0a0a14), LV_OPA_COVER);
    lv_obj_set_style_border_color(s_map_canvas, T_CYAN, 0);
    lv_obj_set_style_border_width(s_map_canvas, 1, 0);
    lv_obj_set_style_border_opa(s_map_canvas, LV_OPA_30, 0);
    lv_obj_center(s_map_canvas);

    // Controls row
    lv_obj_t* ctrl = lv_obj_create(viewport);
    lv_obj_set_width(ctrl, lv_pct(100));
    lv_obj_set_height(ctrl, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl, 0, 0);
    lv_obj_set_style_pad_all(ctrl, 2, 0);
    lv_obj_set_style_pad_gap(ctrl, 4, 0);
    lv_obj_remove_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);

    // Pan buttons
    lv_obj_t* btn_up = tritium_theme::createButton(ctrl, LV_SYMBOL_UP);
    lv_obj_set_size(btn_up, 36, 28);
    lv_obj_add_event_cb(btn_up, [](lv_event_t*) { map_pan(0.5, 0); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* btn_down = tritium_theme::createButton(ctrl, LV_SYMBOL_DOWN);
    lv_obj_set_size(btn_down, 36, 28);
    lv_obj_add_event_cb(btn_down, [](lv_event_t*) { map_pan(-0.5, 0); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* btn_left = tritium_theme::createButton(ctrl, LV_SYMBOL_LEFT);
    lv_obj_set_size(btn_left, 36, 28);
    lv_obj_add_event_cb(btn_left, [](lv_event_t*) { map_pan(0, -0.5); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* btn_right = tritium_theme::createButton(ctrl, LV_SYMBOL_RIGHT);
    lv_obj_set_size(btn_right, 36, 28);
    lv_obj_add_event_cb(btn_right, [](lv_event_t*) { map_pan(0, 0.5); }, LV_EVENT_CLICKED, nullptr);

    // Spacer
    lv_obj_t* spacer = lv_obj_create(ctrl);
    lv_obj_set_size(spacer, 20, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);

    // Zoom buttons
    lv_obj_t* btn_zin = tritium_theme::createButton(ctrl, LV_SYMBOL_PLUS);
    lv_obj_set_size(btn_zin, 36, 28);
    lv_obj_add_event_cb(btn_zin, map_zoom_in_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* btn_zout = tritium_theme::createButton(ctrl, LV_SYMBOL_MINUS);
    lv_obj_set_size(btn_zout, 36, 28);
    lv_obj_add_event_cb(btn_zout, map_zoom_out_cb, LV_EVENT_CLICKED, nullptr);

    // Info label
    s_map_info_lbl = lv_label_create(viewport);
    lv_label_set_text(s_map_info_lbl, "Loading...");
    lv_obj_set_style_text_color(s_map_info_lbl, T_GHOST, 0);
    lv_obj_set_style_text_font(s_map_info_lbl, tritium_shell::uiSmallFont(), 0);

    // Initial render
    s_map_dirty = true;
    s_map_timer = lv_timer_create(map_refresh, 200, nullptr);
#endif
}

//  Register all new apps
// ===========================================================================

void register_all_apps() {
    // Map — offline tile map viewer
    tritium_shell::registerApp({"Map", "Offline maps", LV_SYMBOL_GPS,
                                true, map_app_create});

    // Mesh Chat — P2P messaging over ESP-NOW
    tritium_shell::registerApp({"Chat", "Mesh messaging", LV_SYMBOL_SHUFFLE,
                                false, mesh_chat_create});

    // Mesh Viewer — peers, topology, broadcast, stats
    tritium_shell::registerApp({"Mesh", "Network peers", LV_SYMBOL_LOOP,
                                true, mesh_app_create});

    // BLE Scanner — nearby Bluetooth devices
    tritium_shell::registerApp({"BLE", "Device scanner", LV_SYMBOL_BLUETOOTH,
                                true, ble_app_create});

    // Terminal — on-device serial console for debugging and commands
    tritium_shell::registerApp({"Terminal", "Serial console", LV_SYMBOL_KEYBOARD,
                                true, terminal_create});

    // File Manager — browse LittleFS and SD card
    tritium_shell::registerApp({"Files", "File browser", LV_SYMBOL_DRIVE,
                                true, files_app_create});

    // About — version, board info, memory stats
    tritium_shell::registerApp({"About", "System info", LV_SYMBOL_LIST,
                                true, about_create});
}

}  // namespace shell_apps
