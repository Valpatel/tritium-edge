/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "boot_sequence.h"

#if defined(ENABLE_SETTINGS) || defined(ENABLE_DIAG)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cstdio>
#include <cmath>

// ============================================================================
// 5×7 bitmap font (ASCII 32–122)
// ============================================================================

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
    {0x00,0x1C,0x22,0x41,0x00}, // [
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

// ============================================================================
// Color palette — RGB565, byte-swapped for SPI
// ============================================================================

// Byte-swap flag: true for QSPI/SPI panels, false for RGB parallel panels
static bool _swap_bytes = false;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return _swap_bytes ? ((c >> 8) | (c << 8)) : c;
}

static uint16_t scale_color(uint16_t col, float f) {
    uint16_t c = _swap_bytes ? ((col >> 8) | (col << 8)) : col;
    int r = ((c >> 11) & 0x1F);
    int g = ((c >> 5) & 0x3F);
    int b = (c & 0x1F);
    r = (int)(r * f); if (r > 31) r = 31;
    g = (int)(g * f); if (g > 63) g = 63;
    b = (int)(b * f); if (b > 31) b = 31;
    uint16_t out = (r << 11) | (g << 5) | b;
    return _swap_bytes ? ((out >> 8) | (out << 8)) : out;
}

// Palette (initialized in init) — all cool blue/cyan tones
static uint16_t COL_CYAN, COL_GREEN, COL_WARN, COL_ALERT;
static uint16_t COL_DIM_CYAN, COL_WHITE, COL_DARK_CYAN, COL_LIGHT_BLUE;

// ============================================================================
// 3D math
// ============================================================================

struct Vec3 { float x, y, z; };
struct Vec2 { int x, y; };

static Vec3 rotY(Vec3 v, float a) {
    float c = cosf(a), s = sinf(a);
    return {v.x*c + v.z*s, v.y, -v.x*s + v.z*c};
}
static Vec3 rotX(Vec3 v, float a) {
    float c = cosf(a), s = sinf(a);
    return {v.x, v.y*c - v.z*s, v.y*s + v.z*c};
}
static Vec2 project(Vec3 v, int cx, int cy, float s) {
    float d = 5.0f / (5.0f + v.z);
    return {cx + (int)(v.x * s * d), cy - (int)(v.y * s * d)};
}

// ============================================================================
// Crystal geometry — elongated octahedron
// ============================================================================

static const Vec3 VERTS[6] = {
    { 0.0f,  1.6f,  0.0f},  // top
    { 0.0f, -1.6f,  0.0f},  // bottom
    { 1.0f,  0.0f,  0.0f},  // right
    {-1.0f,  0.0f,  0.0f},  // left
    { 0.0f,  0.0f,  1.0f},  // front
    { 0.0f,  0.0f, -1.0f},  // back
};
static const int EDGES[12][2] = {
    {0,2},{0,3},{0,4},{0,5},   // top to equator
    {1,2},{1,3},{1,4},{1,5},   // bottom to equator
    {2,4},{4,3},{3,5},{5,2},   // equator ring
};

// ============================================================================
// Particle orbits
// ============================================================================

static const int NUM_PARTICLES = 20;
struct Particle {
    float radius, height, speed, angle, phase;
};
static Particle _particles[NUM_PARTICLES];

// ============================================================================
// Background stars
// ============================================================================

static const int NUM_STARS = 60;
struct Star { int16_t x, y; uint16_t color; };
static Star _stars[NUM_STARS];

// ============================================================================
// RNG
// ============================================================================

static uint32_t _rng;
static uint32_t xrand() {
    _rng ^= _rng << 13;
    _rng ^= _rng >> 17;
    _rng ^= _rng << 5;
    return _rng;
}
static float frand() { return (xrand() & 0xFFFF) / 65536.0f; }

// ============================================================================
// Module state
// ============================================================================

