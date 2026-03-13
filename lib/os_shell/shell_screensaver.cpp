// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

// Starfield screensaver overlay for Tritium-OS shell.
//
// On RGB panels (43C-BOX): writes star pixels directly to the display
// framebuffers — no LVGL canvas, no memset, no invalidation. This avoids
// the PSRAM bandwidth issues that starved the httpd task.
//
// On QSPI panels: falls back to a small LVGL canvas approach.
//
// A simple black LVGL overlay captures touch events for dismiss.

#include "shell_screensaver.h"
#include "shell_theme.h"
#include "StarField.h"
#include "touch_input.h"
#include "lvgl_driver.h"
#include "display.h"

#if __has_include("os_settings.h")
#include "os_settings.h"
#define SETTINGS_AVAILABLE 1
#else
#define SETTINGS_AVAILABLE 0
#endif

#ifndef SIMULATOR
#include "tritium_compat.h"
#include "esp_heap_caps.h"
#else
#include <SDL2/SDL.h>
static uint32_t millis() { return SDL_GetTicks(); }
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace shell_screensaver {

// ---------------------------------------------------------------------------
// Configuration defaults
// ---------------------------------------------------------------------------

static constexpr uint32_t DEFAULT_TIMEOUT_S = 10;
static constexpr int      RENDER_FPS        = 20;
static constexpr int      RENDER_PERIOD_MS  = 1000 / RENDER_FPS;
static constexpr int      NUM_STARS         = 250;

// Warp speed cycling defaults
static constexpr uint32_t CRUISE_DURATION_MS = 10000;
static constexpr uint32_t WARP_DURATION_MS   = 3000;

// ---------------------------------------------------------------------------
// Runtime settings (loaded from NVS)
// ---------------------------------------------------------------------------

static bool     s_cfg_reverse    = false;   // travel direction: false=forward, true=backward
static bool     s_cfg_colors     = true;    // colored star tints
static int      s_cfg_star_size  = 2;       // 1=dot only, 2+=cross pattern for bright stars
static bool     s_cfg_warp       = false;   // periodic warp speed bursts
static float    s_cfg_speed      = 0.012f;  // cruise travel speed (0.001..0.1)
static float    s_cfg_brightness = 0.8f;    // overall brightness multiplier (0.1..1.0)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static int s_screen_w = 0;
static int s_screen_h = 0;

static lv_obj_t*  s_overlay = nullptr;
static StarField* s_starfield = nullptr;

static bool     s_active = false;
static uint32_t s_timeout_ms = DEFAULT_TIMEOUT_S * 1000;
static uint32_t s_last_check_ms = 0;
static uint32_t s_last_activity_at_activate = 0;
static uint32_t s_last_render_ms = 0;

static uint32_t s_warp_timer = 0;
static bool     s_warp_mode = false;

// Direct framebuffer access (RGB panels only)
static bool      s_rgb_mode = false;
static uint16_t* s_fb0 = nullptr;
static uint16_t* s_fb1 = nullptr;

// Previous star screen positions for erasing (avoids full memset)
struct StarPos { int16_t x, y; uint8_t drawn_size; };
static StarPos* s_prev_pos = nullptr;
static int      s_prev_count = 0;

// Track if the overlay background has been rendered by LVGL
static bool s_bg_rendered = false;
static uint32_t s_bg_render_start = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void on_touch(lv_event_t* e);
static void create_overlay();
static void render_direct();
static void load_settings();

// ---------------------------------------------------------------------------
// Settings loader
// ---------------------------------------------------------------------------

static void load_settings() {
#if SETTINGS_AVAILABLE
    TritiumSettings& settings = TritiumSettings::instance();

    uint32_t timeout = settings.getInt(SettingsDomain::SCREENSAVER, "timeout_s",
                                        DEFAULT_TIMEOUT_S);
    if (timeout == 0) {
        timeout = DEFAULT_TIMEOUT_S;
        settings.setInt("screensaver", "timeout_s", (int32_t)timeout);
    }
    s_timeout_ms = timeout * 1000;

    s_cfg_reverse   = settings.getBool(SettingsDomain::SCREENSAVER, "sf_reverse", false);
    s_cfg_colors    = settings.getBool(SettingsDomain::SCREENSAVER, "sf_colors", true);
    s_cfg_star_size = settings.getInt(SettingsDomain::SCREENSAVER, "sf_star_size", 2);
    if (s_cfg_star_size < 1) s_cfg_star_size = 1;
    if (s_cfg_star_size > 4) s_cfg_star_size = 4;
    s_cfg_warp      = settings.getBool(SettingsDomain::SCREENSAVER, "sf_warp", false);

    // Speed stored as int 1..100 (thousandths), map to 0.001..0.100
    int speed_raw = settings.getInt(SettingsDomain::SCREENSAVER, "sf_speed", 12);
    if (speed_raw < 1) speed_raw = 1;
    if (speed_raw > 100) speed_raw = 100;
    s_cfg_speed = (float)speed_raw * 0.001f;

    // Brightness stored as int 10..100 (percent), map to 0.1..1.0
    int bright_raw = settings.getInt(SettingsDomain::SCREENSAVER, "sf_brightness", 80);
    if (bright_raw < 10) bright_raw = 10;
    if (bright_raw > 100) bright_raw = 100;
    s_cfg_brightness = (float)bright_raw * 0.01f;
#endif
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init(int screen_width, int screen_height) {
    s_screen_w = screen_width;
    s_screen_h = screen_height;

    s_rgb_mode = lvgl_driver::isRgb();

    if (s_rgb_mode) {
        void* fb0 = nullptr;
        void* fb1 = nullptr;
        if (display_get_rgb_framebuffers(&fb0, &fb1)) {
            s_fb0 = (uint16_t*)fb0;
            s_fb1 = (uint16_t*)fb1;
        } else {
            // Single FB fallback
            s_fb0 = (uint16_t*)lvgl_driver::getFramebuffer();
            s_fb1 = nullptr;
        }
    }

    load_settings();

    printf("[screensaver] init %dx%d, %s mode, timeout=%lus, speed=%.3f, reverse=%d, warp=%d, size=%d\n",
           s_screen_w, s_screen_h,
           s_rgb_mode ? "direct-FB" : "canvas",
           (unsigned long)(s_timeout_ms / 1000),
           s_cfg_speed, s_cfg_reverse, s_cfg_warp, s_cfg_star_size);
}

void reloadSettings() {
    load_settings();
    printf("[screensaver] settings reloaded: speed=%.3f, reverse=%d, warp=%d, size=%d, colors=%d\n",
           s_cfg_speed, s_cfg_reverse, s_cfg_warp, s_cfg_star_size, s_cfg_colors);
}

void tick() {
    uint32_t now = millis();

    // Check activation/dismissal every 500ms
    if (!s_active) {
        if (now - s_last_check_ms < 500) return;
        s_last_check_ms = now;

        if (s_timeout_ms == 0) return;

        uint32_t idle_ms = now - touch_input::lastActivityMs();
        if (idle_ms >= s_timeout_ms) {
            activate();
        }
        return;
    }

    // Active: check for dismiss
    if (now - s_last_check_ms >= 200) {
        s_last_check_ms = now;
        uint32_t last_touch = touch_input::lastActivityMs();
        if (last_touch > s_last_activity_at_activate) {
            dismiss();
            return;
        }
    }

    // Wait for LVGL to render the black overlay before writing stars
    if (!s_bg_rendered) {
        // Give LVGL 2 frames (~40ms at 60Hz) to render the black overlay
        if (now - s_bg_render_start >= 100) {
            s_bg_rendered = true;
            printf("[screensaver] background ready, starting star render\n");
        }
        return;
    }

    // Render at target FPS (called from main loop, outside LVGL mutex)
    if (now - s_last_render_ms < RENDER_PERIOD_MS) return;
    s_last_render_ms = now;

    if (s_rgb_mode && s_fb0) {
        render_direct();
    }
}

bool isActive() {
    return s_active;
}

void dismiss() {
    if (!s_active) return;
    s_active = false;
    s_bg_rendered = false;
    printf("[screensaver] dismissed\n");

    if (s_overlay) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        // Force LVGL to redraw everything underneath
        lv_obj_invalidate(lv_screen_active());
    }
}

void activate() {
    if (s_active) return;
    s_active = true;
    s_last_activity_at_activate = touch_input::lastActivityMs();
    s_warp_timer = millis();
    s_warp_mode = false;
    s_bg_rendered = false;
    s_bg_render_start = millis();
    s_last_render_ms = 0;

    // Reload settings on each activation so changes take effect immediately
    load_settings();

    printf("[screensaver] activated\n");

    if (!s_overlay) {
        create_overlay();
    }

    if (!s_starfield) {
        s_starfield = new StarField(s_screen_w, s_screen_h, NUM_STARS);
    }

    // Allocate previous position tracking
    if (!s_prev_pos) {
        s_prev_pos = new StarPos[NUM_STARS];
    }
    // Mark all previous positions as invalid
    for (int i = 0; i < NUM_STARS; i++) {
        s_prev_pos[i].x = -1;
        s_prev_pos[i].y = -1;
        s_prev_pos[i].drawn_size = 0;
    }
    s_prev_count = NUM_STARS;

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    // LVGL will render the black overlay on the next lv_timer_handler() call
}

void setTimeoutS(uint32_t seconds) {
    s_timeout_ms = seconds * 1000;
    printf("[screensaver] timeout set to %lus\n", (unsigned long)seconds);
}

// ---------------------------------------------------------------------------
// Overlay creation (just a black clickable rect — no canvas)
// ---------------------------------------------------------------------------

static void create_overlay() {
    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_overlay, s_screen_w, s_screen_h);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, on_touch, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    printf("[screensaver] overlay created\n");
}

