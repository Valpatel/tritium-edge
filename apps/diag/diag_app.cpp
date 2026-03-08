// Powered by Tritium
// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "diag_app.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// RGB565 byte-swapped colors for SPI wire format
static constexpr uint16_t COLOR_RED   = 0x00F8;
static constexpr uint16_t COLOR_GREEN = 0xE007;
static constexpr uint16_t COLOR_BLUE  = 0x1F00;
static constexpr uint16_t COLOR_WHITE = 0xFFFF;
static constexpr uint16_t COLOR_BLACK = 0x0000;
static constexpr uint16_t COLOR_CYAN  = 0xFF07;

void DiagApp::setup(esp_lcd_panel_handle_t panel, int width, int height) {
    _panel = panel;
    _w = width;
    _h = height;

    // Allocate a single-row buffer for fills
    size_t row_bytes = _w * sizeof(uint16_t);
    _row_buf = (uint16_t*)heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
    if (!_row_buf) {
        _row_buf = (uint16_t*)heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM);
    }

    _last_ms = millis();

    // Draw initial Tritium splash — cyan on black
    if (_row_buf) {
        // Fill black
        memset(_row_buf, 0, row_bytes);
        for (int y = 0; y < _h; y++) {
            esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + 1, _row_buf);
        }

        // Draw cyan stripe in the middle third (simple splash indicator)
        for (int i = 0; i < _w; i++) _row_buf[i] = COLOR_CYAN;
        int y_start = _h / 3;
        int y_end = 2 * _h / 3;
        for (int y = y_start; y < y_end; y++) {
            esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + 1, _row_buf);
        }
    }

    Serial.println("{\"tritium\":\"diag\",\"status\":\"ready\"}");
}

void DiagApp::loop() {
    if (!_row_buf || !_panel) return;

    uint32_t now = millis();
    if (now - _last_ms < 2000) return;  // Cycle every 2 seconds
    _last_ms = now;

    // Cycle through colors
    static const uint16_t colors[] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_WHITE, COLOR_CYAN, COLOR_BLACK};
    static const char* names[] = {"RED", "GREEN", "BLUE", "WHITE", "CYAN", "BLACK"};
    int idx = _frame % 6;

    uint32_t t0 = millis();
    for (int i = 0; i < _w; i++) _row_buf[i] = colors[idx];
    for (int y = 0; y < _h; y++) {
        esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + 1, _row_buf);
    }
    uint32_t fill_ms = millis() - t0;

    Serial.printf("{\"diag\":\"fill\",\"color\":\"%s\",\"frame\":%lu,\"ms\":%lu}\n",
                  names[idx], _frame, fill_ms);
    _frame++;
}
