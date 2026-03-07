#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>

// ============================================================================
// Color utilities — RGB565 for LovyanGFX / LGFX_Sprite buffers
// ============================================================================
namespace gfx {

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

inline void rgb565_unpack(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = (c >> 11) << 3;
    g = ((c >> 5) & 0x3F) << 2;
    b = (c & 0x1F) << 3;
}

inline uint16_t hsv565(float h, float s, float v) {
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r, g, b;
    if      (h < 60)  { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    return rgb565((uint8_t)((r + m) * 255), (uint8_t)((g + m) * 255), (uint8_t)((b + m) * 255));
}

inline uint16_t blend565(uint16_t a, uint16_t b, uint8_t t) {
    uint8_t ar, ag, ab, br, bg, bb;
    rgb565_unpack(a, ar, ag, ab);
    rgb565_unpack(b, br, bg, bb);
    uint8_t inv = 255 - t;
    return rgb565(
        (ar * inv + br * t) >> 8,
        (ag * inv + bg * t) >> 8,
        (ab * inv + bb * t) >> 8
    );
}

// Neon theme colors
inline constexpr uint16_t NEON_CYAN    = ((0 >> 3) << 11) | ((255 >> 2) << 5) | (208 >> 3);   // #00FFD0
inline constexpr uint16_t NEON_MAGENTA = ((255 >> 3) << 11) | ((0 >> 2) << 5) | (128 >> 3);   // #FF0080
inline constexpr uint16_t NEON_GREEN   = ((0 >> 3) << 11) | ((255 >> 2) << 5) | (65 >> 3);    // #00FF41
inline constexpr uint16_t NEON_PURPLE  = ((148 >> 3) << 11) | ((0 >> 2) << 5) | (211 >> 3);   // #9400D3
inline constexpr uint16_t NEON_ORANGE  = ((255 >> 3) << 11) | ((102 >> 2) << 5) | (0 >> 3);   // #FF6600
inline constexpr uint16_t DEEP_BLACK   = 0x0000;

// Scale a neon color by intensity (0.0 - 1.0)
inline uint16_t neon_scale(uint16_t color, float intensity) {
    uint8_t r, g, b;
    rgb565_unpack(color, r, g, b);
    return rgb565((uint8_t)(r * intensity), (uint8_t)(g * intensity), (uint8_t)(b * intensity));
}

} // namespace gfx

// ============================================================================
// Base effect interface
// ============================================================================
class GfxEffect {
public:
    virtual ~GfxEffect() = default;
    virtual const char* name() const = 0;
    virtual void init(int w, int h) = 0;
    virtual void update(float dt) = 0;
    virtual void render(uint16_t* buf, int w, int h) = 0;
};

// ============================================================================
// Plasma — dark neon sinusoidal waves (cyan/magenta/purple)
// ============================================================================
class PlasmaEffect : public GfxEffect {
    float _time = 0;
    uint16_t _palette[256];

public:
    const char* name() const override { return "Plasma"; }

    void init(int w, int h) override {
        _time = 0;
        for (int i = 0; i < 256; i++) {
            float t = i / 256.0f;
            // Dark neon: mostly black with cyan and magenta peaks
            float cyan   = powf(fmaxf(sinf(t * 6.2832f), 0.0f), 3.0f);
            float magenta = powf(fmaxf(sinf(t * 6.2832f + 2.5f), 0.0f), 3.0f);
            float purple  = powf(fmaxf(sinf(t * 6.2832f + 4.5f), 0.0f), 4.0f);
            float r = magenta * 0.9f + purple * 0.58f;
            float g = cyan * 0.9f;
            float b = cyan * 0.82f + magenta * 0.5f + purple * 0.83f;
            if (r > 1.0f) r = 1.0f;
            if (b > 1.0f) b = 1.0f;
            _palette[i] = gfx::rgb565(r * 255, g * 255, b * 255);
        }
    }

    void update(float dt) override { _time += dt; }

