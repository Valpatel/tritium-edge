// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#pragma once
#include "app.h"
#include <lvgl.h>
#include "ui_scaling.h"

class UiDemoApp : public App {
public:
    const char* name() override { return "UI Demo"; }
    bool usesLVGL() override { return true; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;

private:
    lv_obj_t* _status_bar = nullptr;
    lv_obj_t* _list = nullptr;
    uint32_t _uptime_timer = 0;
    UiScale _scale = {};
};