static esp_lcd_panel_handle_t _panel = nullptr;
static uint16_t* _fb = nullptr;
static uint16_t* _dma = nullptr;
static int _w = 0, _h = 0;
static int _crystal_clip_y = 9999;    // Y limit for crystal rendering (set in showLogo)
static int _scale = 2;                // text scale for service log
static int _title_scale = 4;          // title text scale
static int _line_y = 0;               // cursor Y for next service line
static int _margin_x = 0;
static int _line_height = 0;
static float _crystal_angle = 0.0f;   // current crystal rotation
static int _crystal_cx, _crystal_cy;  // crystal center
static int _crystal_r;                // crystal pixel radius
static int _crystal_bottom;           // Y below crystal + title area
static const int CHUNK = 64;

// ============================================================================
// Drawing primitives
// ============================================================================

static inline void px(int x, int y, uint16_t c) {
    if (x >= 0 && x < _w && y >= 0 && y < _h && y < _crystal_clip_y)
        _fb[y * _w + x] = c;
}
// Unclipped pixel for text drawing (ignores crystal clip)
static inline void px_text(int x, int y, uint16_t c) {
    if (x >= 0 && x < _w && y >= 0 && y < _h) _fb[y * _w + x] = c;
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_glow_line(int x0, int y0, int x1, int y1,
                            uint16_t core, float depth) {
    // Depth-based brightness: 0.25 (far) to 1.0 (near)
    float bright = 0.25f + 0.75f * depth;
    uint16_t g1 = scale_color(core, bright * 0.35f);
    uint16_t g2 = scale_color(core, bright * 0.15f);
    uint16_t cc = scale_color(core, bright);

    // Outer glow (8 offsets at distance ~1.5)
    static const int ox2[] = {-2, 2, 0, 0, -1, -1, 1, 1};
    static const int oy2[] = { 0, 0,-2, 2, -1,  1,-1, 1};
    for (int i = 0; i < 8; i++)
        draw_line(x0+ox2[i], y0+oy2[i], x1+ox2[i], y1+oy2[i], g2);

    // Inner glow (4 cardinal offsets)
    static const int ox1[] = {-1, 1, 0, 0};
    static const int oy1[] = { 0, 0,-1, 1};
    for (int i = 0; i < 4; i++)
        draw_line(x0+ox1[i], y0+oy1[i], x1+ox1[i], y1+oy1[i], g1);

    // Core
    draw_line(x0, y0, x1, y1, cc);
}

static void draw_dot_glow(int x, int y, uint16_t core, int r) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            float dist = sqrtf((float)(dx*dx + dy*dy));
            if (dist <= r) {
                float f = 1.0f - dist / (float)(r + 1);
                px(x + dx, y + dy, scale_color(core, f * f));
            }
        }
    }
    px(x, y, core);
}

static void draw_char_scaled(int x, int y, char c, uint16_t color, int sc) {
    if (c < ' ') return;
    int idx = c - ' ';
    if (idx < 0 || idx >= FONT_CHAR_COUNT) return;
    for (int col = 0; col < 5; col++) {
        uint8_t bits = pgm_read_byte(&font5x7[idx][col]);
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int sy = 0; sy < sc; sy++)
                    for (int sx = 0; sx < sc; sx++)
                        px_text(x + col*sc + sx, y + row*sc + sy, color);
            }
        }
    }
}

static int draw_string(int x, int y, const char* str, uint16_t color, int sc) {
    int sx = x;
    while (*str) {
        draw_char_scaled(x, y, *str, color, sc);
        x += 6 * sc;
        str++;
    }
    return x - sx;
}

static int measure(const char* str, int sc) {
    int n = strlen(str);
    return n > 0 ? n * 6 * sc - sc : 0;
}

static void draw_hline(int x, int y, int len, uint16_t color, int th) {
    for (int dy = 0; dy < th; dy++)
        for (int dx = 0; dx < len; dx++)
            px_text(x + dx, y + dy, color);
}

static void clear_region(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > _h) y1 = _h;
    memset(&_fb[y0 * _w], 0, (y1 - y0) * _w * sizeof(uint16_t));
}

// ============================================================================
// Display output
// ============================================================================

static void push_full() {
    if (!_panel || !_fb || !_dma) return;
    for (int y = 0; y < _h; y += CHUNK) {
        int rows = (y + CHUNK > _h) ? _h - y : CHUNK;
        memcpy(_dma, &_fb[y * _w], rows * _w * 2);
        esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + rows, _dma);
    }
}

