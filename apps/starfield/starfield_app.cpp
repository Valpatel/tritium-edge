#include "starfield_app.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <cstring>

static constexpr float CRUISE_SPEED = 0.012f;
static constexpr float WARP_SPEED = 0.08f;
static constexpr uint32_t CRUISE_DURATION_MS = 10000;
static constexpr uint32_t WARP_DURATION_MS = 3000;
static constexpr uint32_t FPS_UPDATE_MS = 2000;

// --- RGB565 with SPI byte-swap ---
inline uint16_t StarfieldApp::rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (c >> 8) | (c << 8);  // byte-swap for SPI
}

uint16_t StarfieldApp::tintedColor(float br, StarTint tint) {
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
    for (int y = 0; y < _h; y += CHUNK_ROWS) {
        int rows = CHUNK_ROWS;
        if (y + rows > _h) rows = _h - y;
        int pixels = _w * rows;
        memcpy(_dma_buf, &_framebuf[y * _w], pixels * 2);
        esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + rows, _dma_buf);
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
        Serial.println("[starfield] FATAL: framebuffer alloc failed");
        while (1) delay(1000);
    }
    memset(_framebuf, 0, fb_size);

    // Allocate DMA transfer buffer (64 rows at a time)
    size_t dma_size = _w * CHUNK_ROWS * sizeof(uint16_t);
    _dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA);
    if (!_dma_buf) {
        Serial.println("[starfield] FATAL: DMA buffer alloc failed");
        while (1) delay(1000);
    }

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
