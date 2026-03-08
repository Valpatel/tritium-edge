// Powered by Tritium
// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

#include "app.h"

// Minimal diagnostic app — proves display works via esp_lcd.
// Cycles solid color fills, shows Tritium splash, reports over serial.
class DiagApp : public App {
public:
    const char* name() const override { return "Diag"; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;

private:
    esp_lcd_panel_handle_t _panel = nullptr;
    int _w = 0;
    int _h = 0;
    uint16_t* _row_buf = nullptr;
    uint32_t _frame = 0;
    uint32_t _last_ms = 0;
};