static void push_region(int y0, int y1) {
    if (!_panel || !_fb || !_dma) return;
    if (y0 < 0) y0 = 0;
    if (y1 > _h) y1 = _h;
    for (int y = y0; y < y1; y += CHUNK) {
        int rows = (y + CHUNK > y1) ? y1 - y : CHUNK;
        memcpy(_dma, &_fb[y * _w], rows * _w * 2);
        esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + rows, _dma);
    }
}

// ============================================================================
// Crystal renderer
// ============================================================================

static void init_stars() {
    for (int i = 0; i < NUM_STARS; i++) {
        _stars[i].x = xrand() % _w;
        _stars[i].y = xrand() % _h;
        float b = 0.1f + frand() * 0.3f;
        _stars[i].color = scale_color(COL_CYAN, b);
    }
}

static void init_particles() {
    for (int i = 0; i < NUM_PARTICLES; i++) {
        _particles[i].radius = 1.5f + frand() * 1.2f;
        _particles[i].height = (frand() - 0.5f) * 2.0f;
        _particles[i].speed  = 0.8f + frand() * 1.5f;
        _particles[i].angle  = frand() * 6.2832f;
        _particles[i].phase  = frand() * 6.2832f;
    }
}

static void draw_stars() {
    for (int i = 0; i < NUM_STARS; i++)
        px(_stars[i].x, _stars[i].y, _stars[i].color);
}

