#include "starfield_app.h"

#ifdef SIMULATOR
#include "sim_hal.h"
#else
#include <Arduino.h>
#endif
#include "debug_log.h"

static constexpr float CRUISE_SPEED = 0.012f;
static constexpr float WARP_SPEED = 0.08f;
static constexpr uint32_t CRUISE_DURATION_MS = 10000;
static constexpr uint32_t WARP_DURATION_MS = 3000;
static constexpr uint32_t SPLASH_DURATION_MS = 2000;
static constexpr uint32_t FPS_UPDATE_MS = 1000;

uint16_t StarfieldApp::tintedColor(float b, StarTint tint) {
    float r, g, bl;
    switch (tint) {
        case TINT_BLUE:
            r = b * 0.6f;
            g = b * 0.7f;
            bl = b * 1.0f;
            break;
        case TINT_YELLOW:
            r = b * 1.0f;
            g = b * 0.9f;
            bl = b * 0.4f;
            break;
        case TINT_RED:
            r = b * 1.0f;
            g = b * 0.4f;
            bl = b * 0.3f;
            break;
        default: // TINT_WHITE - slight blue-white
            r = b * 0.86f;
            g = b * 0.90f;
            bl = b * 1.0f;
            break;
    }
    uint8_t ri = (uint8_t)(r * 255.0f);
    uint8_t gi = (uint8_t)(g * 255.0f);
    uint8_t bi = (uint8_t)(bl * 255.0f);
    return ((ri >> 3) << 11) | ((gi >> 2) << 5) | (bi >> 3);
}

float StarfieldApp::currentSpeed() const {
    uint32_t elapsed = millis() - _warp_timer;
    float t = (float)elapsed / 500.0f;  // 500ms ramp time
    if (t > 1.0f) t = 1.0f;

    // Ease in (sine curve, first quarter)
    float ease = sinf(t * 1.5708f);  // pi/2

    if (_warping) {
        // Ramp up toward warp speed
        return CRUISE_SPEED + (WARP_SPEED - CRUISE_SPEED) * ease;
    } else {
        // Ramp down toward cruise speed
        return WARP_SPEED - (WARP_SPEED - CRUISE_SPEED) * ease;
    }
}

void StarfieldApp::drawSplash(LGFX& display) {
    int w = display.width();
    int h = display.height();

    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    // Board name - large
    display.setTextSize(3);
    display.setTextDatum(middle_center);
    display.drawString(DISPLAY_DRIVER, w / 2, h / 2 - 20);

    // Resolution - smaller
    display.setTextSize(1);
    char buf[32];
    snprintf(buf, sizeof(buf), "%dx%d %s", DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_IF);
    display.drawString(buf, w / 2, h / 2 + 20);

    // App name
    display.drawString("Starfield", w / 2, h / 2 + 40);
}

void StarfieldApp::drawFpsOverlay() {
    if (!_canvas || _fps <= 0.0f) return;

    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f fps", _fps);

    _canvas->setTextSize(1);
    _canvas->setTextDatum(top_right);
    _canvas->setTextColor(0x7BEF); // dim gray
    _canvas->drawString(buf, _canvas->width() - 4, 4);
}

void StarfieldApp::setup(LGFX& display) {
    DBG_INFO("starfield", "App: %s", name());

    int w = display.width();
    int h = display.height();

    _canvas = new LGFX_Sprite(&display);
    _canvas->setPsram(true);
    _canvas->setColorDepth(16);

    if (!_canvas->createSprite(w, h)) {
        DBG_WARN("starfield", "Full sprite failed, trying half-height");
        if (!_canvas->createSprite(w, h / 2)) {
            DBG_ERROR("starfield", "Sprite allocation failed!");
            delete _canvas;
            _canvas = nullptr;
        }
    }

    if (_canvas) {
        DBG_INFO("starfield", "Sprite: %dx%d in PSRAM", _canvas->width(), _canvas->height());
    }

    _starfield = new StarField(w, h);
    DBG_INFO("starfield", "%d stars", _starfield->getStarCount());

    // Show splash screen
    _start_time = millis();
    _splash_done = false;
    drawSplash(display);

    _warp_timer = _start_time + SPLASH_DURATION_MS;
    _fps_timer = _start_time;
}

void StarfieldApp::loop(LGFX& display) {
    uint32_t now = millis();

    // Splash screen phase
    if (!_splash_done) {
        if (now - _start_time < SPLASH_DURATION_MS) {
            return; // Hold splash
        }
        _splash_done = true;
        _warp_timer = now;
        _fps_timer = now;
        _frame_count = 0;
    }

    // Warp while touching, cruise when released
    lgfx::touch_point_t tp;
    bool touch_down = (display.getTouch(&tp) > 0);
    if (touch_down && !_warping) {
        _warping = true;
        _warp_timer = now;
    } else if (!touch_down && _warping) {
        _warping = false;
        _warp_timer = now;
    }

    float speed = currentSpeed();
    _starfield->update(speed);

    uint32_t t_render_start = millis();

    int h = display.height();
    if (_canvas && _canvas->height() == h) {
        _canvas->fillScreen(TFT_BLACK);

        const Star* stars = _starfield->getStars();
        int count = _starfield->getStarCount();

        for (int i = 0; i < count; i++) {
            int sx, sy;
            float brightness;
            if (_starfield->project(stars[i], sx, sy, brightness)) {
                uint16_t color = tintedColor(brightness, stars[i].tint);

                if (brightness > 0.7f) {
                    _canvas->fillCircle(sx, sy, 2, color);
                } else if (brightness > 0.35f) {
                    _canvas->fillCircle(sx, sy, 1, color);
                } else {
                    _canvas->drawPixel(sx, sy, color);
                }

                // Trails - more prominent during warp
                float trail_threshold = _warping ? 0.2f : 0.5f;
                if (brightness > trail_threshold && stars[i].prev_z > stars[i].z) {
                    int tx, ty;
                    float tb;
                    Star trail = stars[i];
                    trail.z = stars[i].prev_z;
                    if (_starfield->project(trail, tx, ty, tb)) {
                        uint16_t tail_color = tintedColor(brightness * 0.3f, stars[i].tint);
                        _canvas->drawLine(sx, sy, tx, ty, tail_color);
                    }
                }
            }
        }

        drawFpsOverlay();

        uint32_t t_push_start = millis();
        _canvas->pushSprite(0, 0);
        uint32_t t_push_end = millis();

        _render_ms = t_push_start - t_render_start;
        _push_ms = t_push_end - t_push_start;

    } else {
        // Direct rendering fallback (no sprite / sprite too small)
        display.startWrite();
        display.fillScreen(TFT_BLACK);

        const Star* stars = _starfield->getStars();
        int count = _starfield->getStarCount();

        for (int i = 0; i < count; i++) {
            int sx, sy;
            float brightness;
            if (_starfield->project(stars[i], sx, sy, brightness)) {
                uint16_t color = tintedColor(brightness, stars[i].tint);
                if (brightness > 0.5f) {
                    display.fillCircle(sx, sy, 1, color);
                } else {
                    display.drawPixel(sx, sy, color);
                }
            }
        }

        display.endWrite();
    }

    // FPS counter
    _frame_count++;
    if (now - _fps_timer >= FPS_UPDATE_MS) {
        _fps = _frame_count * 1000.0f / (now - _fps_timer);
        DBG_DEBUG("perf", "FPS:%.1f render:%lums push:%lums %s",
                  _fps, _render_ms, _push_ms, _warping ? "WARP" : "cruise");
        _frame_count = 0;
        _fps_timer = now;
    }
}