    void render(uint16_t* buf, int w, int h) override {
        float t = _time * 1.5f;
        float inv_w = 1.0f / w;
        float inv_h = 1.0f / h;
        for (int y = 0; y < h; y++) {
            float fy = y * inv_h * 6.0f;
            for (int x = 0; x < w; x++) {
                float fx = x * inv_w * 6.0f;
                float v = sinf(fx * 1.3f + t);
                v += sinf(fy * 1.1f + t * 0.7f);
                v += sinf((fx + fy) * 0.7f + t * 0.5f);
                v += sinf(sqrtf(fx * fx + fy * fy + 1.0f) * 0.8f + t * 0.6f);
                int idx = (int)((v * 32.0f + 128.0f)) & 0xFF;
                buf[y * w + x] = _palette[idx];
            }
        }
    }
};

// ============================================================================
// Fire — bottom-up fire in neon cyan (blue fire / ghost fire)
// ============================================================================
class FireEffect : public GfxEffect {
    uint8_t* _heat = nullptr;
    uint16_t _palette[256];
    int _w = 0, _h = 0;

public:
    ~FireEffect() override { delete[] _heat; }
    const char* name() const override { return "Ghost Fire"; }

    void init(int w, int h) override {
        _w = w; _h = h;
        delete[] _heat;
        _heat = new uint8_t[w * h]();
        // Ghost fire palette: black → deep purple → cyan → white core
        for (int i = 0; i < 256; i++) {
            float t = i / 255.0f;
            uint8_t r, g, b;
            if (t < 0.25f) {
                float s = t / 0.25f;
                r = s * 30;  g = 0;  b = s * 60;       // black → dark purple
            } else if (t < 0.5f) {
                float s = (t - 0.25f) / 0.25f;
                r = 30 - s * 30;  g = s * 80;  b = 60 + s * 100;  // purple → teal
            } else if (t < 0.75f) {
                float s = (t - 0.5f) / 0.25f;
                r = 0;  g = 80 + s * 175;  b = 160 + s * 60;  // teal → bright cyan
            } else {
                float s = (t - 0.75f) / 0.25f;
                r = s * 200;  g = 255;  b = 220 + s * 35;  // cyan → white
            }
            _palette[i] = gfx::rgb565(r, g, b);
        }
    }

    void update(float dt) override {
        if (!_heat) return;
        for (int x = 0; x < _w; x++) {
            _heat[(_h - 1) * _w + x] = (rand() % 160) + 96;
        }
        for (int y = 0; y < _h - 1; y++) {
            for (int x = 0; x < _w; x++) {
                int x1 = (x - 1 + _w) % _w;
                int x2 = (x + 1) % _w;
                int sum = _heat[(y + 1) * _w + x1]
                        + _heat[(y + 1) * _w + x]
                        + _heat[(y + 1) * _w + x]
                        + _heat[(y + 1) * _w + x2];
                int avg = sum / 4;
                int cool = rand() % 3;
                _heat[y * _w + x] = (avg > cool) ? avg - cool : 0;
            }
        }
    }

    void render(uint16_t* buf, int w, int h) override {
        if (!_heat) return;
        for (int i = 0; i < w * h; i++) {
            buf[i] = _palette[_heat[i]];
        }
    }
};

// ============================================================================
// Matrix Rain — classic green-on-black digital rain
// ============================================================================
class MatrixRainEffect : public GfxEffect {
    static constexpr int CHAR_H = 10;
    static constexpr int CHAR_W = 6;

    struct Column {
        float y;
        float speed;
        int length;
    };

    Column* _cols = nullptr;
    int _ncols = 0, _nrows = 0;
    int _w = 0, _h = 0;
    uint8_t* _chars = nullptr;

public:
    ~MatrixRainEffect() override {
        delete[] _cols;
        delete[] _chars;
    }
    const char* name() const override { return "Matrix"; }

    void init(int w, int h) override {
        _w = w; _h = h;
        _ncols = w / CHAR_W;
        _nrows = h / CHAR_H;
        delete[] _cols;
        delete[] _chars;
        _cols = new Column[_ncols];
        int char_count = _ncols * (_nrows + 30);
        _chars = new uint8_t[char_count]();
        for (int i = 0; i < _ncols; i++) resetCol(i, true);
        for (int i = 0; i < char_count; i++) _chars[i] = rand() % 96 + 33;
    }

    void update(float dt) override {
        for (int i = 0; i < _ncols; i++) {
            _cols[i].y += _cols[i].speed * dt;
            if (_cols[i].y - _cols[i].length > _nrows + 5) resetCol(i, false);
        }
        // Occasional char mutation
        int char_count = _ncols * (_nrows + 30);
        for (int i = 0; i < 3; i++) {
            _chars[rand() % char_count] = rand() % 96 + 33;
        }
    }

