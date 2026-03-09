// Powered by Tritium
// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "diag_app.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_mac.h>
#include <cstring>
#include <cstdio>

// 5x7 bitmap font (ASCII 32-126)
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 space
    {0x00,0x00,0x5F,0x00,0x00}, // 33 !
    {0x00,0x07,0x00,0x07,0x00}, // 34 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
    {0x23,0x13,0x08,0x64,0x62}, // 37 %
    {0x36,0x49,0x55,0x22,0x50}, // 38 &
    {0x00,0x05,0x03,0x00,0x00}, // 39 '
    {0x00,0x1C,0x22,0x41,0x00}, // 40 (
    {0x00,0x41,0x22,0x1C,0x00}, // 41 )
    {0x08,0x2A,0x1C,0x2A,0x08}, // 42 *
    {0x08,0x08,0x3E,0x08,0x08}, // 43 +
    {0x00,0x50,0x30,0x00,0x00}, // 44 ,
    {0x08,0x08,0x08,0x08,0x08}, // 45 -
    {0x00,0x60,0x60,0x00,0x00}, // 46 .
    {0x20,0x10,0x08,0x04,0x02}, // 47 /
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
    {0x00,0x42,0x7F,0x40,0x00}, // 49 1
    {0x42,0x61,0x51,0x49,0x46}, // 50 2
    {0x21,0x41,0x45,0x4B,0x31}, // 51 3
    {0x18,0x14,0x12,0x7F,0x10}, // 52 4
    {0x27,0x45,0x45,0x45,0x39}, // 53 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
    {0x01,0x71,0x09,0x05,0x03}, // 55 7
    {0x36,0x49,0x49,0x49,0x36}, // 56 8
    {0x06,0x49,0x49,0x29,0x1E}, // 57 9
    {0x00,0x36,0x36,0x00,0x00}, // 58 :
    {0x00,0x56,0x36,0x00,0x00}, // 59 ;
    {0x00,0x08,0x14,0x22,0x41}, // 60 <
    {0x14,0x14,0x14,0x14,0x14}, // 61 =
    {0x41,0x22,0x14,0x08,0x00}, // 62 >
    {0x02,0x01,0x51,0x09,0x06}, // 63 ?
    {0x32,0x49,0x79,0x41,0x3E}, // 64 @
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
    {0x7F,0x49,0x49,0x49,0x36}, // 66 B
    {0x3E,0x41,0x41,0x41,0x22}, // 67 C
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
    {0x7F,0x49,0x49,0x49,0x41}, // 69 E
    {0x7F,0x09,0x09,0x01,0x01}, // 70 F
    {0x3E,0x41,0x41,0x51,0x32}, // 71 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
    {0x00,0x41,0x7F,0x41,0x00}, // 73 I
    {0x20,0x40,0x41,0x3F,0x01}, // 74 J
    {0x7F,0x08,0x14,0x22,0x41}, // 75 K
    {0x7F,0x40,0x40,0x40,0x40}, // 76 L
    {0x7F,0x02,0x04,0x02,0x7F}, // 77 M
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
    {0x7F,0x09,0x09,0x09,0x06}, // 80 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 82 R
    {0x46,0x49,0x49,0x49,0x31}, // 83 S
    {0x01,0x01,0x7F,0x01,0x01}, // 84 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
    {0x7F,0x20,0x18,0x20,0x7F}, // 87 W
    {0x63,0x14,0x08,0x14,0x63}, // 88 X
    {0x03,0x04,0x78,0x04,0x03}, // 89 Y
    {0x61,0x51,0x49,0x45,0x43}, // 90 Z
    {0x00,0x00,0x7F,0x41,0x41}, // 91 [
    {0x02,0x04,0x08,0x10,0x20}, // 92 backslash
    {0x41,0x41,0x7F,0x00,0x00}, // 93 ]
    {0x04,0x02,0x01,0x02,0x04}, // 94 ^
    {0x40,0x40,0x40,0x40,0x40}, // 95 _
    {0x00,0x01,0x02,0x04,0x00}, // 96 `
    {0x20,0x54,0x54,0x54,0x78}, // 97 a
    {0x7F,0x48,0x44,0x44,0x38}, // 98 b
    {0x38,0x44,0x44,0x44,0x20}, // 99 c
    {0x38,0x44,0x44,0x48,0x7F}, // 100 d
    {0x38,0x54,0x54,0x54,0x18}, // 101 e
    {0x08,0x7E,0x09,0x01,0x02}, // 102 f
    {0x08,0x14,0x54,0x54,0x3C}, // 103 g
    {0x7F,0x08,0x04,0x04,0x78}, // 104 h
    {0x00,0x44,0x7D,0x40,0x00}, // 105 i
    {0x20,0x40,0x44,0x3D,0x00}, // 106 j
    {0x00,0x7F,0x10,0x28,0x44}, // 107 k
    {0x00,0x41,0x7F,0x40,0x00}, // 108 l
    {0x7C,0x04,0x18,0x04,0x78}, // 109 m
    {0x7C,0x08,0x04,0x04,0x78}, // 110 n
    {0x38,0x44,0x44,0x44,0x38}, // 111 o
    {0x7C,0x14,0x14,0x14,0x08}, // 112 p
    {0x08,0x14,0x14,0x18,0x7C}, // 113 q
    {0x7C,0x08,0x04,0x04,0x08}, // 114 r
    {0x48,0x54,0x54,0x54,0x20}, // 115 s
    {0x04,0x3F,0x44,0x40,0x20}, // 116 t
    {0x3C,0x40,0x40,0x20,0x7C}, // 117 u
    {0x1C,0x20,0x40,0x20,0x1C}, // 118 v
    {0x3C,0x40,0x30,0x40,0x3C}, // 119 w
    {0x44,0x28,0x10,0x28,0x44}, // 120 x
    {0x0C,0x50,0x50,0x50,0x3C}, // 121 y
    {0x44,0x64,0x54,0x4C,0x44}, // 122 z
    {0x00,0x08,0x36,0x41,0x00}, // 123 {
    {0x00,0x00,0x7F,0x00,0x00}, // 124 |
    {0x00,0x41,0x36,0x08,0x00}, // 125 }
    {0x08,0x04,0x08,0x10,0x08}, // 126 ~
};

