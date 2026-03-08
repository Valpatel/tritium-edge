// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#include "ui_init.h"
#include <cstdlib>
#include <cstring>

#ifndef SIMULATOR
#include <esp_heap_caps.h>
#endif

static esp_lcd_panel_handle_t s_panel = nullptr;
static int s_width = 0;
static int s_height = 0;

// DMA-capable internal SRAM buffer for chunked transfer to QSPI panels
static uint16_t* s_dma_buf = nullptr;
static constexpr int DMA_CHUNK_ROWS = 32;

static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2;
    int y2 = area->y2;
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;

#ifndef SIMULATOR
    // Push in chunks through DMA buffer for QSPI compatibility
    if (s_dma_buf) {
        const uint16_t* src = (const uint16_t*)px_map;
        int rows_done = 0;
        while (rows_done < h) {
            int chunk = (h - rows_done < DMA_CHUNK_ROWS) ? (h - rows_done) : DMA_CHUNK_ROWS;
            int pixels = w * chunk;
            memcpy(s_dma_buf, src + (rows_done * w), pixels * sizeof(uint16_t));
            esp_lcd_panel_draw_bitmap(s_panel, x1, y1 + rows_done,
                                      x1 + w, y1 + rows_done + chunk,
                                      s_dma_buf);
            rows_done += chunk;
        }
    } else {
        esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x1 + w, y1 + h, px_map);
    }
#endif

    lv_display_flush_ready(disp);
}

static void allocate_buffers(size_t buf_size, uint8_t*& buf1, uint8_t*& buf2) {
    buf1 = nullptr;
    buf2 = nullptr;

#ifndef SIMULATOR
    // Try PSRAM first on ESP32
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > buf_size * 2) {
        buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        buf2 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    }
#endif

    if (!buf1) {
        buf1 = (uint8_t*)malloc(buf_size);
        buf2 = (uint8_t*)malloc(buf_size);
    }
}

lv_display_t* ui_init(esp_lcd_panel_handle_t panel, int width, int height) {
    s_panel = panel;
    s_width = width;
    s_height = height;

    lv_init();

    lv_display_t* disp = lv_display_create(width, height);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    // Use 1/10th of screen size for partial rendering
    size_t buf_size = (size_t)width * (height / 10) * sizeof(uint16_t);
    if (buf_size < 32 * 1024) buf_size = 32 * 1024;

    uint8_t* buf1;
    uint8_t* buf2;
    allocate_buffers(buf_size, buf1, buf2);

    lv_display_set_buffers(disp, buf1, buf2, buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

#ifndef SIMULATOR
    // Allocate DMA-capable buffer for QSPI panel transfers
    size_t dma_size = (size_t)width * DMA_CHUNK_ROWS * sizeof(uint16_t);
    s_dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#endif

    return disp;
}

uint32_t ui_tick() {
    return lv_timer_handler();
}
