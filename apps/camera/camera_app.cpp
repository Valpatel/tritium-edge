#include "camera_app.h"
#include "hal_camera.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <cstring>

static CameraHAL cam;
static uint32_t _frameCount = 0;
static uint32_t _lastFpsTime = 0;

// Minimal 5x7 bitmap font (space through Z) for FPS overlay
static const uint8_t font5x7[][5] PROGMEM = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x00,0x08,0x14,0x22,0x41}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x41,0x22,0x14,0x08,0x00}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x01,0x01}, // F
    {0x3E,0x41,0x41,0x51,0x32}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x04,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
};

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static float _lastFps = 0.0f;

static void drawChar(uint16_t* fb, int fbw, int fbh, int x, int y, char c, uint16_t color) {
    if (c < ' ' || c > 'P') return;
    int idx = c - ' ';
    if (idx >= (int)(sizeof(font5x7) / sizeof(font5x7[0]))) return;
    for (int col = 0; col < 5; col++) {
        uint8_t bits = pgm_read_byte(&font5x7[idx][col]);
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < fbw && py >= 0 && py < fbh) {
                    fb[py * fbw + px] = color;
                }
            }
        }
    }
}

static void drawString(uint16_t* fb, int fbw, int fbh, int x, int y,
                        const char* str, uint16_t color) {
    while (*str) {
        drawChar(fb, fbw, fbh, x, y, *str, color);
        x += 6;
        str++;
    }
}

void CameraApp::pushFramebuffer() {
    for (int y = 0; y < _h; y += CHUNK_ROWS) {
        int rows = CHUNK_ROWS;
        if (y + rows > _h) rows = _h - y;
        int pixels = _w * rows;
        memcpy(_dma_buf, &_framebuf[y * _w], pixels * 2);
        esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + rows, _dma_buf);
    }
}

void CameraApp::setup(esp_lcd_panel_handle_t panel, int width, int height) {
    _panel = panel;
    _w = width;
    _h = height;

    // Allocate PSRAM framebuffer
    size_t fb_size = _w * _h * sizeof(uint16_t);
    _framebuf = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (!_framebuf) {
        Serial.printf("[camera] Framebuffer alloc failed (%d bytes)\n", fb_size);
        return;
    }
    memset(_framebuf, 0, fb_size);

    // DMA chunk buffer in internal RAM for fast transfer
    size_t dma_size = _w * CHUNK_ROWS * sizeof(uint16_t);
    _dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!_dma_buf) {
        // Fall back to PSRAM
        _dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_SPIRAM);
    }

    // Clear display
    pushFramebuffer();

    if (!cam.init(CamResolution::QVGA_320x240, CamPixelFormat::RGB565)) {
        Serial.printf("[camera] Camera init FAILED\n");
        // Draw error message
        uint16_t red = rgb565(255, 0, 0);
        drawString(_framebuf, _w, _h, 10, _h / 2, "CAMERA INIT FAILED", red);
        pushFramebuffer();
        return;
    }

    cam.setFlip(false, true);

    Serial.printf("[camera] Camera ready: %dx%d -> %dx%d\n",
             cam.getWidth(), cam.getHeight(), _w, _h);
    _lastFpsTime = millis();
}

void CameraApp::loop() {
    if (!cam.available() || !_framebuf || !_dma_buf) return;

    CameraFrame* f = cam.capture();
    if (!f || !f->data) return;

    const uint16_t* src = (const uint16_t*)f->data;
    uint16_t fw = f->width;   // 320
    uint16_t fh = f->height;  // 240

    // Scale camera frame to fill display
    // For 320x480: duplicate each row 2x (QVGA 240 rows -> 480 rows)
    // For other resolutions: simple nearest-neighbor scaling
    if (fw == (uint16_t)_w && fh * 2 == (uint16_t)_h) {
        // Fast path: 320x240 -> 320x480, duplicate rows
        for (uint16_t row = 0; row < fh && (row * 2 + 1) < (uint16_t)_h; row++) {
            const uint16_t* srcRow = &src[row * fw];
            uint16_t* dstRow1 = &_framebuf[(row * 2) * _w];
            uint16_t* dstRow2 = &_framebuf[(row * 2 + 1) * _w];
            memcpy(dstRow1, srcRow, fw * 2);
            memcpy(dstRow2, srcRow, fw * 2);
        }
    } else {
        // Generic nearest-neighbor scale
        for (int dy = 0; dy < _h; dy++) {
            int sy = dy * fh / _h;
            if (sy >= fh) sy = fh - 1;
            const uint16_t* srcRow = &src[sy * fw];
            uint16_t* dstRow = &_framebuf[dy * _w];
            for (int dx = 0; dx < _w; dx++) {
                int sx = dx * fw / _w;
                if (sx >= fw) sx = fw - 1;
                dstRow[dx] = srcRow[sx];
            }
        }
    }

    cam.releaseFrame();

    // FPS overlay
    _frameCount++;
    uint32_t now = millis();
    if (now - _lastFpsTime >= 2000) {
        _lastFps = _frameCount * 1000.0f / (now - _lastFpsTime);
        Serial.printf("[camera] FPS: %.1f\n", _lastFps);
        _frameCount = 0;
        _lastFpsTime = now;
    }

    // Draw FPS text in top-left corner
    if (_lastFps > 0) {
        char fps_str[16];
        snprintf(fps_str, sizeof(fps_str), "%.1f FPS", _lastFps);
        // Black background box
        for (int y = 0; y < 10; y++) {
            for (int x = 0; x < 60; x++) {
                if (x < _w && y < _h)
                    _framebuf[y * _w + x] = 0;
            }
        }
        drawString(_framebuf, _w, _h, 2, 2, fps_str, rgb565(0, 255, 0));
    }

    pushFramebuffer();
}
