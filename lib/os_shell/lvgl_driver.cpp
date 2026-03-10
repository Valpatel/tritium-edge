/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file lvgl_driver.cpp
 * @brief LVGL 9.2 display driver for esp_lcd panel backend.
 *
 * Two render modes with proper synchronization:
 *
 *   - RGB panels (43C-BOX): DIRECT mode using the panel's OWN framebuffers
 *     (zero-copy). The esp_lcd RGB driver allocates 2 PSRAM FBs internally
 *     and scans them via bounce-buffer DMA. LVGL renders into the back FB,
 *     and flush calls draw_bitmap which does an ATOMIC buffer swap (no copy)
 *     since the source pointer IS one of the panel's FBs.
 *
 *     Vsync sync uses the Espressif two-semaphore handshake:
 *       1. flush_cb gives sem_gui_ready ("rendering done")
 *       2. on_vsync ISR takes sem_gui_ready, gives sem_vsync_end
 *       3. flush_cb takes sem_vsync_end, calls draw_bitmap (swap)
 *     This ensures the swap waits for the NEXT vsync after rendering,
 *     not a stale vsync that fired during rendering.
 *
 *   - QSPI panels: FULL mode with full-screen PSRAM buffers (preferred) or
 *     PARTIAL mode with 1/10th-screen buffers (fallback). Each flush waits
 *     for DMA completion via semaphore. FULL mode renders the entire frame
 *     before flushing, so the panel receives a complete image per transfer —
 *     no mid-frame partial updates that cause tearing on fast-refreshing
 *     widgets like the system monitor bars.
 *
 * Tick source is handled by lv_conf.h (LV_TICK_CUSTOM = millis()).
 */

#if defined(ENABLE_SHELL)

#include "lvgl_driver.h"
#include "display.h"

#include <esp_heap_caps.h>
#include "tritium_compat.h"

static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_display_t* s_disp = nullptr;
static bool s_is_rgb = false;
static int s_width = 0;
static int s_height = 0;

// QSPI panels: DMA completion semaphore
static SemaphoreHandle_t s_flush_sem = nullptr;

// RGB panels: two-semaphore vsync handshake (Espressif pattern)
static SemaphoreHandle_t s_sem_gui_ready = nullptr;
static SemaphoreHandle_t s_sem_vsync_end = nullptr;

// Debug counters
static volatile uint32_t s_flush_count = 0;
static volatile uint32_t s_last_flush_ms = 0;
static uint8_t* s_buf1 = nullptr;