// Render one frame of the crystal at the given rotation angles.
// edge_progress: 0..1, how much of each edge is visible (for materialization).
// brightness: overall brightness multiplier.
static void draw_crystal(float angle_y, float angle_x,
                          float edge_progress, float brightness) {
    // Transform vertices
    Vec3 rot[6];
    Vec2 proj[6];
    for (int i = 0; i < 6; i++) {
        rot[i] = rotY(VERTS[i], angle_y);
        rot[i] = rotX(rot[i], angle_x);
        proj[i] = project(rot[i], _crystal_cx, _crystal_cy, (float)_crystal_r);
    }

    // Sort edges by depth (back-to-front: largest avg_z first)
    struct ES { int idx; float z; };
    ES sorted[12];
    for (int i = 0; i < 12; i++) {
        int a = EDGES[i][0], b = EDGES[i][1];
        sorted[i] = {i, (rot[a].z + rot[b].z) * 0.5f};
    }
    for (int i = 0; i < 11; i++)
        for (int j = i + 1; j < 12; j++)
            if (sorted[j].z > sorted[i].z) {
                ES tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
            }

    // Draw outer crystal edges
    for (int i = 0; i < 12; i++) {
        int ei = sorted[i].idx;
        int a = EDGES[ei][0], b = EDGES[ei][1];

        // Edge materialization: grow from midpoint
        float prog = edge_progress;
        // Stagger edges slightly for cascade effect
        float stagger = 1.0f - (float)ei / 18.0f;
        float ep = (prog - (1.0f - stagger)) / stagger;
        if (ep < 0.0f) ep = 0.0f;
        if (ep > 1.0f) ep = 1.0f;
        if (ep < 0.01f) continue;

        int mx = (proj[a].x + proj[b].x) / 2;
        int my = (proj[a].y + proj[b].y) / 2;
        int x0 = mx + (int)((proj[a].x - mx) * ep);
        int y0 = my + (int)((proj[a].y - my) * ep);
        int x1 = mx + (int)((proj[b].x - mx) * ep);
        int y1 = my + (int)((proj[b].y - my) * ep);

        // Depth → 0 (far) to 1 (near)
        float depth = 0.5f - sorted[i].z / 4.0f;
        if (depth < 0.0f) depth = 0.0f;
        if (depth > 1.0f) depth = 1.0f;

        draw_glow_line(x0, y0, x1, y1,
                        scale_color(COL_CYAN, brightness), depth);
    }

    // Inner crystal (smaller, counter-rotating, magenta tint)
    Vec3 irot[6];
    Vec2 iproj[6];
    for (int i = 0; i < 6; i++) {
        Vec3 v = {VERTS[i].x * 0.45f, VERTS[i].y * 0.45f, VERTS[i].z * 0.45f};
        irot[i] = rotY(v, -angle_y * 1.4f);
        irot[i] = rotX(irot[i], -angle_x * 0.7f);
        iproj[i] = project(irot[i], _crystal_cx, _crystal_cy, (float)_crystal_r);
    }

    // Sort inner edges
    ES isorted[12];
    for (int i = 0; i < 12; i++) {
        int a = EDGES[i][0], b = EDGES[i][1];
        isorted[i] = {i, (irot[a].z + irot[b].z) * 0.5f};
    }
    for (int i = 0; i < 11; i++)
        for (int j = i + 1; j < 12; j++)
            if (isorted[j].z > isorted[i].z) {
                ES tmp = isorted[i]; isorted[i] = isorted[j]; isorted[j] = tmp;
            }

    for (int i = 0; i < 12; i++) {
        int ei = isorted[i].idx;
        int a = EDGES[ei][0], b = EDGES[ei][1];
        if (edge_progress < 0.3f) continue;
        float ep = (edge_progress - 0.3f) / 0.7f;
        if (ep > 1.0f) ep = 1.0f;

        int mx = (iproj[a].x + iproj[b].x) / 2;
        int my = (iproj[a].y + iproj[b].y) / 2;
        int x0 = mx + (int)((iproj[a].x - mx) * ep);
        int y0 = my + (int)((iproj[a].y - my) * ep);
        int x1 = mx + (int)((iproj[b].x - mx) * ep);
        int y1 = my + (int)((iproj[b].y - my) * ep);

        float depth = 0.5f - isorted[i].z / 4.0f;
        if (depth < 0.0f) depth = 0.0f;
        if (depth > 1.0f) depth = 1.0f;

        draw_glow_line(x0, y0, x1, y1,
                        scale_color(COL_LIGHT_BLUE, brightness * 0.7f), depth);
    }

    // Connecting lines between inner and outer vertices (energy conduits)
    if (edge_progress > 0.5f) {
        float cp = (edge_progress - 0.5f) / 0.5f;
        uint16_t conduit_col = scale_color(COL_CYAN, brightness * 0.15f * cp);
        for (int i = 0; i < 6; i++)
            draw_line(proj[i].x, proj[i].y, iproj[i].x, iproj[i].y, conduit_col);
    }

    // Vertex dots on outer crystal
    if (edge_progress > 0.1f) {
        int dot_r = (_crystal_r > 60) ? 3 : 2;
        for (int i = 0; i < 6; i++) {
            float vdepth = 0.5f - rot[i].z / 3.0f;
            if (vdepth < 0.2f) vdepth = 0.2f;
            if (vdepth > 1.0f) vdepth = 1.0f;
            draw_dot_glow(proj[i].x, proj[i].y,
                          scale_color(COL_WHITE, brightness * vdepth), dot_r);
        }
    }
}

// Render orbiting particles
static void draw_particles(float angle_y, float angle_x, float brightness) {
    for (int i = 0; i < NUM_PARTICLES; i++) {
        Particle& p = _particles[i];
        float a = p.angle;
        float bob = sinf(a * 2.0f + p.phase) * 0.3f;
        Vec3 pos = {p.radius * cosf(a), p.height + bob, p.radius * sinf(a)};
        // Apply same camera rotation as crystal for consistency
        pos = rotX(pos, angle_x * 0.3f);
        Vec2 sp = project(pos, _crystal_cx, _crystal_cy, (float)_crystal_r);

        // Particle color: alternate cyan/blue/white
        uint16_t col;
        if (i % 3 == 0) col = COL_CYAN;
        else if (i % 3 == 1) col = COL_LIGHT_BLUE;
        else col = COL_WHITE;

        float depth = 0.5f - pos.z / 5.0f;
        if (depth < 0.15f) depth = 0.15f;
        if (depth > 1.0f) depth = 1.0f;

        draw_dot_glow(sp.x, sp.y, scale_color(col, brightness * depth),
                      (_crystal_r > 60) ? 2 : 1);

        // Trail: draw dimmer dot at previous angle
        float prev_a = a - p.speed * 0.04f;
        float prev_bob = sinf(prev_a * 2.0f + p.phase) * 0.3f;
        Vec3 prev = {p.radius * cosf(prev_a), p.height + prev_bob,
                     p.radius * sinf(prev_a)};
        prev = rotX(prev, angle_x * 0.3f);
        Vec2 tp = project(prev, _crystal_cx, _crystal_cy, (float)_crystal_r);
        px(tp.x, tp.y, scale_color(col, brightness * depth * 0.3f));
    }
}

