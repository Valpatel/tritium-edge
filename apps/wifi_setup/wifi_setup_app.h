// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#pragma once
#include "app.h"
#include <lvgl.h>
#include "ui_scaling.h"
#include "wifi_manager.h"

class WifiSetupApp : public App {
public:
    const char* name() override { return "WiFi Setup"; }
    bool usesLVGL() override { return true; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;

private:
    void createMainScreen();
    void createScanList();
    void createSavedScreen();
    void refreshScanList();
    void refreshSavedList();
    void refreshStatusBar();
    void showPasswordDialog(const char* ssid);
    void showConnecting(const char* ssid);

    static void onScanBtnClicked(lv_event_t* e);
    static void onSavedBtnClicked(lv_event_t* e);
    static void onBackBtnClicked(lv_event_t* e);
    static void onApItemClicked(lv_event_t* e);
    static void onSavedItemClicked(lv_event_t* e);
    static void onConnectBtnClicked(lv_event_t* e);
    static void onPasswordShowToggle(lv_event_t* e);
    static void onDeleteConfirm(lv_event_t* e);
    static void onWifiStateChange(WifiState state);

    WifiManager _wifi;
    UiScale _scale = {};

    // Main screen widgets
    lv_obj_t* _main_screen = nullptr;
    lv_obj_t* _status_bar = nullptr;
    lv_obj_t* _wifi_icon = nullptr;
    lv_obj_t* _scan_list = nullptr;
    lv_obj_t* _saved_screen = nullptr;
    lv_obj_t* _saved_list = nullptr;

    // Password dialog
    lv_obj_t* _pwd_dialog = nullptr;
    lv_obj_t* _pwd_input = nullptr;
    char _selected_ssid[33] = {};

    // Status
    uint32_t _scan_timer = 0;
    uint32_t _status_timer = 0;
    bool _scan_pending = false;

    static WifiSetupApp* _instance;
};
