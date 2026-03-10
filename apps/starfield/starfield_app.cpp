#include "starfield_app.h"
#include "tritium_compat.h"
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include "display.h"
#include <cstring>

// Minimal 5x7 bitmap font for overlay text (ASCII 32-127)
// Each char is 5 columns, each column is 7 bits (LSB = top row)
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
};

static constexpr float CRUISE_SPEED = 0.012f;
static constexpr float WARP_SPEED = 0.08f;
static constexpr uint32_t CRUISE_DURATION_MS = 10000;
static constexpr uint32_t WARP_DURATION_MS = 3000;
static constexpr uint32_t FPS_UPDATE_MS = 2000;

// --- RGB565 with SPI byte-swap ---
inline uint16_t StarfieldApp::rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
#if defined(BOARD_TOUCH_LCD_43C_BOX)
    return c;  // RGB parallel — no byte swap needed
#else
    return (c >> 8) | (c << 8);  // byte-swap for SPI transport
#endif
}

uint16_t StarfieldApp::tintedColor(float br, StarTint tint) {
    // Pure black and white starfield
    uint8_t v = (uint8_t)(br * 255.0f);
    return rgb565(v, v, v);

    // Original tinted colors (unused)
    (void)tint;
    float r, g, b;
    switch (tint) {
        case TINT_BLUE:
            r = br * 0.6f;
            g = br * 0.7f;
            b = br * 1.0f;
            break;
        case TINT_YELLOW:
            r = br * 1.0f;
            g = br * 0.9f;
            b = br * 0.4f;
            break;
        case TINT_RED:
            r = br * 1.0f;
            g = br * 0.4f;
            b = br * 0.3f;
            break;
        default:  // TINT_WHITE - slight blue-white
            r = br * 0.86f;
            g = br * 0.90f;
            b = br * 1.0f;
            break;
    }
    return rgb565((uint8_t)(r * 255.0f), (uint8_t)(g * 255.0f), (uint8_t)(b * 255.0f));
}

float StarfieldApp::currentSpeed() const {
    uint32_t elapsed = millis() - _warp_timer;
    float t = (float)elapsed / 500.0f;  // 500ms ramp time
    if (t > 1.0f) t = 1.0f;

    float ease = sinf(t * 1.5708f);  // pi/2

    if (_warping) {
        return CRUISE_SPEED + (WARP_SPEED - CRUISE_SPEED) * ease;
    } else {
        return WARP_SPEED - (WARP_SPEED - CRUISE_SPEED) * ease;
    }
}

void StarfieldApp::pushFramebuffer() {
    if (!_dma_buf) {
        // RGB parallel panels: push framebuffer directly (no SPI DMA needed)
        esp_lcd_panel_draw_bitmap(_panel, 0, 0, _w, _h, _framebuf);
        display_count_frame();
        return;
    }
    for (int y = 0; y < _h; y += CHUNK_ROWS) {
        int rows = CHUNK_ROWS;
        if (y + rows > _h) rows = _h - y;
        int pixels = _w * rows;
        memcpy(_dma_buf, &_framebuf[y * _w], pixels * 2);
        esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + rows, _dma_buf);
    }
    display_count_frame();
}

void StarfieldApp::setOverlayText(const char* line1, const char* line2, const char* line3) {
    if (line1) strncpy(_overlay[0], line1, sizeof(_overlay[0]) - 1);
    else _overlay[0][0] = '\0';
    if (line2) strncpy(_overlay[1], line2, sizeof(_overlay[1]) - 1);
    else _overlay[1][0] = '\0';
    if (line3) strncpy(_overlay[2], line3, sizeof(_overlay[2]) - 1);
    else _overlay[2][0] = '\0';
}

void StarfieldApp::drawChar(int x, int y, char c, uint16_t color) {
    if (c < ' ' || c > 'Z') return;  // Only support space through Z
    int idx = c - ' ';
    if (idx >= (int)(sizeof(font5x7) / sizeof(font5x7[0]))) return;

    for (int col = 0; col < 5; col++) {
        uint8_t bits = pgm_read_byte(&font5x7[idx][col]);
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < _w && py >= 0 && py < _h) {
                    _framebuf[py * _w + px] = color;
                }
            }
        }
    }
}