static void advance_particles(float dt) {
    for (int i = 0; i < NUM_PARTICLES; i++)
        _particles[i].angle += _particles[i].speed * dt;
}

// ============================================================================
// Public API
// ============================================================================

namespace boot_sequence {

void init(esp_lcd_panel_handle_t panel, int width, int height) {
    _panel = panel;
    _w = width;
    _h = height;

    // RGB parallel panels (43C-BOX) use native byte order;
    // QSPI/SPI panels need byte-swapped RGB565
#if defined(BOARD_TOUCH_LCD_43C_BOX)
    _swap_bytes = false;
#else
    _swap_bytes = true;
#endif

    // Scale for service log text
    if (_w >= 600)      { _scale = 2; _title_scale = 4; }
    else if (_w >= 320) { _scale = 2; _title_scale = 3; }
    else                { _scale = 1; _title_scale = 2; }

    _line_height = 7 * _scale + 3;
    _margin_x = 4 * _scale;

    // Crystal layout
    _crystal_r = ((_w < _h) ? _w : _h) / 7;
    if (_crystal_r < 25) _crystal_r = 25;
    if (_crystal_r > 90) _crystal_r = 90;
    _crystal_cx = _w / 2;
    _crystal_cy = _crystal_r + 20;
    _crystal_bottom = _crystal_cy + _crystal_r + 10;

    // Palette
    // Cool blue/cyan palette — no warm tones
    COL_CYAN       = rgb565(0x00, 0xF0, 0xFF);  // primary accent
    COL_GREEN      = rgb565(0x00, 0xDD, 0xCC);  // OK status — blue-teal
    COL_ALERT      = rgb565(0xCC, 0x44, 0xFF);  // FAIL — cool violet
    COL_WARN       = rgb565(0x60, 0x90, 0xCC);  // SKIP — steel blue
    COL_DIM_CYAN   = rgb565(0x00, 0x70, 0x80);  // muted labels
    COL_WHITE      = rgb565(0xD0, 0xE0, 0xFF);  // text — blue-white
    COL_DARK_CYAN  = rgb565(0x00, 0x30, 0x40);  // separators
    COL_LIGHT_BLUE = rgb565(0x40, 0x80, 0xFF);  // inner crystal

    // Allocate buffers
    size_t fb_size = _w * _h * sizeof(uint16_t);
    _fb = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (!_fb) { Serial.printf("[boot] PSRAM alloc failed\n"); return; }
    memset(_fb, 0, fb_size);

    size_t dma_size = _w * CHUNK * sizeof(uint16_t);
    _dma = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA);
    if (!_dma) {
        heap_caps_free(_fb); _fb = nullptr;
        Serial.printf("[boot] DMA alloc failed\n");
        return;
    }

    _rng = millis() ^ 0xDEADBEEF;
    init_stars();
    init_particles();

    Serial.printf("[boot] Crystal boot initialized %dx%d r=%d\n", _w, _h, _crystal_r);
}

