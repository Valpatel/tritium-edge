/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "tritium_splash.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cstring>

// ---------------------------------------------------------------------------
// 5x7 bitmap font (ASCII 32-90, space through Z)
// Each char is 5 columns; each column byte has 7 bits (LSB = top row).
// ---------------------------------------------------------------------------
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
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x7F,0x20,0x18,0x20,0x7F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x03,0x04,0x78,0x04,0x03}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    // Lowercase (mapped from 'a'=65+32=97, index 65)
    {0x00,0x1C,0x22,0x41,0x00}, // [ (unused filler)
    {0x02,0x04,0x08,0x10,0x20}, // backslash
    {0x00,0x41,0x22,0x1C,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0x00,0x01,0x02,0x04,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7E,0x09,0x01,0x02}, // f
    {0x08,0x14,0x54,0x54,0x3C}, // g
    {0x7F,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7D,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00}, // j
    {0x7F,0x10,0x28,0x44,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78}, // m
    {0x7C,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7C}, // q
    {0x7C,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x20,0x7C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
};

static const int FONT_CHAR_COUNT = sizeof(font5x7) / sizeof(font5x7[0]);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// RGB565 with SPI byte-swap (matching starfield convention)
static inline uint16_t rgb565_swap(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (c >> 8) | (c << 8);
}

// Scale an 8-bit channel by brightness [0..255]
static inline uint8_t scale8(uint8_t val, uint8_t brightness) {
    return (uint8_t)(((uint16_t)val * brightness) >> 8);
}

// Draw one 5x7 char scaled by `scale` into framebuffer
static void draw_char_scaled(uint16_t* fb, int fb_w, int fb_h,
                             int x, int y, char c, uint16_t color, int scale) {
    if (c < ' ') return;
    int idx = c - ' ';
    if (idx < 0 || idx >= FONT_CHAR_COUNT) return;

    for (int col = 0; col < 5; col++) {
        uint8_t bits = pgm_read_byte(&font5x7[idx][col]);
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                // Fill a scale x scale block
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px >= 0 && px < fb_w && py >= 0 && py < fb_h) {
                            fb[py * fb_w + px] = color;
                        }
                    }
                }
            }
        }
    }
}

// Draw a string scaled, returns total pixel width
static int draw_string_scaled(uint16_t* fb, int fb_w, int fb_h,
                              int x, int y, const char* str,
                              uint16_t color, int scale) {
    int start_x = x;
    while (*str) {
        draw_char_scaled(fb, fb_w, fb_h, x, y, *str, color, scale);
        x += 6 * scale;  // 5px char + 1px gap, scaled
        str++;
    }
    return x - start_x;
}

// Measure string width in pixels at given scale
static int measure_string(const char* str, int scale) {
    int len = strlen(str);
    if (len == 0) return 0;
    return len * 6 * scale - scale;  // last char has no trailing gap
}