    void render(uint16_t* buf, int w, int h) override {
        // Fade existing: fast decay to deep black
        for (int i = 0; i < w * h; i++) {
            uint8_t r, g, b;
            gfx::rgb565_unpack(buf[i], r, g, b);
            buf[i] = gfx::rgb565(r >> 3, g > 12 ? g - 12 : 0, b >> 3);
        }

        int char_count = _ncols * (_nrows + 30);
        for (int c = 0; c < _ncols; c++) {
            int head_row = (int)_cols[c].y;
            int px = c * CHAR_W;

            for (int r = head_row - _cols[c].length; r <= head_row; r++) {
                if (r < 0 || r >= _nrows) continue;
                int py = r * CHAR_H;
                float fade = 1.0f - (float)(head_row - r) / _cols[c].length;
                if (fade < 0) fade = 0;

                uint16_t color;
                if (r == head_row) {
                    color = gfx::rgb565(180, 255, 180);  // bright white-green head
                } else {
                    uint8_t g_val = (uint8_t)(fade * fade * 220);
                    color = gfx::rgb565(0, g_val, g_val / 6);
                }
                drawGlyph(buf, w, h, px, py, _chars[(c * (_nrows + 30) + r) % char_count], color);
            }
        }
    }

private:
    void resetCol(int i, bool scatter) {
        _cols[i].y = scatter ? -(rand() % (_nrows + 15)) : -(rand() % 10);
        _cols[i].speed = 5.0f + (rand() % 15);
        _cols[i].length = 8 + rand() % 20;
    }

    void drawGlyph(uint16_t* buf, int bw, int bh, int x, int y, uint8_t ch, uint16_t color) {
        uint32_t seed = ch * 2654435761u;
        for (int dy = 1; dy < CHAR_H - 1; dy++) {
            for (int dx = 0; dx < CHAR_W - 1; dx++) {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                if ((seed & 3) == 0) {
                    int px = x + dx, py = y + dy;
                    if (px >= 0 && px < bw && py >= 0 && py < bh)
                        buf[py * bw + px] = color;
                }
            }
        }
    }
};

// ============================================================================
// Particle Bursts — neon fireworks with gravity and trails
// ============================================================================
class ParticleEffect : public GfxEffect {
    static constexpr int MAX_PARTICLES = 800;
    static constexpr int BURST_SIZE = 60;

    struct Particle {
        float x, y, vx, vy;
        float life, max_life;
        uint16_t color;
    };

    uint16_t burstColor() {
        const uint16_t colors[] = {
            gfx::NEON_CYAN, gfx::NEON_MAGENTA, gfx::NEON_GREEN,
            gfx::NEON_PURPLE, gfx::NEON_ORANGE
        };
        return colors[rand() % 5];
    }

    Particle _particles[MAX_PARTICLES];
    int _w = 0, _h = 0;
    float _timer = 0;
    float _burst_interval = 0.8f;

public:
    const char* name() const override { return "Neon Sparks"; }

    void init(int w, int h) override {
        _w = w; _h = h; _timer = 0;
        for (auto& p : _particles) p.life = 0;
        burst(w / 2, h / 3);
    }

    void update(float dt) override {
        _timer += dt;
        if (_timer > _burst_interval) {
            _timer -= _burst_interval;
            burst(50 + rand() % (_w - 100), 50 + rand() % (_h * 2 / 3));
            _burst_interval = 0.6f + (rand() % 80) / 100.0f;
        }
        for (auto& p : _particles) {
            if (p.life <= 0) continue;
            p.life -= dt / p.max_life;
            p.vy += 60.0f * dt;
            p.vx *= 0.998f;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            if (p.y > _h + 10) p.life = 0;
        }
    }

    void render(uint16_t* buf, int w, int h) override {
        // Slow fade — leaves ghostly trails
        for (int i = 0; i < w * h; i++) {
            uint8_t r, g, b;
            gfx::rgb565_unpack(buf[i], r, g, b);
            buf[i] = gfx::rgb565(
                r > 8 ? r - 8 : 0,
                g > 8 ? g - 8 : 0,
                b > 8 ? b - 8 : 0
            );
        }

        for (const auto& p : _particles) {
            if (p.life <= 0) continue;
            int px = (int)p.x, py = (int)p.y;
            if (px < 1 || px >= w - 1 || py < 1 || py >= h - 1) continue;

            float v = p.life;
            uint16_t color = gfx::neon_scale(p.color, v);

            // Core pixel
            buf[py * w + px] = color;

            // Glow halo for bright particles
            if (v > 0.4f) {
                uint16_t glow = gfx::neon_scale(p.color, v * 0.4f);
                buf[(py - 1) * w + px] = gfx::blend565(buf[(py - 1) * w + px], glow, 160);
                buf[(py + 1) * w + px] = gfx::blend565(buf[(py + 1) * w + px], glow, 160);
                buf[py * w + px - 1] = gfx::blend565(buf[py * w + px - 1], glow, 160);
                buf[py * w + px + 1] = gfx::blend565(buf[py * w + px + 1], glow, 160);
            }
        }
    }

private:
    void burst(int cx, int cy) {
        uint16_t base_color = burstColor();
        for (int i = 0; i < BURST_SIZE; i++) {
            Particle* p = findDead();
            if (!p) break;
            float angle = (rand() % 1000) / 1000.0f * 6.2832f;
            float speed = 40.0f + (rand() % 120);
            p->x = cx;
            p->y = cy;
            p->vx = cosf(angle) * speed;
            p->vy = sinf(angle) * speed - 50.0f;
            p->max_life = 1.2f + (rand() % 100) / 100.0f;
            p->life = 1.0f;
            p->color = base_color;
        }
    }