// ---------------------------------------------------------------------------
// Flush callback — called by LVGL when a draw buffer is ready to push
// ---------------------------------------------------------------------------

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    if (s_is_rgb) {
        // RGB panel with zero-copy double-buffered FBs.
        //
        // In DIRECT mode, LVGL renders dirty rects in-place into the back FB
        // and may call flush_cb MULTIPLE times per frame (once per dirty area).
        // Only swap on the LAST flush to avoid showing partially-updated frames.
        if (lv_display_flush_is_last(disp)) {
            // Two-semaphore vsync handshake (Espressif proven pattern):
            //   1. Give sem_gui_ready to signal "rendering done, ready to swap"
            //   2. Take sem_vsync_end to block until the NEXT vsync fires
            //
            // The on_vsync ISR only gives sem_vsync_end if it can take
            // sem_gui_ready, ensuring we wait for a FRESH vsync (not a stale
            // one that fired while LVGL was still rendering).
            if (s_sem_gui_ready && s_sem_vsync_end) {
                xSemaphoreGive(s_sem_gui_ready);
                xSemaphoreTake(s_sem_vsync_end, pdMS_TO_TICKS(50));
            }

            // Trigger the FB swap. Pass full-screen coordinates and the FB
            // base pointer so esp_lcd recognizes it as its own FB and does
            // an atomic swap (not a bounce-buffer copy).
            esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_width, s_height, px_map);

            s_flush_count++;
            s_last_flush_ms = millis();
        }
        // else: intermediate flush — no-op, pixels are already in the FB

        lv_display_flush_ready(disp);
    } else {
        // QSPI panels: push each area via DMA transfer.
        int x1 = area->x1;
        int y1 = area->y1;
        int x2 = area->x2 + 1;  // esp_lcd uses exclusive end coordinates
        int y2 = area->y2 + 1;

        esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, px_map);

        // Wait for DMA completion before LVGL can reuse the draw buffer.
        if (s_flush_sem) {
            xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(100));
        }

        s_flush_count++;
        s_last_flush_ms = millis();

        lv_display_flush_ready(disp);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace lvgl_driver {

lv_display_t* init(esp_lcd_panel_handle_t panel, int width, int height) {
    s_panel = panel;
    s_is_rgb = display_is_rgb();
    if (s_is_rgb) {
        // RGB panels: two-semaphore vsync handshake
        s_sem_gui_ready = (SemaphoreHandle_t)display_get_sem_gui_ready();
        s_sem_vsync_end = (SemaphoreHandle_t)display_get_sem_vsync_end();
    } else {
        // QSPI panels: DMA completion semaphore
        s_flush_sem = display_get_flush_semaphore();
    }
    s_width = width;
    s_height = height;

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    s_disp = lv_display_create(width, height);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    size_t buf_size;
    lv_display_render_mode_t mode;

    if (s_is_rgb) {
        // RGB panels: use the panel's OWN framebuffers (zero-copy pattern).
        //
        // The RGB panel driver (esp_lcd) allocates 2 PSRAM framebuffers internally
        // and continuously DMA-scans from one via bounce buffers. If we allocate
        // SEPARATE buffers and call draw_bitmap(), it does a multi-transfer
        // bounce-buffer copy that races with the panel scanout = tearing.
        //
        // Instead: give LVGL the panel's own FBs. When draw_bitmap() receives
        // a pointer that IS one of its own FBs, it performs an atomic buffer
        // swap (just changes which FB the bounce DMA reads from) instead of
        // copying. This swap is synchronized to vsync = zero tearing.
        void* fb0 = nullptr;
        void* fb1 = nullptr;
        esp_err_t err = display_get_rgb_framebuffers(&fb0, &fb1);

        if (err == ESP_OK && fb0 && fb1) {
            buf_size = (size_t)width * height * sizeof(uint16_t);
            mode = LV_DISPLAY_RENDER_MODE_DIRECT;
            s_buf1 = (uint8_t*)fb0;
            lv_display_set_buffers(s_disp, fb0, fb1, buf_size, mode);
            Serial.printf("[lvgl] RGB panel — DIRECT mode, zero-copy FB swap, %uKB x2\n",
                          (unsigned)(buf_size / 1024));
        } else {
            // Fallback: single FB (tearing possible but functional)
            if (fb0) {
                buf_size = (size_t)width * height * sizeof(uint16_t);
                mode = LV_DISPLAY_RENDER_MODE_DIRECT;
                s_buf1 = (uint8_t*)fb0;
                lv_display_set_buffers(s_disp, fb0, nullptr, buf_size, mode);
                Serial.printf("[lvgl] RGB panel — DIRECT mode, single FB fallback\n");
            } else {
                Serial.printf("[lvgl] FATAL: Cannot get RGB panel framebuffers\n");
                return nullptr;
            }
        }
    } else {
        // QSPI panels: prefer FULL mode to avoid mid-frame partial flushes
        // that cause tearing on rapidly-updating widgets (e.g. monitor bars).
        buf_size = (size_t)width * height * sizeof(uint16_t);
        mode = LV_DISPLAY_RENDER_MODE_FULL;
        Serial.printf("[lvgl] QSPI panel — FULL mode preferred, %uKB per buffer\n",
                      (unsigned)(buf_size / 1024));

        // Allocate double-buffered draw buffers in PSRAM
        s_buf1 = nullptr;
        uint8_t* buf2 = nullptr;

        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (psram_free > buf_size * 2 + 64 * 1024) {
            s_buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
            buf2 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        }

        if (!s_buf1 || !buf2) {
            if (s_buf1) { heap_caps_free(s_buf1); s_buf1 = nullptr; }
            if (buf2) heap_caps_free(buf2);

            // Fall back to PARTIAL mode with smaller buffers
            buf_size = (size_t)width * (height / 10) * sizeof(uint16_t);
            if (buf_size < 32 * 1024) buf_size = 32 * 1024;
            mode = LV_DISPLAY_RENDER_MODE_PARTIAL;
            s_buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
            buf2 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
            if (!s_buf1 || !buf2) {
                if (s_buf1) { heap_caps_free(s_buf1); s_buf1 = nullptr; }
                if (buf2) heap_caps_free(buf2);
                buf_size = 32 * 1024;
                s_buf1 = (uint8_t*)malloc(buf_size);
                buf2 = (uint8_t*)malloc(buf_size);
            }
            if (s_buf1) {
                Serial.printf("[lvgl] QSPI PARTIAL fallback (%uKB x%d)\n",
                              (unsigned)(buf_size / 1024), buf2 ? 2 : 1);
            }
        } else {
            Serial.printf("[lvgl] PSRAM buffers: %uKB x2\n",
                          (unsigned)(buf_size / 1024));
        }

        if (!s_buf1) {
            Serial.printf("[lvgl] FATAL: Cannot allocate draw buffers\n");
            return nullptr;
        }

        lv_display_set_buffers(s_disp, s_buf1, buf2, buf_size, mode);
    }

    Serial.printf("[lvgl] Display driver ready: %dx%d (%s, %s)\n",
                  width, height,
                  s_is_rgb ? "RGB" : "QSPI",
                  mode == LV_DISPLAY_RENDER_MODE_DIRECT ? "DIRECT" :
                  mode == LV_DISPLAY_RENDER_MODE_FULL ? "FULL" : "PARTIAL");
    return s_disp;
}

uint32_t tick() {
    return lv_timer_handler();
}

lv_display_t* display() {
    return s_disp;
}

bool isRgb() {
    return s_is_rgb;
}

uint32_t getFlushCount() {
    return s_flush_count;
}

uint32_t getLastFlushMs() {
    return s_last_flush_ms;
}

int getWidth() {
    return s_width;
}

int getHeight() {
    return s_height;
}

// For DIRECT mode, return the current draw buffer (full-screen framebuffer)
const uint8_t* getFramebuffer() {
    return s_buf1;
}

}  // namespace lvgl_driver

#endif  // ENABLE_SHELL