// ---------------------------------------------------------------------------
// Direct framebuffer rendering (RGB panels only)
// ---------------------------------------------------------------------------

static inline void write_pixel(uint16_t* fb, int stride, int x, int y, uint16_t color) {
    if (x >= 0 && x < s_screen_w && y >= 0 && y < s_screen_h) {
        fb[y * stride + x] = color;
    }
}

// Erase a star's pixels from both framebuffers
static inline void erase_star(int ox, int oy, int stride, int size) {
    write_pixel(s_fb0, stride, ox, oy, 0);
    if (s_fb1) write_pixel(s_fb1, stride, ox, oy, 0);

    if (size >= 2) {
        // Cross pattern
        write_pixel(s_fb0, stride, ox - 1, oy, 0);
        write_pixel(s_fb0, stride, ox + 1, oy, 0);
        write_pixel(s_fb0, stride, ox, oy - 1, 0);
        write_pixel(s_fb0, stride, ox, oy + 1, 0);
        if (s_fb1) {
            write_pixel(s_fb1, stride, ox - 1, oy, 0);
            write_pixel(s_fb1, stride, ox + 1, oy, 0);
            write_pixel(s_fb1, stride, ox, oy - 1, 0);
            write_pixel(s_fb1, stride, ox, oy + 1, 0);
        }
    }
    if (size >= 3) {
        // Larger cross
        write_pixel(s_fb0, stride, ox - 2, oy, 0);
        write_pixel(s_fb0, stride, ox + 2, oy, 0);
        write_pixel(s_fb0, stride, ox, oy - 2, 0);
        write_pixel(s_fb0, stride, ox, oy + 2, 0);
        if (s_fb1) {
            write_pixel(s_fb1, stride, ox - 2, oy, 0);
            write_pixel(s_fb1, stride, ox + 2, oy, 0);
            write_pixel(s_fb1, stride, ox, oy - 2, 0);
            write_pixel(s_fb1, stride, ox, oy + 2, 0);
        }
    }
    if (size >= 4) {
        // 3x3 block center ring
        write_pixel(s_fb0, stride, ox - 1, oy - 1, 0);
        write_pixel(s_fb0, stride, ox + 1, oy - 1, 0);
        write_pixel(s_fb0, stride, ox - 1, oy + 1, 0);
        write_pixel(s_fb0, stride, ox + 1, oy + 1, 0);
        if (s_fb1) {
            write_pixel(s_fb1, stride, ox - 1, oy - 1, 0);
            write_pixel(s_fb1, stride, ox + 1, oy - 1, 0);
            write_pixel(s_fb1, stride, ox - 1, oy + 1, 0);
            write_pixel(s_fb1, stride, ox + 1, oy + 1, 0);
        }
    }
}