void StarfieldApp::drawString(int x, int y, const char* str, uint16_t color) {
    while (*str) {
        drawChar(x, y, *str, color);
        x += 6;  // 5px char + 1px gap
        str++;
    }
}

void StarfieldApp::setup(esp_lcd_panel_handle_t panel, int width, int height) {
    _panel = panel;
    _w = width;
    _h = height;

    Serial.printf("[starfield] Powered by Tritium\n");
    Serial.printf("[starfield] Display: %dx%d\n", _w, _h);

    // Allocate PSRAM framebuffer
    size_t fb_size = _w * _h * sizeof(uint16_t);
    _framebuf = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (!_framebuf) {
        Serial.printf("[starfield] FATAL: framebuffer alloc failed\n");
        while (1) delay(1000);
    }
    memset(_framebuf, 0, fb_size);

    // Allocate DMA transfer buffer for QSPI panels (chunked SPI push).
    // RGB parallel panels write directly to panel framebuffer — skip DMA.
#if defined(BOARD_TOUCH_LCD_43C_BOX)
    // RGB panel: esp_lcd_panel_draw_bitmap copies directly to panel FB in PSRAM
    _dma_buf = nullptr;
    Serial.printf("[starfield] RGB panel — direct framebuffer mode\n");
#else
    size_t dma_size = _w * CHUNK_ROWS * sizeof(uint16_t);
    _dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA);
    if (!_dma_buf) {
        Serial.printf("[starfield] No DMA buffer — using direct framebuffer push\n");
    }
#endif

    _starfield = new StarField(_w, _h);
    Serial.printf("[starfield] %d stars, running\n", _starfield->getStarCount());

    uint32_t now = millis();
    _fps_timer = now;
    _warp_timer = now;
    _warp_cycle_timer = now;
    _warping = false;
}

void StarfieldApp::loop() {
    uint32_t now = millis();

    // Warp speed cycling: alternate cruise/warp
    uint32_t cycle_elapsed = now - _warp_cycle_timer;
    if (!_warping && cycle_elapsed >= CRUISE_DURATION_MS) {
        _warping = true;
        _warp_timer = now;
        _warp_cycle_timer = now;
    } else if (_warping && cycle_elapsed >= WARP_DURATION_MS) {
        _warping = false;
        _warp_timer = now;
        _warp_cycle_timer = now;
    }

    float speed = currentSpeed();
    _starfield->update(speed);

    // Clear framebuffer
    memset(_framebuf, 0, _w * _h * sizeof(uint16_t));

    // Render stars
    const Star* stars = _starfield->getStars();
    int count = _starfield->getStarCount();

    for (int i = 0; i < count; i++) {
        int sx, sy;
        float brightness;
        if (_starfield->project(stars[i], sx, sy, brightness)) {
            uint16_t color = tintedColor(brightness, stars[i].tint);
            _framebuf[sy * _w + sx] = color;

            // Bright stars get cross pattern
            if (brightness > 0.6f) {
                if (sx > 0)      _framebuf[sy * _w + sx - 1] = color;
                if (sx < _w - 1) _framebuf[sy * _w + sx + 1] = color;
                if (sy > 0)      _framebuf[(sy - 1) * _w + sx] = color;
                if (sy < _h - 1) _framebuf[(sy + 1) * _w + sx] = color;
            }
        }
    }

    // Draw overlay text if set (commissioning info, etc.)
    if (_overlay[0][0]) {
        uint16_t bright = rgb565(255, 255, 255);
        uint16_t dim = rgb565(160, 160, 160);
        // Scale text position for larger displays
        int line_spacing = (_h > 300) ? 16 : 10;
        int y_start = _h / 2 - line_spacing * 2;
        drawString(8, y_start,                  _overlay[0], bright);
        drawString(8, y_start + line_spacing,   _overlay[1], dim);
        drawString(8, y_start + line_spacing*2, _overlay[2], dim);
    }

    // Push to display via DMA chunks
    pushFramebuffer();

    // FPS tracking
    _frame_count++;
    if (now - _fps_timer >= FPS_UPDATE_MS) {
        float fps = _frame_count * 1000.0f / (now - _fps_timer);
        Serial.printf("[starfield] %.1f FPS %s\n", fps, _warping ? "WARP" : "cruise");
        _frame_count = 0;
        _fps_timer = now;
    }
}