// Known I2C device names
static const char* i2c_device_name(uint8_t addr) {
    switch (addr) {
        case 0x18: return "ES8311";
        case 0x20: return "TCA9554";
        case 0x34: return "AXP2101";
        case 0x3B: return "Touch";
        case 0x51: return "PCF85063";
        case 0x6B: return "QMI8658";
        default: return nullptr;
    }
}

// ============================================================================
// Drawing primitives
// ============================================================================

void DiagApp::clearScreen(uint16_t color) {
    int total = _w * _h;
    for (int i = 0; i < total; i++) _framebuf[i] = color;
}

void DiagApp::drawChar(int x, int y, char c, uint16_t color) {
    if (c < 32 || c > 126) return;
    const uint8_t* glyph = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                int px = x + col, py = y + row;
                if (px >= 0 && px < _w && py >= 0 && py < _h)
                    _framebuf[py * _w + px] = color;
            }
        }
    }
}

void DiagApp::drawString(int x, int y, const char* str, uint16_t color) {
    while (*str) {
        drawChar(x, y, *str++, color);
        x += FONT_W;
    }
}

void DiagApp::fillRect(int x, int y, int w, int h, uint16_t color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > _w) ? _w : (x + w);
    int y1 = (y + h > _h) ? _h : (y + h);
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            _framebuf[py * _w + px] = color;
}

void DiagApp::drawHLine(int x, int y, int w, uint16_t color) {
    if (y < 0 || y >= _h) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w > _w) ? _w : (x + w);
    for (int px = x0; px < x1; px++) _framebuf[y * _w + px] = color;
}

