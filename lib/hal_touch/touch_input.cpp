#include "touch_input.h"
#include "hal_touch.h"
#include "debug_log.h"

#ifdef SIMULATOR

// ── Simulator stubs ─────────────────────────────────────────────────────────

namespace touch_input {
bool init() { return false; }
void inject(uint16_t, uint16_t, bool) {}
void injectRelease() {}
uint32_t lastActivityMs() { return 0; }
bool isRemoteActive() { return false; }
DebugInfo getDebugInfo() { return {}; }
}  // namespace touch_input

#else  // ESP32

#include "tritium_compat.h"
#include <freertos/FreeRTOS.h>

// ── Shared state ────────────────────────────────────────────────────────────

// Injected touch from remote control (written by web handler, read by LVGL cb)
static volatile bool     s_inject_pressed  = false;
static volatile uint16_t s_inject_x        = 0;
static volatile uint16_t s_inject_y        = 0;
static volatile bool     s_inject_pending  = false;  // new injection waiting
static volatile uint32_t s_inject_expire   = 0;      // auto-release after timeout
static volatile uint32_t s_last_activity   = 0;

// Hardware touch instance — expected to be initialized externally
extern TouchHAL touch;

static lv_indev_t* s_indev = nullptr;

// Remote injection timeout: auto-release if no new inject within 2s
static constexpr uint32_t INJECT_TIMEOUT_MS = 2000;

// Debug counters
static volatile uint32_t s_read_cb_calls   = 0;
static volatile uint32_t s_hw_touch_count  = 0;
static volatile uint32_t s_inject_count    = 0;
static volatile int16_t  s_last_raw_x      = -1;
static volatile int16_t  s_last_raw_y      = -1;
static volatile uint32_t s_last_touch_ms   = 0;
static volatile bool     s_currently_pressed = false;

namespace touch_input {

// ── LVGL read callback ──────────────────────────────────────────────────────

void read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    s_read_cb_calls++;

    // Priority 1: injected remote touch
    if (s_inject_pending || s_inject_pressed) {
        uint32_t now = millis();

        // Auto-release if no new injection for a while
        if (s_inject_pressed && !s_inject_pending && now > s_inject_expire) {
            s_inject_pressed = false;
            s_currently_pressed = false;
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = s_inject_x;
            data->point.y = s_inject_y;
            return;
        }

        s_inject_pending = false;
        data->point.x = s_inject_x;
        data->point.y = s_inject_y;
        data->state = s_inject_pressed ? LV_INDEV_STATE_PRESSED
                                       : LV_INDEV_STATE_RELEASED;
        if (s_inject_pressed) {
            s_last_activity = now;
            s_inject_count++;
            s_last_raw_x = s_inject_x;
            s_last_raw_y = s_inject_y;
            s_last_touch_ms = now;
            s_currently_pressed = true;
        } else {
            s_currently_pressed = false;
        }
        return;
    }

    // Priority 2: hardware touch
    uint16_t hx, hy;
    if (touch.available() && touch.read(hx, hy)) {
        data->point.x = hx;
        data->point.y = hy;
        data->state = LV_INDEV_STATE_PRESSED;
        s_last_activity = millis();
        s_hw_touch_count++;
        s_last_raw_x = hx;
        s_last_raw_y = hy;
        s_last_touch_ms = millis();
        s_currently_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        s_currently_pressed = false;
    }
}

// ── Public API ──────────────────────────────────────────────────────────────

bool init() {
    s_indev = lv_indev_create();
    if (!s_indev) {
        DBG_WARN("TOUCH", "Failed to create LVGL indev");
        return false;
    }

    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, read_cb);

    s_last_activity = millis();

    DBG_INFO("TOUCH", "Unified touch input initialized (hw=%s)",
             touch.available() ? "yes" : "no");
    return true;
}

void inject(uint16_t x, uint16_t y, bool pressed) {
    s_inject_x       = x;
    s_inject_y       = y;
    s_inject_pressed = pressed;
    s_inject_pending = true;
    s_inject_expire  = millis() + INJECT_TIMEOUT_MS;
    s_last_activity  = millis();
}

void injectRelease() {
    s_inject_pressed = false;
    s_inject_pending = true;
}

uint32_t lastActivityMs() {
    return s_last_activity;
}

bool isRemoteActive() {
    return s_inject_pressed;
}

DebugInfo getDebugInfo() {
    DebugInfo info = {};
    info.read_cb_calls  = s_read_cb_calls;
    info.hw_touch_count = s_hw_touch_count;
    info.inject_count   = s_inject_count;
    info.last_raw_x     = s_last_raw_x;
    info.last_raw_y     = s_last_raw_y;
    info.last_touch_ms  = s_last_touch_ms;
    info.hw_available   = touch.available();
    info.currently_pressed = s_currently_pressed;
    return info;
}

}  // namespace touch_input

#endif  // SIMULATOR
