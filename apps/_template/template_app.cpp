#include "template_app.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <cstring>

void TemplateApp::setup(esp_lcd_panel_handle_t panel, int width, int height) {
    Serial.printf("App: %s (%dx%d)\n", name(), width, height);

    _panel = panel;
    _w = width;
    _h = height;

    // Framebuffer in PSRAM (full frame, RGB565)
    size_t fb_size = _w * _h * sizeof(uint16_t);
    _framebuf = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);

    // DMA transfer buffer in internal SRAM (one chunk worth of rows)
    size_t dma_size = _w * CHUNK_ROWS * sizeof(uint16_t);
    _dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA);

    // Clear to black
    memset(_framebuf, 0, fb_size);
    pushFramebuffer();

    // Initialize your app state here.
}

void TemplateApp::loop() {
    // Called repeatedly. Render one frame into _framebuf, then push.
    //
    // Example: write a pixel at (_frame % _w, _frame % _h)
    //   uint16_t color = 0xFFFF;  // white, already byte-swapped
    //   _framebuf[((_frame % _h) * _w) + (_frame % _w)] = color;
    //
    // Note: QSPI panels need byte-swapped RGB565: (c >> 8) | (c << 8)

    _frame++;

    pushFramebuffer();
}

void TemplateApp::pushFramebuffer() {
    // DMA-chunked transfer: copy PSRAM framebuffer to internal SRAM in slices,
    // then push each slice to the panel via esp_lcd.
    for (int y = 0; y < _h; y += CHUNK_ROWS) {
        int rows = (y + CHUNK_ROWS <= _h) ? CHUNK_ROWS : (_h - y);
        size_t chunk_bytes = _w * rows * sizeof(uint16_t);
        memcpy(_dma_buf, &_framebuf[y * _w], chunk_bytes);
        esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + rows, _dma_buf);
    }
}