void showLogo(const char* version) {
    if (!_fb) return;

    const int TOTAL_FRAMES = 70;
    const float dt = 0.035f;  // ~30fps time step

    // Title sits well below crystal with breathing room
    int title_gap = 7 * _title_scale;  // one full title-height of gap
    int title_y = _crystal_bottom + title_gap;

    const char* title = "TRITIUM-OS";
    int title_w = measure(title, _title_scale);
    int title_x = (_w - title_w) / 2;

    char ver_str[32];
    snprintf(ver_str, sizeof(ver_str), "[ v%s ]", version);
    int ver_w = measure(ver_str, _scale);
    int ver_x = (_w - ver_w) / 2;
    int ver_y = title_y + 7 * _title_scale + 6;

    const char* author1 = "Created by Matthew Valancy";
    int author1_w = measure(author1, 1);
    int author1_x = (_w - author1_w) / 2;
    int author1_y = _h - 7 - 4;  // bottom line

    const char* author2 = "Copyright 2026, Valpatel Software LLC";
    int author2_w = measure(author2, 1);
    int author2_x = (_w - author2_w) / 2;
    int author2_y = author1_y - 7 - 3;  // line above

    // Top of the text zone — crystal animation stays above this line
    int text_zone_top = _crystal_bottom + title_gap / 2;

    // Separator and service log start position
    int sep_y = ver_y + 7 * _scale + 10;

    _crystal_angle = 0.0f;
    _crystal_clip_y = text_zone_top;  // Clip crystal rendering above title

    for (int frame = 0; frame < TOTAL_FRAMES; frame++) {
        uint32_t t0 = millis();

        // Only clear the crystal animation region (above text zone)
        clear_region(0, text_zone_top);

        // --- Background stars (dim, static) ---
        draw_stars();

        // --- Crystal animation ---
        float edge_prog;
        if (frame < 25) {
            edge_prog = (float)frame / 25.0f;
        } else {
            edge_prog = 1.0f;
        }

        float wobble_x = sinf(_crystal_angle * 0.4f) * 0.15f;
        draw_crystal(_crystal_angle, wobble_x, edge_prog, 1.0f);

        // Particles appear after frame 15
        if (frame > 15) {
            float pfade = (frame < 25) ? (float)(frame - 15) / 10.0f : 1.0f;
            draw_particles(_crystal_angle, wobble_x, pfade);
        }
        advance_particles(dt);

        // --- Expanding pulse ring at frame 30 ---
        if (frame >= 28 && frame < 42) {
            int ring_r = (frame - 28) * (_crystal_r / 6);
            float ring_fade = 1.0f - (float)(frame - 28) / 14.0f;
            uint16_t ring_col = scale_color(COL_CYAN, ring_fade * 0.3f);
            for (int a = 0; a < 360; a += 2) {
                float rad = a * 3.14159f / 180.0f;
                int rx = _crystal_cx + (int)(ring_r * cosf(rad));
                int ry = _crystal_cy + (int)(ring_r * 0.6f * sinf(rad));
                px(rx, ry, ring_col);
            }
        }

        // --- Author text at bottom (appears early, subtle) ---
        if (frame > 3 && frame <= 15) {
            float af = (float)(frame - 3) / 12.0f;
            clear_region(author2_y, _h);
            draw_string(author2_x, author2_y, author2,
                        scale_color(COL_DIM_CYAN, af * 0.5f), 1);
            draw_string(author1_x, author1_y, author1,
                        scale_color(COL_DIM_CYAN, af * 0.5f), 1);
            push_region(author2_y, _h);
        }

        // Push crystal region only — fast, no flicker in text area
        push_region(0, text_zone_top);

        // --- Title text (drawn once into text zone, not cleared each frame) ---
        if (frame == 25) {
            // Decorative line between crystal and title
            draw_hline(_margin_x * 4, text_zone_top, _w - 8 * _margin_x,
                       COL_DARK_CYAN, 1);
            push_region(text_zone_top, text_zone_top + 2);
        }
        if (frame >= 25 && frame <= 40) {
            float tf = (float)(frame - 25) / 15.0f;
            if (tf > 1.0f) tf = 1.0f;
            // Clear and redraw title at increasing brightness
            clear_region(title_y, ver_y + 7 * _scale + 2);
            draw_string(title_x, title_y, title,
                        scale_color(COL_WHITE, tf), _title_scale);
            push_region(title_y, ver_y + 7 * _scale + 2);
        }

        // --- Version text (fades in later) ---
        if (frame >= 35 && frame <= 50) {
            float vf = (float)(frame - 35) / 15.0f;
            if (vf > 1.0f) vf = 1.0f;
            clear_region(ver_y, ver_y + 7 * _scale + 2);
            draw_string(ver_x, ver_y, ver_str,
                        scale_color(COL_DIM_CYAN, vf), _scale);
            push_region(ver_y, ver_y + 7 * _scale + 2);
        }

        // --- Decorative separator line (draws in from center) ---
        if (frame > 45) {
            float sp = (frame < 60) ? (float)(frame - 45) / 15.0f : 1.0f;
            int half = (int)((_w / 2 - _margin_x) * sp);
            draw_hline(_w/2 - half, sep_y, half * 2, COL_DARK_CYAN, 1);
            push_region(sep_y, sep_y + 2);
        }

        _crystal_angle += 0.045f;

        uint32_t elapsed = millis() - t0;
        if (elapsed < 33) delay(33 - elapsed);
    }

    // Final state: draw everything clean one last time
    draw_hline(_margin_x, sep_y, _w - 2 * _margin_x, COL_DARK_CYAN, 1);
    draw_string(title_x, title_y, title, COL_WHITE, _title_scale);
    draw_string(ver_x, ver_y, ver_str, COL_DIM_CYAN, _scale);
    draw_string(author2_x, author2_y, author2, scale_color(COL_DIM_CYAN, 0.5f), 1);
    draw_string(author1_x, author1_y, author1, scale_color(COL_DIM_CYAN, 0.5f), 1);
    _line_y = sep_y + 8;
    push_full();
}