void DiagApp::drawBar(int x, int y, int w, int h, float value,
                      uint16_t fg, uint16_t bg) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    fillRect(x, y, w, h, bg);
    int filled = (int)(w * value);
    if (filled > 0) fillRect(x, y, filled, h, fg);
}

void DiagApp::pushBuffer() {
    for (int y = 0; y < _h; y += CHUNK_ROWS) {
        int rows = CHUNK_ROWS;
        if (y + rows > _h) rows = _h - y;
        memcpy(_dma_buf, &_framebuf[y * _w], _w * rows * 2);
        esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + rows, _dma_buf);
    }
}

// ============================================================================
// Setup
// ============================================================================

void DiagApp::setup(esp_lcd_panel_handle_t panel, int width, int height) {
    _panel = panel;
    _w = width;
    _h = height;

    // Colors
    COL_BG     = rgb565(0x00, 0x00, 0x00);
    COL_HEADER = rgb565(0x1A, 0x1A, 0x2E);
    COL_TEXT   = rgb565(0xDD, 0xDD, 0xDD);
    COL_DIM    = rgb565(0x88, 0x88, 0x88);
    COL_ACCENT = rgb565(0x00, 0x99, 0xFF);
    COL_GREEN  = rgb565(0x00, 0xCC, 0x66);
    COL_RED    = rgb565(0xFF, 0x44, 0x44);
    COL_YELLOW = rgb565(0xFF, 0xAA, 0x00);
    COL_BAR_BG = rgb565(0x22, 0x22, 0x22);

    // Allocate PSRAM framebuffer
    size_t fb_size = _w * _h * sizeof(uint16_t);
    _framebuf = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (!_framebuf) {
        Serial.println("[diag] FATAL: framebuffer alloc failed");
        return;
    }
    memset(_framebuf, 0, fb_size);

    // DMA transfer buffer
    size_t dma_size = _w * CHUNK_ROWS * sizeof(uint16_t);
    _dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA);
    if (!_dma_buf) {
        Serial.println("[diag] FATAL: DMA buffer alloc failed");
        return;
    }

    // I2C bus scan
#if defined(SENSOR_SDA) && defined(SENSOR_SCL)
    Wire.begin(SENSOR_SDA, SENSOR_SCL);
    Wire.setClock(400000);
#elif defined(IMU_SDA) && defined(IMU_SCL)
    Wire.begin(IMU_SDA, IMU_SCL);
    Wire.setClock(400000);
#endif

    _i2c_count = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            _i2c_devs[_i2c_count].addr = addr;
            _i2c_devs[_i2c_count].present = true;
            _i2c_count++;
            const char* name = i2c_device_name(addr);
            Serial.printf("[diag] I2C 0x%02X: %s\n", addr, name ? name : "unknown");
        }
    }
    _i2c_scanned = true;

    _page = 0;
    _page_timer = millis();
    _color_timer = millis();

    Serial.printf("[diag] Ready: %dx%d, %d I2C devices\n", _w, _h, _i2c_count);
}

// ============================================================================
// Loop
// ============================================================================

void DiagApp::loop() {
    if (!_framebuf || !_dma_buf) return;

    uint32_t now = millis();

    // Auto-advance pages
    if (now - _page_timer >= PAGE_INTERVAL_MS) {
        _page_timer = now;
        _page = (_page + 1) % NUM_PAGES;
    }

    // Check serial for manual page advance
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            _page = (_page + 1) % NUM_PAGES;
            _page_timer = now;
        }
    }

    clearScreen(COL_BG);

    int y = drawHeader(0);

    switch (_page) {
        case 0: drawMemoryPage(y); break;
        case 1: drawI2CPage(y); break;
        case 2: drawBoardPage(y); break;
        case 3: drawColorTestPage(y); break;
    }

    pushBuffer();
}

// ============================================================================
// Page renderers
// ============================================================================

