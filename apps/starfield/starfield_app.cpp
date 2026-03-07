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
static constexpr uint32_t BAT_UPDATE_MS = 5000;

uint16_t StarfieldApp::tintedColor(float b, StarTint tint) {
    (void)tint;
    uint8_t v = (uint8_t)(b * 255.0f);
    return ((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3);
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

void StarfieldApp::setup(LGFX& display) {
    DBG_INFO("starfield", "App: %s", name());

    int w = display.width();
    int h = display.height();

    _canvas = new LGFX_Sprite(&display);
    _canvas->setPsram(true);
    _canvas->setColorDepth(16);

    // Half-res scaling only for RGB parallel panels (line-buffer push to DMA framebuffer)
    // QSPI/SPI panels need full-res pushSprite — scaled pushImage causes blank screen
#if defined(BOARD_TOUCH_LCD_43C_BOX)
    bool large = true;
#else
    bool large = false;
#endif
    int sw = large ? w / 2 : w;
    int sh = large ? h / 2 : h;
    _render_scale = large ? 2 : 1;

    if (!_canvas->createSprite(sw, sh)) {
        DBG_WARN("starfield", "Sprite %dx%d failed, trying half-height", sw, sh);
        if (!_canvas->createSprite(sw, sh / 2)) {
            DBG_ERROR("starfield", "Sprite allocation failed!");
            delete _canvas;
            _canvas = nullptr;
        }
    }

    if (_canvas) {
        DBG_INFO("starfield", "Sprite: %dx%d (scale %dx)", _canvas->width(), _canvas->height(), _render_scale);
    }

    _starfield = new StarField(_canvas ? _canvas->width() : w, _canvas ? _canvas->height() : h);
    DBG_INFO("starfield", "%d stars", _starfield->getStarCount());

    // Init battery monitor
#ifndef SIMULATOR
    _power_ok = _power.initLgfx(0);
#endif
    if (_power_ok) {
        BatteryConfig batCfg;
        batCfg.chemistry = BatteryChemistry::LI_ION;
        batCfg.capacity_mah = 2600.0f;
        batCfg.avg_draw_ma = 150.0f;
        _battery.init(&_power, batCfg);
        _battery.update();

        BatteryWidgetConfig wCfg;
        wCfg.style = BatteryWidgetStyle::ICON_PERCENT_VOLT;
        wCfg.anchor = BatteryWidgetAnchor::TOP_CENTER;
        wCfg.icon_width = 44;
        wCfg.icon_height = 20;
        wCfg.text_size = 2;
        wCfg.margin = 8;
        _battWidget.init(wCfg);

        DBG_INFO("starfield", "Battery: %d%% %.2fV",
                 _battery.getStatus().soc, _battery.getStatus().voltage);
    }

    // Init IMU for motion detection
#ifndef SIMULATOR
    _imu_ok = _imu.initLgfx(0);
#endif
    if (_imu_ok) {
        DBG_INFO("starfield", "IMU ready (WHO_AM_I: 0x%02X)", _imu.whoAmI());
    }

    // Show splash screen
    _start_time = millis();
    _splash_done = false;
    drawSplash(display);

    _warp_timer = _start_time + SPLASH_DURATION_MS;
    _fps_timer = _start_time;
    _bat_timer = _start_time;
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

    // Check for touch or significant motion to show UI
    // Skip IMU for first second after splash to avoid transient triggers
    lgfx::touch_point_t tp;
    bool touch_down = (display.getTouch(&tp) > 0);
    bool motion = false;
    if (_imu_ok && (now - _start_time > SPLASH_DURATION_MS + 1000)) {
        motion = _imu.detectMotion(60.0f);
    }

    if (touch_down || motion) {
        _ui_visible = true;
        _ui_show_time = now;
    }
    if (_ui_visible && (now - _ui_show_time >= UI_TIMEOUT_MS)) {
        _ui_visible = false;
    }

    // Warp while touching
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
    if (_canvas) {
        _canvas->fillScreen(TFT_BLACK);

        const Star* stars = _starfield->getStars();
        int count = _starfield->getStarCount();

        for (int i = 0; i < count; i++) {
            int sx, sy;
            float brightness;
            if (_starfield->project(stars[i], sx, sy, brightness)) {
                // Draw trail FIRST so star draws on top
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

                uint16_t color = tintedColor(brightness, stars[i].tint);
                if (brightness > 0.7f) {
                    _canvas->fillCircle(sx, sy, 2, color);
                } else if (brightness > 0.35f) {
                    _canvas->fillCircle(sx, sy, 1, color);
                } else {
                    _canvas->drawPixel(sx, sy, color);
                }
            }
        }

        // Battery widget — only visible on touch/motion
        if (_ui_visible && _power_ok) {
            if (now - _bat_timer >= BAT_UPDATE_MS) {
                _battery.update();
                _bat_timer = now;
            }
            _battWidget.draw(*_canvas, _battery.getStatus());
        }

        uint32_t t_push_start = millis();
        if (_render_scale > 1) {
            // Scale 2x: double each pixel horizontally and each row vertically
            int sw = _canvas->width();
            int sh = _canvas->height();
            int dw = sw * 2;
            const uint16_t* buf = (const uint16_t*)_canvas->getBuffer();
            // Reuse a line buffer for horizontal doubling
            if (!_line_buf) _line_buf = (uint16_t*)heap_caps_malloc(dw * sizeof(uint16_t), MALLOC_CAP_DMA);
            if (_line_buf) {
                display.startWrite();
                for (int y = 0; y < sh; y++) {
                    const uint16_t* src = buf + y * sw;
                    for (int x = 0; x < sw; x++) {
                        _line_buf[x * 2] = src[x];
                        _line_buf[x * 2 + 1] = src[x];
                    }
                    int dy = y * 2;
                    display.pushImage(0, dy, dw, 1, _line_buf);
                    display.pushImage(0, dy + 1, dw, 1, _line_buf);
                }
                display.endWrite();
            }
        } else {
            _canvas->pushSprite(0, 0);
        }
        uint32_t t_push_end = millis();

        _render_ms = t_push_start - t_render_start;
        _push_ms = t_push_end - t_push_start;

    } else {
        // Direct rendering — writes to display's DMA framebuffer (RGB panels)
        display.startWrite();
        display.fillScreen(TFT_BLACK);

        const Star* stars = _starfield->getStars();
        int count = _starfield->getStarCount();

        for (int i = 0; i < count; i++) {
            int sx, sy;
            float brightness;
            if (_starfield->project(stars[i], sx, sy, brightness)) {
                uint16_t color = tintedColor(brightness, stars[i].tint);

                if (brightness > 0.7f) {
                    display.fillCircle(sx, sy, 2, color);
                } else if (brightness > 0.35f) {
                    display.fillCircle(sx, sy, 1, color);
                } else {
                    display.drawPixel(sx, sy, color);
                }

                // Trails
                float trail_threshold = _warping ? 0.2f : 0.5f;
                if (brightness > trail_threshold && stars[i].prev_z > stars[i].z) {
                    int tx, ty;
                    float tb;
                    Star trail = stars[i];
                    trail.z = stars[i].prev_z;
                    if (_starfield->project(trail, tx, ty, tb)) {
                        uint16_t tail_color = tintedColor(brightness * 0.3f, stars[i].tint);
                        display.drawLine(sx, sy, tx, ty, tail_color);
                    }
                }
            }
        }

        // Battery widget — draw directly on display
        if (_ui_visible && _power_ok) {
            if (now - _bat_timer >= BAT_UPDATE_MS) {
                _battery.update();
                _bat_timer = now;
            }
            // Widget needs a sprite — use a small temporary one
            if (!_overlay) {
                _overlay = new LGFX_Sprite(&display);
                _overlay->setPsram(true);
                _overlay->setColorDepth(16);
                _overlay->createSprite(160, 50);
            }
            if (_overlay) {
                _overlay->fillScreen(TFT_BLACK);
                _battWidget.draw(*_overlay, _battery.getStatus());
                int ox = (display.width() - _overlay->width()) / 2;
                _overlay->pushSprite(ox, 8, TFT_BLACK);
            }
        }

        display.endWrite();
        _render_ms = millis() - t_render_start;
        _push_ms = 0;
    }

    // FPS counter (serial only)
    _frame_count++;
    if (now - _fps_timer >= FPS_UPDATE_MS) {
        _fps = _frame_count * 1000.0f / (now - _fps_timer);
        DBG_DEBUG("perf", "FPS:%.1f render:%lums push:%lums %s",
                  _fps, _render_ms, _push_ms, _warping ? "WARP" : "cruise");
        _frame_count = 0;
        _fps_timer = now;
    }
}