// Draw a star's pixels to both framebuffers
static inline void draw_star(int sx, int sy, int stride, uint16_t color,
                              float brightness, int size) {
    // Core pixel
    write_pixel(s_fb0, stride, sx, sy, color);
    if (s_fb1) write_pixel(s_fb1, stride, sx, sy, color);

    // Cross pattern for bright stars at size >= 2
    if (size >= 2 && brightness > 0.5f) {
        write_pixel(s_fb0, stride, sx - 1, sy, color);
        write_pixel(s_fb0, stride, sx + 1, sy, color);
        write_pixel(s_fb0, stride, sx, sy - 1, color);
        write_pixel(s_fb0, stride, sx, sy + 1, color);
        if (s_fb1) {
            write_pixel(s_fb1, stride, sx - 1, sy, color);
            write_pixel(s_fb1, stride, sx + 1, sy, color);
            write_pixel(s_fb1, stride, sx, sy - 1, color);
            write_pixel(s_fb1, stride, sx, sy + 1, color);
        }
    }

    // Larger cross for size >= 3
    if (size >= 3 && brightness > 0.4f) {
        write_pixel(s_fb0, stride, sx - 2, sy, color);
        write_pixel(s_fb0, stride, sx + 2, sy, color);
        write_pixel(s_fb0, stride, sx, sy - 2, color);
        write_pixel(s_fb0, stride, sx, sy + 2, color);
        if (s_fb1) {
            write_pixel(s_fb1, stride, sx - 2, sy, color);
            write_pixel(s_fb1, stride, sx + 2, sy, color);
            write_pixel(s_fb1, stride, sx, sy - 2, color);
            write_pixel(s_fb1, stride, sx, sy + 2, color);
        }
    }

    // 3x3 block for size >= 4
    if (size >= 4 && brightness > 0.6f) {
        write_pixel(s_fb0, stride, sx - 1, sy - 1, color);
        write_pixel(s_fb0, stride, sx + 1, sy - 1, color);
        write_pixel(s_fb0, stride, sx - 1, sy + 1, color);
        write_pixel(s_fb0, stride, sx + 1, sy + 1, color);
        if (s_fb1) {
            write_pixel(s_fb1, stride, sx - 1, sy - 1, color);
            write_pixel(s_fb1, stride, sx + 1, sy - 1, color);
            write_pixel(s_fb1, stride, sx - 1, sy + 1, color);
            write_pixel(s_fb1, stride, sx + 1, sy + 1, color);
        }
    }
}

