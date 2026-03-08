#include "effects_app.h"
#include "gfx_effects.h"

#ifdef SIMULATOR
#include "sim_hal.h"
#else
#include <Arduino.h>
#endif

#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <cstring>

// Minimal 5x7 font for overlay (space through Z)
static const uint8_t font5x7[][5] PROGMEM = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00},
    {0x08,0x2A,0x1C,0x2A,0x08}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x01,0x01}, {0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x04,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},
};

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void drawChar(uint16_t* fb, int fbw, int fbh, int x, int y, char c, uint16_t color) {
    if (c < ' ' || c > 'P') return;
    int idx = c - ' ';
    if (idx >= (int)(sizeof(font5x7) / sizeof(font5x7[0]))) return;
    for (int col = 0; col < 5; col++) {
        uint8_t bits = pgm_read_byte(&font5x7[idx][col]);
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                int px = x + col, py = y + row;
                if (px >= 0 && px < fbw && py >= 0 && py < fbh)
                    fb[py * fbw + px] = color;
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

static ScannerEffect      fx_scanner;
static MatrixRainEffect   fx_matrix;
static ParticleEffect     fx_particles;
static FireEffect         fx_fire;
static TunnelEffect       fx_tunnel;
static MetaballsEffect    fx_metaballs;
static PlasmaEffect       fx_plasma;

static GfxEffect* effects[] = {
    &fx_scanner,
    &fx_matrix,
    &fx_particles,
    &fx_fire,
    &fx_tunnel,
    &fx_metaballs,
    &fx_plasma,
};
static constexpr int NUM_EFFECTS = sizeof(effects) / sizeof(effects[0]);

void EffectsApp::pushFramebuffer() {
    for (int y = 0; y < _h; y += CHUNK_ROWS) {
        int rows = CHUNK_ROWS;
        if (y + rows > _h) rows = _h - y;
        int pixels = _w * rows;
        memcpy(_dma_buf, &_framebuf[y * _w], pixels * 2);
        esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + rows, _dma_buf);
    }
}

void EffectsApp::setup(esp_lcd_panel_handle_t panel, int width, int height) {
    _panel = panel;
    _w = width;
    _h = height;

    size_t fb_size = _w * _h * sizeof(uint16_t);
    _framebuf = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (!_framebuf) {
        Serial.printf("[effects] Framebuffer alloc failed\n");
        return;
    }
    memset(_framebuf, 0, fb_size);

    size_t dma_size = _w * CHUNK_ROWS * sizeof(uint16_t);
    _dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!_dma_buf) {
        _dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_SPIRAM);
    }

    for (int i = 0; i < NUM_EFFECTS; i++) {
        effects[i]->init(_w, _h);
    }

    _current = 0;
    _effect_start = millis();
    _fps_timer = millis();
    _last_frame = millis();
    _frame_count = 0;
    _last_time = 0;

    Serial.printf("[effects] Effects demo: %dx%d, %d effects, target %d FPS\n",
                  _w, _h, NUM_EFFECTS, TARGET_FPS);
}

void EffectsApp::drawOverlay(const char* name) {
    if (!_framebuf) return;

    // Effect name - bottom left
    drawString(_framebuf, _w, _h, 6, _h - 12, name, 0xFFFF);

    // FPS - top right
    if (_fps > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f FPS", _fps);
        int len = strlen(buf);
        drawString(_framebuf, _w, _h, _w - len * 6 - 4, 4, buf, 0x7BEF);
    }

    // Effect counter - top left
    char idx_buf[16];
    snprintf(idx_buf, sizeof(idx_buf), "%d/%d", _current + 1, NUM_EFFECTS);
    drawString(_framebuf, _w, _h, 4, 4, idx_buf, 0x7BEF);
}

void EffectsApp::loop() {
    if (!_framebuf || !_dma_buf) return;

    uint32_t now = millis();

    // Frame rate limiting
    uint32_t elapsed_frame = now - _last_frame;
    if (elapsed_frame < FRAME_MIN_MS) {
        delay(FRAME_MIN_MS - elapsed_frame);
        now = millis();
    }
    _last_frame = now;

    // Compute dt
    float now_s = now / 1000.0f;
    float dt = now_s - _last_time;
    if (dt > 0.1f) dt = 0.033f;
    if (dt <= 0) dt = 0.033f;
    _last_time = now_s;

    // Cycle effects
    uint32_t effect_elapsed = now - _effect_start;
    if (effect_elapsed > EFFECT_DURATION_MS) {
        _current = (_current + 1) % NUM_EFFECTS;
        _effect_start = now;
        memset(_framebuf, 0, _w * _h * 2);
    }

    // Update and render
    GfxEffect* fx = effects[_current];
    fx->update(dt);
    fx->render(_framebuf, _w, _h);

    drawOverlay(fx->name());
    pushFramebuffer();

    // FPS tracking
    _frame_count++;
    if (now - _fps_timer >= 1000) {
        _fps = _frame_count * 1000.0f / (now - _fps_timer);
        _frame_count = 0;
        _fps_timer = now;
    }
}