    Particle* findDead() {
        for (auto& p : _particles) {
            if (p.life <= 0) return &p;
        }
        return nullptr;
    }
};

// ============================================================================
// Tunnel — neon wireframe tunnel zoom
// ============================================================================
class TunnelEffect : public GfxEffect {
    float _time = 0;
    uint16_t _palette[256];

public:
    const char* name() const override { return "Tunnel"; }

    void init(int w, int h) override {
        _time = 0;
        // Neon gradient palette: deep purple → cyan → dark
        for (int i = 0; i < 256; i++) {
            float t = i / 255.0f;
            float r = powf(sinf(t * 3.14159f), 2.0f) * 0.6f;
            float g = powf(sinf(t * 3.14159f + 0.5f), 2.0f) * 0.9f;
            float b = powf(sinf(t * 3.14159f + 1.0f), 2.0f);
            _palette[i] = gfx::rgb565(r * 255, g * 255, b * 255);
        }
    }

    void update(float dt) override { _time += dt; }

    void render(uint16_t* buf, int w, int h) override {
        float cx = w * 0.5f;
        float cy = h * 0.5f;
        float t = _time;

        for (int y = 0; y < h; y++) {
            float dy = y - cy;
            for (int x = 0; x < w; x++) {
                float dx = x - cx;
                float dist = sqrtf(dx * dx + dy * dy) + 0.1f;
                float angle = atan2f(dy, dx);

                float tex_u = 80.0f / dist + t * 2.5f;
                float tex_v = angle * 80.0f / 3.14159f + t * 1.5f;

                // Grid-like pattern for wireframe feel
                float grid = fmodf(fabsf(tex_u), 1.0f) * fmodf(fabsf(tex_v), 1.0f);
                int idx = ((int)(tex_u * 4.0f) ^ (int)(tex_v * 4.0f)) & 0xFF;

                float shade = 30.0f / (dist + 15.0f);
                if (shade > 1.0f) shade = 1.0f;
                // Boost edges for neon wireframe look
                float edge = (grid < 0.15f) ? 1.5f : 0.6f;
                shade *= edge;
                if (shade > 1.0f) shade = 1.0f;

                uint8_t r, g, b;
                gfx::rgb565_unpack(_palette[idx], r, g, b);
                buf[y * w + x] = gfx::rgb565(
                    (uint8_t)(r * shade),
                    (uint8_t)(g * shade),
                    (uint8_t)(b * shade)
                );
            }
        }
    }
};

// ============================================================================
// Metaballs — neon organic blobs on dark background
// ============================================================================
class MetaballsEffect : public GfxEffect {
    static constexpr int NUM_BALLS = 6;
    struct Ball { float x, y, vx, vy, radius; };
    Ball _balls[NUM_BALLS];
    int _w = 0, _h = 0;
    float _time = 0;

public:
    const char* name() const override { return "Metaballs"; }

    void init(int w, int h) override {
        _w = w; _h = h; _time = 0;
        for (auto& b : _balls) {
            b.x = rand() % w;
            b.y = rand() % h;
            b.vx = (rand() % 80 - 40);
            b.vy = (rand() % 80 - 40);
            b.radius = 30.0f + rand() % 50;
        }
    }

    void update(float dt) override {
        _time += dt;
        for (auto& b : _balls) {
            b.x += b.vx * dt;
            b.y += b.vy * dt;
            if (b.x < 0 || b.x >= _w) b.vx = -b.vx;
            if (b.y < 0 || b.y >= _h) b.vy = -b.vy;
            b.x = fmaxf(0, fminf(b.x, _w - 1));
            b.y = fmaxf(0, fminf(b.y, _h - 1));
        }
    }

