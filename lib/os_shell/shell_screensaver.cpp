// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

// Starfield screensaver overlay for Tritium-OS shell.
//
// Uses an LVGL canvas at half resolution (400x240 for 800x480 screens).
// Stars are rendered directly to the canvas buffer via fast memset + pixel
// writes (~192KB in PSRAM). Canvas is centered on a black overlay.
// The half-resolution keeps LVGL's flush time manageable, preventing
// mutex starvation of the httpd task.

#include "shell_screensaver.h"
#include "shell_theme.h"
#include "StarField.h"
#include "touch_input.h"

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
// Configuration
// ---------------------------------------------------------------------------

static constexpr uint32_t DEFAULT_TIMEOUT_S = 10;
static constexpr int      RENDER_FPS        = 15;
static constexpr int      RENDER_PERIOD_MS  = 1000 / RENDER_FPS;
static constexpr int      NUM_STARS         = 250;

// Warp speed cycling
static constexpr uint32_t CRUISE_DURATION_MS = 10000;
static constexpr uint32_t WARP_DURATION_MS   = 3000;
static constexpr float    CRUISE_SPEED       = 0.012f;
static constexpr float    WARP_SPEED         = 0.08f;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static int s_screen_w = 0;
static int s_screen_h = 0;
static int s_canvas_w = 0;
static int s_canvas_h = 0;

static lv_obj_t*   s_overlay = nullptr;
static lv_obj_t*   s_canvas  = nullptr;
static uint8_t*    s_canvas_buf = nullptr;

static StarField*  s_starfield = nullptr;
static lv_timer_t* s_render_timer = nullptr;

static bool     s_active = false;
static uint32_t s_timeout_ms = DEFAULT_TIMEOUT_S * 1000;
static uint32_t s_last_check_ms = 0;
static uint32_t s_last_activity_at_activate = 0;

static uint32_t s_warp_timer = 0;
static bool     s_warp_mode = false;

// Direct buffer pointers
static uint16_t* s_fb = nullptr;
static int       s_stride = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void render_tick(lv_timer_t* timer);
static void on_touch(lv_event_t* e);
static void create_overlay();

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init(int screen_width, int screen_height) {
    s_screen_w = screen_width;
    s_screen_h = screen_height;

    // Use half resolution for the canvas — keeps flush time reasonable
    // while still looking good (stars are rendered at this resolution)
    s_canvas_w = screen_width / 2;
    s_canvas_h = screen_height / 2;

#if SETTINGS_AVAILABLE
    TritiumSettings& settings = TritiumSettings::instance();
    uint32_t timeout = settings.getInt(SettingsDomain::SCREENSAVER, "timeout_s",
                                        DEFAULT_TIMEOUT_S);
    s_timeout_ms = timeout * 1000;
#endif

    printf("[screensaver] init %dx%d, canvas %dx%d, timeout=%lus\n",
           s_screen_w, s_screen_h, s_canvas_w, s_canvas_h,
           (unsigned long)(s_timeout_ms / 1000));
}

void tick() {
    uint32_t now = millis();

    if (now - s_last_check_ms < 500) return;
    s_last_check_ms = now;

    if (s_active) {
        uint32_t last_touch = touch_input::lastActivityMs();
        if (last_touch > s_last_activity_at_activate) {
            dismiss();
        }
        return;
    }

    if (s_timeout_ms == 0) return;

    uint32_t idle_ms = now - touch_input::lastActivityMs();
    if (idle_ms >= s_timeout_ms) {
        activate();
    }
}

bool isActive() {
    return s_active;
}

void dismiss() {
    if (!s_active) return;
    s_active = false;
    printf("[screensaver] dismissed\n");

    if (s_render_timer) {
        lv_timer_delete(s_render_timer);
        s_render_timer = nullptr;
    }

    if (s_overlay) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void activate() {
    if (s_active) return;
    s_active = true;
    s_last_activity_at_activate = touch_input::lastActivityMs();
    s_warp_timer = millis();
    s_warp_mode = false;

    printf("[screensaver] activated\n");

    if (!s_overlay) {
        create_overlay();
    }

    if (!s_starfield) {
        s_starfield = new StarField(s_canvas_w, s_canvas_h, NUM_STARS);
    }

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);

    // Cache buffer pointers
    lv_draw_buf_t* db = lv_canvas_get_draw_buf(s_canvas);
    s_fb = (uint16_t*)db->data;
    s_stride = db->header.stride / 2;

    // Initial clear
    memset(s_fb, 0, db->header.stride * s_canvas_h);
    lv_obj_invalidate(s_canvas);

    if (!s_render_timer) {
        s_render_timer = lv_timer_create(render_tick, RENDER_PERIOD_MS, nullptr);
    }
}