void showService(const char* name, const char* status, const char* detail) {
    if (!_fb) return;
    if (_line_y + _line_height > _h) return;

    // Crystal stays static (frozen at final showLogo angle) — no flicker.

    // Draw the service line
    int x = _margin_x;
    int y = _line_y;

    x += draw_string(x, y, "[ ", COL_CYAN, _scale);

    char padded[12];
    snprintf(padded, sizeof(padded), "%-10s", name);
    x += draw_string(x, y, padded, COL_DIM_CYAN, _scale);

    x += draw_string(x, y, "] ", COL_CYAN, _scale);

    uint16_t scol = COL_CYAN;
    const char* stxt = status;
    if (strcasecmp(status, "ok") == 0)   { scol = COL_GREEN;   stxt = "OK"; }
    else if (strcasecmp(status, "fail") == 0) { scol = COL_ALERT; stxt = "FAIL"; }
    else if (strcasecmp(status, "skip") == 0) { scol = COL_WARN;  stxt = "SKIP"; }

    if (detail) {
        x += draw_string(x, y, detail, COL_WHITE, _scale);
    } else {
        x += draw_string(x, y, stxt, scol, _scale);
    }

    _line_y += _line_height;

    // Push just the service line region — clean, no flicker
    push_region(y, y + _line_height + 2);
    delay(80);
}

void showReady() {
    if (!_fb) return;

    // Blank line + separator
    _line_y += _line_height / 2;
    draw_hline(_margin_x, _line_y, _w - 2 * _margin_x, COL_DARK_CYAN, 1);
    _line_y += 6;

    // ">> SYSTEM READY <<" centered in green
    const char* bl = ">>";
    const char* msg = " SYSTEM READY ";
    const char* br = "<<";
    int fw = measure(bl, _scale) + measure(msg, _scale) + measure(br, _scale);
    int x = (_w - fw) / 2;
    x += draw_string(x, _line_y, bl, COL_CYAN, _scale);
    x += draw_string(x, _line_y, msg, COL_GREEN, _scale);
    draw_string(x, _line_y, br, COL_CYAN, _scale);

    push_full();

    // Hold long enough to read the service log and ready status
    delay(2000);
}

void finish() {
    if (_dma) { heap_caps_free(_dma); _dma = nullptr; }
    if (_fb)  { heap_caps_free(_fb);  _fb = nullptr; }
    _panel = nullptr;
    Serial.printf("[boot] Boot sequence complete\n");
}

} // namespace boot_sequence

#endif // ENABLE_SETTINGS || ENABLE_DIAG
