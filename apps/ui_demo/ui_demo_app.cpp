// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#include "ui_demo_app.h"
#include "ui_init.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include <cstdio>

#ifdef SIMULATOR
#include <SDL2/SDL.h>
static uint32_t millis() { return SDL_GetTicks(); }
#ifndef DISPLAY_DRIVER
#define DISPLAY_DRIVER "Simulator"
#endif
#else
#include <Arduino.h>
#endif

static void on_settings_item_click(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t* label = lv_obj_get_child(btn, 0);
    if (!label) return;

    const char* text = lv_label_get_text(label);
    char msg[64];
    snprintf(msg, sizeof(msg), "Selected: %s", text);
    ui_show_notification("Settings", msg, 2000);
}

void UiDemoApp::setup(esp_lcd_panel_handle_t panel, int width, int height) {
    lv_display_t* disp = ui_init(panel, width, height);
    ui_apply_dark_theme(disp);

    _scale = ui_compute_scale(width, height);

    lv_obj_t* scr = lv_screen_active();

    // Main layout: vertical flex (status bar + content)
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_pad_gap(scr, 0, 0);

    // Status bar
    _status_bar = ui_create_status_bar(scr, _scale);
    ui_status_bar_set_title(_status_bar, DISPLAY_DRIVER);
    ui_status_bar_set_status(_status_bar, LV_SYMBOL_WIFI " " LV_SYMBOL_BATTERY_FULL);

    // Scrollable settings list
    _list = ui_create_settings_list(scr, _scale);

    // Add demo items
    lv_list_add_text(_list, "Display");

    char res_buf[32];
    snprintf(res_buf, sizeof(res_buf), "Resolution: %dx%d", width, height);
    lv_obj_t* btn;

    btn = lv_list_add_btn(_list, LV_SYMBOL_IMAGE, res_buf);
    lv_obj_add_event_cb(btn, on_settings_item_click, LV_EVENT_CLICKED, nullptr);

    btn = lv_list_add_btn(_list, LV_SYMBOL_EYE_OPEN, "Brightness");
    lv_obj_add_event_cb(btn, on_settings_item_click, LV_EVENT_CLICKED, nullptr);

    btn = lv_list_add_btn(_list, LV_SYMBOL_REFRESH, "Rotation");
    lv_obj_add_event_cb(btn, on_settings_item_click, LV_EVENT_CLICKED, nullptr);

    lv_list_add_text(_list, "System");

    btn = lv_list_add_btn(_list, LV_SYMBOL_WIFI, "WiFi Settings");
    lv_obj_add_event_cb(btn, on_settings_item_click, LV_EVENT_CLICKED, nullptr);

    btn = lv_list_add_btn(_list, LV_SYMBOL_SETTINGS, "Device Info");
    lv_obj_add_event_cb(btn, on_settings_item_click, LV_EVENT_CLICKED, nullptr);

    btn = lv_list_add_btn(_list, LV_SYMBOL_DOWNLOAD, "Firmware Update");
    lv_obj_add_event_cb(btn, on_settings_item_click, LV_EVENT_CLICKED, nullptr);

    btn = lv_list_add_btn(_list, LV_SYMBOL_POWER, "Restart");
    lv_obj_add_event_cb(btn, on_settings_item_click, LV_EVENT_CLICKED, nullptr);

    lv_list_add_text(_list, "Apps");

    btn = lv_list_add_btn(_list, LV_SYMBOL_PLAY, "Starfield Demo");
    lv_obj_add_event_cb(btn, on_settings_item_click, LV_EVENT_CLICKED, nullptr);

    btn = lv_list_add_btn(_list, LV_SYMBOL_AUDIO, "Audio Visualizer");
    lv_obj_add_event_cb(btn, on_settings_item_click, LV_EVENT_CLICKED, nullptr);

    // Show startup notification
    ui_show_notification("Welcome", "UI Demo loaded.\nSwipe to scroll.", 3000);

    _uptime_timer = millis();
}

void UiDemoApp::loop() {
    ui_tick();

    // Update status bar uptime every 5 seconds
    uint32_t now = millis();
    if (now - _uptime_timer >= 5000) {
        _uptime_timer = now;
        uint32_t secs = now / 1000;
        char buf[32];
        snprintf(buf, sizeof(buf), "%um%02us " LV_SYMBOL_BATTERY_FULL,
                 (unsigned)(secs / 60), (unsigned)(secs % 60));
        ui_status_bar_set_status(_status_bar, buf);
    }
}