// Push framebuffer to display in chunks (DMA-friendly)
static void push_framebuffer(esp_lcd_panel_handle_t panel, uint16_t* fb,
                             int w, int h, uint16_t* dma_buf, int chunk_rows) {
    for (int y = 0; y < h; y += chunk_rows) {
        int rows = chunk_rows;
        if (y + rows > h) rows = h - y;
        int pixels = w * rows;
        memcpy(dma_buf, &fb[y * w], pixels * 2);
        esp_lcd_panel_draw_bitmap(panel, 0, y, w, y + rows, dma_buf);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void tritium_splash(esp_lcd_panel_handle_t panel, int w, int h) {
    Serial.printf("[splash] Tritium boot splash %dx%d\n", w, h);

    // Allocate PSRAM framebuffer
    size_t fb_size = w * h * sizeof(uint16_t);
    uint16_t* fb = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (!fb) {
        Serial.printf("[splash] PSRAM alloc failed (%d bytes), skipping\n", fb_size);
        return;
    }

    // DMA transfer buffer (64 rows)
    static const int CHUNK_ROWS = 64;
    size_t dma_size = w * CHUNK_ROWS * sizeof(uint16_t);
    uint16_t* dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA);
    if (!dma_buf) {
        Serial.printf("[splash] DMA alloc failed, skipping\n");
        heap_caps_free(fb);
        return;
    }

    // --- Layout ---
    // Choose scale based on display width
    int title_scale = 4;
    int sub_scale = 2;
    if (w < 240) {
        title_scale = 3;
        sub_scale = 1;
    } else if (w >= 600) {
        title_scale = 6;
        sub_scale = 3;
    }

    const char* title = "TRITIUM";
    const char* creator = "Created by Matthew Valancy";
    const char* powered = "Powered by Tritium v" TRITIUM_VERSION;

    int title_w = measure_string(title, title_scale);
    int creator_w = measure_string(creator, sub_scale);
    int powered_w = measure_string(powered, sub_scale);

    int title_h = 7 * title_scale;
    int sub_h = 7 * sub_scale;
    int gap = sub_h;  // vertical gap between lines

    int total_h = title_h + gap + sub_h + (gap / 2) + sub_h;
    int base_y = (h - total_h) / 2;

    int title_x = (w - title_w) / 2;
    int creator_x = (w - creator_w) / 2;
    int powered_x = (w - powered_w) / 2;

    int title_y = base_y;
    int creator_y = title_y + title_h + gap;
    int powered_y = creator_y + sub_h + (gap / 2);

    // Target colors (will be scaled by brightness)
    // Cyan #00F0FF -> r=0, g=240, b=255
    // Dim cyan for subtitles: ~40% brightness
    const uint8_t title_r = 0, title_g = 240, title_b = 255;
    const uint8_t sub_r = 0, sub_g = 96, sub_b = 102;

    // --- Animation: fade in (500ms), hold (1500ms), fade out (500ms) ---
    static const int FADE_IN_MS = 500;
    static const int HOLD_MS = 1500;
    static const int FADE_OUT_MS = 500;
    static const int FRAME_MS = 33;  // ~30 FPS

    uint32_t anim_start = millis();
    int total_ms = FADE_IN_MS + HOLD_MS + FADE_OUT_MS;

    while (true) {
        uint32_t elapsed = millis() - anim_start;
        if (elapsed >= (uint32_t)total_ms) break;

        // Compute brightness 0..255
        uint8_t brightness;
        if (elapsed < (uint32_t)FADE_IN_MS) {
            // Fade in: 0 -> 255
            brightness = (uint8_t)((elapsed * 255) / FADE_IN_MS);
        } else if (elapsed < (uint32_t)(FADE_IN_MS + HOLD_MS)) {
            // Hold at full
            brightness = 255;
        } else {
            // Fade out: 255 -> 0
            uint32_t fade_elapsed = elapsed - FADE_IN_MS - HOLD_MS;
            brightness = (uint8_t)(255 - (fade_elapsed * 255) / FADE_OUT_MS);
        }

        // Clear to black
        memset(fb, 0, fb_size);

        // Render text at current brightness
        uint16_t tcol = rgb565_swap(scale8(title_r, brightness),
                                    scale8(title_g, brightness),
                                    scale8(title_b, brightness));
        uint16_t scol = rgb565_swap(scale8(sub_r, brightness),
                                    scale8(sub_g, brightness),
                                    scale8(sub_b, brightness));

        draw_string_scaled(fb, w, h, title_x, title_y, title, tcol, title_scale);
        draw_string_scaled(fb, w, h, creator_x, creator_y, creator, scol, sub_scale);
        draw_string_scaled(fb, w, h, powered_x, powered_y, powered, scol, sub_scale);

        // Push to display
        push_framebuffer(panel, fb, w, h, dma_buf, CHUNK_ROWS);

        // Frame pacing
        uint32_t frame_time = millis() - anim_start - elapsed;
        if (frame_time < (uint32_t)FRAME_MS) {
            delay(FRAME_MS - frame_time);
        }
    }

    // Final black frame
    memset(fb, 0, fb_size);
    push_framebuffer(panel, fb, w, h, dma_buf, CHUNK_ROWS);

    // Free temp resources
    heap_caps_free(dma_buf);
    heap_caps_free(fb);

    Serial.printf("[splash] Done\n");
}