int DiagApp::drawHeader(int y) {
    int hh = FONT_H + 4;
    fillRect(0, y, _w, hh, COL_HEADER);

    char title[40];
    snprintf(title, sizeof(title), "TRITIUM DIAG [%d/%d]", _page + 1, NUM_PAGES);
    drawString(MARGIN, y + 2, title, COL_ACCENT);

    // Uptime in top-right
    uint32_t up = millis() / 1000;
    char uptxt[20];
    snprintf(uptxt, sizeof(uptxt), "%luh%02lum", (unsigned long)(up / 3600),
             (unsigned long)((up % 3600) / 60));
    int tw = strlen(uptxt) * FONT_W;
    drawString(_w - tw - MARGIN, y + 2, uptxt, COL_DIM);

    return y + hh + 2;
}

int DiagApp::drawMemoryPage(int y) {
    int lineH = FONT_H + 2;
    int barW = _w - 2 * MARGIN - 4;

    drawString(MARGIN, y, "MEMORY", COL_ACCENT);
    y += lineH;

    // Internal heap
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    uint32_t minHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    float heapUsed = 1.0f - (float)freeHeap / (float)totalHeap;

    char buf[80];
    snprintf(buf, sizeof(buf), " Heap: %luK / %luK",
             (unsigned long)(freeHeap / 1024), (unsigned long)(totalHeap / 1024));
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    uint16_t hc = heapUsed > 0.85f ? COL_RED : (heapUsed > 0.7f ? COL_YELLOW : COL_GREEN);
    drawBar(MARGIN + 2, y, barW, 6, heapUsed, hc, COL_BAR_BG);
    y += 10;

    snprintf(buf, sizeof(buf), " Min free: %luK", (unsigned long)(minHeap / 1024));
    drawString(MARGIN, y, buf, COL_DIM);
    y += lineH;

    // Largest free block
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    snprintf(buf, sizeof(buf), " Largest block: %luK", (unsigned long)(largest / 1024));
    drawString(MARGIN, y, buf, COL_DIM);
    y += lineH;

    drawHLine(MARGIN, y + 1, _w - 2 * MARGIN, COL_DIM);
    y += 4;

    // PSRAM
    if (ESP.getPsramSize() > 0) {
        uint32_t freePsram = ESP.getFreePsram();
        uint32_t totalPsram = ESP.getPsramSize();
        float psUsed = 1.0f - (float)freePsram / (float)totalPsram;

        snprintf(buf, sizeof(buf), " PSRAM: %luK / %luK",
                 (unsigned long)(freePsram / 1024), (unsigned long)(totalPsram / 1024));
        drawString(MARGIN, y, buf, COL_TEXT);
        y += lineH;

        drawBar(MARGIN + 2, y, barW, 6, psUsed, COL_ACCENT, COL_BAR_BG);
        y += 10;
    }

    // Flash
    uint32_t flashSize = ESP.getFlashChipSize();
    snprintf(buf, sizeof(buf), " Flash: %luMB", (unsigned long)(flashSize / (1024 * 1024)));
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    return y;
}

int DiagApp::drawI2CPage(int y) {
    int lineH = FONT_H + 2;

    char buf[80];
    snprintf(buf, sizeof(buf), "I2C BUS SCAN (%d found)", _i2c_count);
    drawString(MARGIN, y, buf, COL_ACCENT);
    y += lineH;

    if (!_i2c_scanned) {
        drawString(MARGIN, y, " Scanning...", COL_DIM);
        return y + lineH;
    }

    if (_i2c_count == 0) {
        drawString(MARGIN, y, " No devices found", COL_RED);
        return y + lineH;
    }

    for (int i = 0; i < _i2c_count && y + lineH < _h - 10; i++) {
        uint8_t addr = _i2c_devs[i].addr;
        const char* devname = i2c_device_name(addr);
        if (devname) {
            snprintf(buf, sizeof(buf), " 0x%02X  %s", addr, devname);
            drawString(MARGIN, y, buf, COL_GREEN);
        } else {
            snprintf(buf, sizeof(buf), " 0x%02X  unknown", addr);
            drawString(MARGIN, y, buf, COL_TEXT);
        }
        y += lineH;
    }

    return y;
}