void setTimeoutS(uint32_t seconds) {
    s_timeout_ms = seconds * 1000;
    printf("[screensaver] timeout set to %lus\n", (unsigned long)seconds);
}

// ---------------------------------------------------------------------------
// Overlay creation
// ---------------------------------------------------------------------------

static void create_overlay() {
    // Full-screen black background
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

    // Canvas at half resolution, centered on overlay
    size_t buf_size = LV_CANVAS_BUF_SIZE(s_canvas_w, s_canvas_h,
                                          16, LV_DRAW_BUF_STRIDE_ALIGN);

#ifndef SIMULATOR
    s_canvas_buf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
#else
    s_canvas_buf = (uint8_t*)malloc(buf_size);
#endif

    if (!s_canvas_buf) {
        printf("[screensaver] ERROR: canvas alloc failed (%zu bytes)\n", buf_size);
        return;
    }

    s_canvas = lv_canvas_create(s_overlay);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, s_canvas_w, s_canvas_h,
                          LV_COLOR_FORMAT_RGB565);

    // Center the canvas on the overlay
    int offset_x = (s_screen_w - s_canvas_w) / 2;
    int offset_y = (s_screen_h - s_canvas_h) / 2;
    lv_obj_set_pos(s_canvas, offset_x, offset_y);

    // Pass touch events through to overlay
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_EVENT_BUBBLE);

    printf("[screensaver] canvas %dx%d (%zu bytes), offset (%d,%d)\n",
           s_canvas_w, s_canvas_h, buf_size, offset_x, offset_y);
}

// ---------------------------------------------------------------------------
// Render tick
// ---------------------------------------------------------------------------

static void render_tick(lv_timer_t* timer) {
    (void)timer;
    if (!s_active || !s_starfield || !s_fb) return;

    // Warp speed cycling
    uint32_t now = millis();
    uint32_t elapsed = now - s_warp_timer;
    float speed;

    if (s_warp_mode) {
        speed = WARP_SPEED;
        if (elapsed >= WARP_DURATION_MS) {
            s_warp_mode = false;
            s_warp_timer = now;
        }
    } else {
        speed = CRUISE_SPEED;
        if (elapsed >= CRUISE_DURATION_MS) {
            s_warp_mode = true;
            s_warp_timer = now;
        }
    }

    s_starfield->update(speed);

    // Clear buffer (~192KB for 400x240 — fast even from PSRAM)
    lv_draw_buf_t* db = lv_canvas_get_draw_buf(s_canvas);
    memset(s_fb, 0, db->header.stride * s_canvas_h);

    // Render stars directly to buffer
    const Star* stars = s_starfield->getStars();
    int count = s_starfield->getStarCount();

    for (int i = 0; i < count; i++) {
        int sx, sy;
        float brightness;
        if (!s_starfield->project(stars[i], sx, sy, brightness)) continue;

        uint8_t gray = (uint8_t)(brightness * 255.0f);
        uint8_t r = gray, g = gray, b = gray;

        switch (stars[i].tint) {
            case TINT_BLUE:   r = gray / 3; g = gray / 2; break;
            case TINT_YELLOW: b = gray / 3; break;
            case TINT_RED:    g = gray / 4; b = gray / 4; break;
            default: break;
        }

        uint16_t color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

        // Core pixel
        if (sx >= 0 && sx < s_canvas_w && sy >= 0 && sy < s_canvas_h) {
            s_fb[sy * s_stride + sx] = color;
        }

        // Bright stars: cross pattern
        if (brightness > 0.5f) {
            if (sx > 0 && sx < s_canvas_w && sy >= 0 && sy < s_canvas_h)
                s_fb[sy * s_stride + sx - 1] = color;
            if (sx >= 0 && sx < s_canvas_w - 1 && sy >= 0 && sy < s_canvas_h)
                s_fb[sy * s_stride + sx + 1] = color;
            if (sx >= 0 && sx < s_canvas_w && sy > 0 && sy < s_canvas_h)
                s_fb[(sy - 1) * s_stride + sx] = color;
            if (sx >= 0 && sx < s_canvas_w && sy >= 0 && sy < s_canvas_h - 1)
                s_fb[(sy + 1) * s_stride + sx] = color;
        }
    }

    // Only invalidate the canvas area (not full screen)
    lv_obj_invalidate(s_canvas);
}

// ---------------------------------------------------------------------------
// Touch handler
// ---------------------------------------------------------------------------

static void on_touch(lv_event_t* e) {
    (void)e;
    dismiss();
}

}  // namespace shell_screensaver
