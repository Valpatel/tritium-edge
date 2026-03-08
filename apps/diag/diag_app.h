// Powered by Tritium
// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

#include "app.h"
#include <esp_heap_caps.h>

// On-device diagnostic app — displays system health directly on screen.
// Shows: memory stats, I2C bus scan, crash info, uptime, board info.
// Cycles through pages automatically or via serial "DIAG_NEXT" command.
class DiagApp : public App {
public:
    const char* name() override { return "Diag"; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;

private:
    // Rendering
    void clearScreen(uint16_t color);
    void drawChar(int x, int y, char c, uint16_t color);
    void drawString(int x, int y, const char* str, uint16_t color);
    void fillRect(int x, int y, int w, int h, uint16_t color);
    void drawHLine(int x, int y, int w, uint16_t color);
    void drawBar(int x, int y, int w, int h, float value, uint16_t fg, uint16_t bg);
    void pushBuffer();

    // Pages
    int drawHeader(int y);
    int drawMemoryPage(int y);
    int drawI2CPage(int y);
    int drawBoardPage(int y);
    int drawColorTestPage(int y);

    static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
        uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        return (c >> 8) | (c << 8);
    }

    esp_lcd_panel_handle_t _panel = nullptr;
    int _w = 0;
    int _h = 0;
    uint16_t* _framebuf = nullptr;
    uint16_t* _dma_buf = nullptr;
    static constexpr int CHUNK_ROWS = 64;
    static constexpr int FONT_W = 6;
    static constexpr int FONT_H = 8;
    static constexpr int MARGIN = 4;

    // Page cycling
    int _page = 0;
    static constexpr int NUM_PAGES = 4;
    uint32_t _page_timer = 0;
    static constexpr uint32_t PAGE_INTERVAL_MS = 5000;

    // I2C scan results (cached once at boot)
    struct I2CDevice {
        uint8_t addr;
        bool present;
    };
    static constexpr int MAX_I2C_DEVS = 128;
    I2CDevice _i2c_devs[MAX_I2C_DEVS];
    int _i2c_count = 0;
    bool _i2c_scanned = false;

    // Color test state
    uint32_t _color_frame = 0;
    uint32_t _color_timer = 0;

    // Colors
    uint16_t COL_BG;
    uint16_t COL_HEADER;
    uint16_t COL_TEXT;
    uint16_t COL_DIM;
    uint16_t COL_ACCENT;
    uint16_t COL_GREEN;
    uint16_t COL_RED;
    uint16_t COL_YELLOW;
    uint16_t COL_BAR_BG;
};