    void render(uint16_t* buf, int w, int h) override {
        int step = (w * h > 200000) ? 3 : (w * h > 100000) ? 2 : 1;

        for (int y = 0; y < h; y += step) {
            for (int x = 0; x < w; x += step) {
                float sum = 0;
                for (const auto& b : _balls) {
                    float dx = x - b.x;
                    float dy = y - b.y;
                    sum += (b.radius * b.radius) / (dx * dx + dy * dy + 1.0f);
                }

                uint16_t color;
                if (sum > 1.2f) {
                    // Hot core: white-cyan
                    float v = fminf((sum - 1.2f) * 2.0f, 1.0f);
                    color = gfx::rgb565(v * 120, 200 + v * 55, 180 + v * 75);
                } else if (sum > 0.8f) {
                    // Edge: neon cyan glow
                    float v = (sum - 0.8f) / 0.4f;
                    color = gfx::rgb565(0, (uint8_t)(v * 200), (uint8_t)(v * 170));
                } else if (sum > 0.4f) {
                    // Outer glow: dim purple
                    float v = (sum - 0.4f) / 0.4f;
                    color = gfx::rgb565((uint8_t)(v * 40), 0, (uint8_t)(v * 60));
                } else {
                    color = 0;  // deep black
                }

                for (int sy = 0; sy < step && y + sy < h; sy++) {
                    for (int sx = 0; sx < step && x + sx < w; sx++) {
                        buf[(y + sy) * w + (x + sx)] = color;
                    }
                }
            }
        }
    }
};

// ============================================================================
// Scanner — horizontal neon scanline sweep
// ============================================================================
class ScannerEffect : public GfxEffect {
    float _time = 0;
    int _w = 0, _h = 0;

public:
    const char* name() const override { return "Scanner"; }
    void init(int w, int h) override { _w = w; _h = h; _time = 0; }
    void update(float dt) override { _time += dt; }

    void render(uint16_t* buf, int w, int h) override {
        // Fade existing
        for (int i = 0; i < w * h; i++) {
            uint8_t r, g, b;
            gfx::rgb565_unpack(buf[i], r, g, b);
            buf[i] = gfx::rgb565(
                r > 6 ? r - 6 : 0,
                g > 6 ? g - 6 : 0,
                b > 6 ? b - 6 : 0
            );
        }

        // Dual sweeping scanlines
        float period = 3.0f;
        float pos1 = fmodf(_time / period, 1.0f);
        float pos2 = fmodf(_time / period + 0.5f, 1.0f);
        int scanY1 = (int)(pos1 * h);
        int scanY2 = (int)(pos2 * h);

        auto drawScanline = [&](int scanY, uint16_t baseColor) {
            for (int dy = -20; dy <= 20; dy++) {
                int y = scanY + dy;
                if (y < 0 || y >= h) continue;
                float intensity = 1.0f - fabsf(dy) / 20.0f;
                intensity = intensity * intensity * intensity;
                uint16_t color = gfx::neon_scale(baseColor, intensity);

                for (int x = 0; x < w; x++) {
                    // Add interference pattern
                    float noise = sinf(x * 0.05f + _time * 3.0f) * 0.3f + 0.7f;
                    uint16_t px_color = gfx::neon_scale(color, noise);
                    buf[y * w + x] = gfx::blend565(buf[y * w + x], px_color, (uint8_t)(intensity * 220));
                }
            }

            // Horizontal data bars at scan position
            for (int i = 0; i < 8; i++) {
                int bar_y = scanY + 25 + i * 4;
                if (bar_y < 0 || bar_y >= h) continue;
                int bar_w = (int)(sinf(_time * 2.0f + i) * 0.3f + 0.5f) * w;
                uint16_t bar_color = gfx::neon_scale(baseColor, 0.3f);
                for (int x = 0; x < bar_w && x < w; x++) {
                    buf[bar_y * w + x] = bar_color;
                }
            }
        };

        drawScanline(scanY1, gfx::NEON_CYAN);
        drawScanline(scanY2, gfx::NEON_MAGENTA);

        // Corner grid decoration
        uint16_t grid_color = gfx::rgb565(0, 30, 25);
        for (int y = 0; y < h; y += 20) {
            for (int x = 0; x < w; x += 20) {
                if (x < w && y < h) {
                    buf[y * w + x] = gfx::blend565(buf[y * w + x], grid_color, 128);
                }
            }
        }
    }
};