int DiagApp::drawBoardPage(int y) {
    int lineH = FONT_H + 2;
    char buf[80];

    drawString(MARGIN, y, "BOARD INFO", COL_ACCENT);
    y += lineH;

    snprintf(buf, sizeof(buf), " Display: %s", DISPLAY_DRIVER);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    snprintf(buf, sizeof(buf), " Interface: %s", DISPLAY_IF);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    snprintf(buf, sizeof(buf), " Resolution: %dx%d", _w, _h);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    drawHLine(MARGIN, y + 1, _w - 2 * MARGIN, COL_DIM);
    y += 4;

    // CPU info
    snprintf(buf, sizeof(buf), " CPU: ESP32-S3 @ %luMHz", (unsigned long)(ESP.getCpuFreqMHz()));
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    snprintf(buf, sizeof(buf), " SDK: %s", ESP.getSdkVersion());
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    // MAC address
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(buf, sizeof(buf), " MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    drawHLine(MARGIN, y + 1, _w - 2 * MARGIN, COL_DIM);
    y += 4;

    // Capabilities
    drawString(MARGIN, y, " Capabilities:", COL_ACCENT);
    y += lineH;

    int cx = MARGIN + FONT_W;
#if HAS_IMU
    drawString(cx, y, "IMU", COL_GREEN); cx += 4 * FONT_W;
#endif
#if HAS_PMIC
    drawString(cx, y, "PMIC", COL_GREEN); cx += 5 * FONT_W;
#endif
#if HAS_RTC
    drawString(cx, y, "RTC", COL_GREEN); cx += 4 * FONT_W;
#endif
#if HAS_AUDIO_CODEC
    drawString(cx, y, "AUD", COL_GREEN); cx += 4 * FONT_W;
#endif
#if HAS_CAMERA
    drawString(cx, y, "CAM", COL_GREEN); cx += 4 * FONT_W;
#endif
    y += lineH;

    return y;
}

int DiagApp::drawColorTestPage(int y) {
    int lineH = FONT_H + 2;

    drawString(MARGIN, y, "COLOR TEST", COL_ACCENT);
    y += lineH;

    // Solid color blocks
    struct { const char* name; uint16_t color; } colors[] = {
        {"RED",   rgb565(0xFF, 0x00, 0x00)},
        {"GREEN", rgb565(0x00, 0xFF, 0x00)},
        {"BLUE",  rgb565(0x00, 0x00, 0xFF)},
        {"WHITE", rgb565(0xFF, 0xFF, 0xFF)},
        {"CYAN",  rgb565(0x00, 0xFF, 0xFF)},
        {"YELLOW",rgb565(0xFF, 0xFF, 0x00)},
    };

    int blockH = 16;
    int blockW = (_w - 2 * MARGIN - 10) / 3;

    for (int i = 0; i < 6; i++) {
        int col = i % 3;
        int row = i / 3;
        int bx = MARGIN + col * (blockW + 5);
        int by = y + row * (blockH + lineH + 2);

        fillRect(bx, by, blockW, blockH, colors[i].color);
        drawString(bx, by + blockH + 2, colors[i].name, COL_DIM);
    }

    y += 2 * (blockH + lineH + 2) + 4;

    // Gradient test (horizontal grayscale)
    drawString(MARGIN, y, "Grayscale:", COL_DIM);
    y += lineH;

    int gradW = _w - 2 * MARGIN;
    for (int px = 0; px < gradW; px++) {
        uint8_t v = (uint8_t)(px * 255 / gradW);
        uint16_t gc = rgb565(v, v, v);
        for (int py = y; py < y + 8 && py < _h; py++) {
            int fx = MARGIN + px;
            if (fx < _w) _framebuf[py * _w + fx] = gc;
        }
    }
    y += 12;

    return y;
}