static void render_direct() {
    if (!s_starfield || !s_fb0) return;

    int stride = s_screen_w;

    // Determine speed for this frame
    float speed = s_cfg_speed;

    if (s_cfg_warp) {
        // Warp speed cycling
        uint32_t now = millis();
        uint32_t elapsed = now - s_warp_timer;

        if (s_warp_mode) {
            speed = s_cfg_speed * 6.5f;  // warp = ~6.5x cruise speed
            if (elapsed >= WARP_DURATION_MS) {
                s_warp_mode = false;
                s_warp_timer = now;
            }
        } else {
            if (elapsed >= CRUISE_DURATION_MS) {
                s_warp_mode = true;
                s_warp_timer = now;
            }
        }
    }

    // Apply direction (negative speed = backward/receding stars)
    if (s_cfg_reverse) speed = -speed;

    s_starfield->update(speed);

    // Erase previous star positions using their actual drawn size
    for (int i = 0; i < s_prev_count; i++) {
        int ox = s_prev_pos[i].x;
        int oy = s_prev_pos[i].y;
        if (ox < 0) continue;
        erase_star(ox, oy, stride, s_prev_pos[i].drawn_size);
    }

    // Render new star positions
    const Star* stars = s_starfield->getStars();
    int count = s_starfield->getStarCount();

    for (int i = 0; i < count; i++) {
        int sx, sy;
        float brightness;
        if (!s_starfield->project(stars[i], sx, sy, brightness)) {
            s_prev_pos[i].x = -1;
            s_prev_pos[i].y = -1;
            s_prev_pos[i].drawn_size = 0;
            continue;
        }

        // Compute effective drawn size based on brightness thresholds
        int effective_size = 1;
        if (s_cfg_star_size >= 2 && brightness > 0.5f) effective_size = 2;
        if (s_cfg_star_size >= 3 && brightness > 0.4f) effective_size = 3;
        if (s_cfg_star_size >= 4 && brightness > 0.6f) effective_size = 4;

        // Save position + drawn size for next frame erasure
        s_prev_pos[i].x = (int16_t)sx;
        s_prev_pos[i].y = (int16_t)sy;
        s_prev_pos[i].drawn_size = (uint8_t)effective_size;

        uint8_t gray = (uint8_t)(brightness * s_cfg_brightness * 255.0f);
        uint8_t r = gray, g = gray, b = gray;

        if (s_cfg_colors) {
            switch (stars[i].tint) {
                case TINT_BLUE:   r = gray / 3; g = gray / 2; break;
                case TINT_YELLOW: b = gray / 3; break;
                case TINT_RED:    g = gray / 4; b = gray / 4; break;
                default: break;
            }
        }

        uint16_t color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        draw_star(sx, sy, stride, color, brightness, effective_size);
    }
}

// ---------------------------------------------------------------------------
// Touch handler
// ---------------------------------------------------------------------------

static void on_touch(lv_event_t* e) {
    (void)e;
    dismiss();
}

}  // namespace shell_screensaver
