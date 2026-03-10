// Tritium-OS Lock Screen — PIN entry overlay.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "lock_screen.h"
#include "shell_theme.h"
#include "os_shell.h"
#include <cstdio>
#include <cstring>
#include <ctime>

#if __has_include("os_settings.h")
#include "os_settings.h"
#define SETTINGS_AVAILABLE 1
#else
#define SETTINGS_AVAILABLE 0
#endif

namespace lock_screen {

// Colors come from shell_theme.h macros (T_CYAN, T_VOID, T_GHOST, etc.)
// T_RED/T_SURFACE aliases for readability:
#define T_RED     T_MAGENTA
#define T_SURFACE T_SURFACE2

static constexpr int MAX_PIN_LEN = 8;
static constexpr int MIN_PIN_LEN = 4;

// State
static lv_obj_t* s_overlay = nullptr;
static lv_obj_t* s_clock_lbl = nullptr;
static lv_obj_t* s_date_lbl = nullptr;
static lv_obj_t* s_dots = nullptr;
static lv_obj_t* s_status_lbl = nullptr;
static lv_obj_t* s_dot_objs[MAX_PIN_LEN] = {};

static char s_stored_pin[MAX_PIN_LEN + 1] = {};
static char s_entered[MAX_PIN_LEN + 1] = {};
static int s_entered_len = 0;
static bool s_locked = false;
static int s_attempts = 0;
static uint32_t s_lockout_until = 0;

static void load_pin() {
#if SETTINGS_AVAILABLE
    TritiumSettings& s = TritiumSettings::instance();
    const char* stored = s.getString(SettingsDomain::SYSTEM, "lock_pin", "");
    if (stored && stored[0]) {
        strncpy(s_stored_pin, stored, MAX_PIN_LEN);
        s_stored_pin[MAX_PIN_LEN] = '\0';
    } else {
        s_stored_pin[0] = '\0';
    }
#endif
}

static void update_dots() {
    if (!s_dots) return;
    for (int i = 0; i < MAX_PIN_LEN; i++) {
        if (!s_dot_objs[i]) continue;
        if (i < s_entered_len) {
            lv_obj_set_style_bg_color(s_dot_objs[i], T_CYAN, 0);
            lv_obj_set_style_bg_opa(s_dot_objs[i], LV_OPA_COVER, 0);
        } else if (i < (int)strlen(s_stored_pin)) {
            lv_obj_set_style_bg_color(s_dot_objs[i], T_GHOST, 0);
            lv_obj_set_style_bg_opa(s_dot_objs[i], LV_OPA_50, 0);
        } else {
            lv_obj_set_style_bg_opa(s_dot_objs[i], LV_OPA_TRANSP, 0);
        }
    }
}

static void check_pin() {
    if (strcmp(s_entered, s_stored_pin) == 0) {
        // Correct PIN
        s_locked = false;
        s_attempts = 0;
        s_entered_len = 0;
        s_entered[0] = '\0';
        if (s_overlay) {
            lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_status_lbl) {
            lv_label_set_text(s_status_lbl, "");
        }
    } else {
        // Wrong PIN
        s_attempts++;
        s_entered_len = 0;
        s_entered[0] = '\0';
        update_dots();

        if (s_status_lbl) {
            if (s_attempts >= 5) {
                lv_label_set_text(s_status_lbl, "Too many attempts. Wait 30s.");
                // Lockout for 30 seconds
                // (millis not easily accessible here, use lv_tick for simplicity)
                s_lockout_until = lv_tick_get() + 30000;
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "Wrong PIN (%d/5)", s_attempts);
                lv_label_set_text(s_status_lbl, buf);
            }
            lv_obj_set_style_text_color(s_status_lbl, T_RED, 0);
        }
    }
}

static void key_cb(lv_event_t* e) {
    const char* txt = lv_btnmatrix_get_btn_text(
        (lv_obj_t*)lv_event_get_target(e),
        lv_btnmatrix_get_selected_btn((lv_obj_t*)lv_event_get_target(e)));
    if (!txt) return;

    // Check lockout
    if (s_lockout_until > 0 && lv_tick_get() < s_lockout_until) {
        return;
    }
    if (s_lockout_until > 0 && lv_tick_get() >= s_lockout_until) {
        s_lockout_until = 0;
        s_attempts = 0;
        if (s_status_lbl) {
            lv_label_set_text(s_status_lbl, "Enter PIN");
            lv_obj_set_style_text_color(s_status_lbl, T_GHOST, 0);
        }
    }

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        if (s_entered_len > 0) {
            s_entered_len--;
            s_entered[s_entered_len] = '\0';
            update_dots();
        }
    } else if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        if (s_entered_len >= MIN_PIN_LEN) {
            check_pin();
        }
    } else if (txt[0] >= '0' && txt[0] <= '9' && s_entered_len < MAX_PIN_LEN) {
        s_entered[s_entered_len++] = txt[0];
        s_entered[s_entered_len] = '\0';
        update_dots();

        // Auto-check when PIN length matches stored
        if (s_entered_len == (int)strlen(s_stored_pin)) {
            check_pin();
        }
    }
}

void init() {
    load_pin();
}

void show() {
    if (!is_enabled()) return;

    s_locked = true;
    s_entered_len = 0;
    s_entered[0] = '\0';

    // Create full-screen overlay if not already created
    if (!s_overlay) {
        s_overlay = lv_obj_create(lv_screen_active());
        lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(s_overlay, 0, 0);
        lv_obj_set_style_bg_color(s_overlay, T_VOID, 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_overlay, 0, 0);
        lv_obj_set_style_radius(s_overlay, 0, 0);
        lv_obj_set_style_pad_all(s_overlay, 0, 0);
        lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

        // Spacer top
        lv_obj_t* spacer_top = lv_obj_create(s_overlay);
        lv_obj_set_size(spacer_top, 1, 20);
        lv_obj_set_style_bg_opa(spacer_top, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer_top, 0, 0);

        // Clock
        s_clock_lbl = lv_label_create(s_overlay);
        lv_label_set_text(s_clock_lbl, "--:--");
        lv_obj_set_style_text_font(s_clock_lbl, &lv_font_montserrat_36, 0);
        lv_obj_set_style_text_color(s_clock_lbl, T_CYAN, 0);

        // Date
        s_date_lbl = lv_label_create(s_overlay);
        lv_label_set_text(s_date_lbl, "");
        lv_obj_set_style_text_font(s_date_lbl, tritium_shell::uiSmallFont(), 0);
        lv_obj_set_style_text_color(s_date_lbl, T_GHOST, 0);

        // Spacer
        lv_obj_t* spacer_mid = lv_obj_create(s_overlay);
        lv_obj_set_size(spacer_mid, 1, 10);
        lv_obj_set_style_bg_opa(spacer_mid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer_mid, 0, 0);

        // PIN dots row
        s_dots = lv_obj_create(s_overlay);
        lv_obj_set_size(s_dots, 200, 24);
        lv_obj_set_style_bg_opa(s_dots, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_dots, 0, 0);
        lv_obj_set_style_pad_all(s_dots, 0, 0);
        lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(s_dots, 8, 0);
        lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_SCROLLABLE);

        int pin_len = strlen(s_stored_pin);
        for (int i = 0; i < pin_len && i < MAX_PIN_LEN; i++) {
            s_dot_objs[i] = lv_obj_create(s_dots);
            lv_obj_set_size(s_dot_objs[i], 14, 14);
            lv_obj_set_style_radius(s_dot_objs[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(s_dot_objs[i], T_GHOST, 0);
            lv_obj_set_style_bg_opa(s_dot_objs[i], LV_OPA_50, 0);
            lv_obj_set_style_border_color(s_dot_objs[i], T_CYAN, 0);
            lv_obj_set_style_border_width(s_dot_objs[i], 1, 0);
            lv_obj_set_style_border_opa(s_dot_objs[i], LV_OPA_50, 0);
        }

        // Status label
        s_status_lbl = lv_label_create(s_overlay);
        lv_label_set_text(s_status_lbl, "Enter PIN");
        lv_obj_set_style_text_font(s_status_lbl, tritium_shell::uiSmallFont(), 0);
        lv_obj_set_style_text_color(s_status_lbl, T_GHOST, 0);

        // Numeric keypad
        static const char* btn_map[] = {
            "1", "2", "3", "\n",
            "4", "5", "6", "\n",
            "7", "8", "9", "\n",
            LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
        };

        lv_obj_t* keypad = lv_btnmatrix_create(s_overlay);
        lv_btnmatrix_set_map(keypad, btn_map);
        lv_obj_set_size(keypad, 240, 200);
        lv_obj_set_style_bg_color(keypad, T_SURFACE, 0);
        lv_obj_set_style_bg_opa(keypad, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(keypad, T_CYAN, 0);
        lv_obj_set_style_border_width(keypad, 1, 0);
        lv_obj_set_style_border_opa(keypad, LV_OPA_20, 0);
        lv_obj_set_style_radius(keypad, 8, 0);
        lv_obj_set_style_pad_all(keypad, 4, 0);

        // Button styling
        lv_obj_set_style_bg_color(keypad, T_SURFACE, LV_PART_ITEMS);
        lv_obj_set_style_bg_opa(keypad, LV_OPA_COVER, LV_PART_ITEMS);
        lv_obj_set_style_text_color(keypad, T_CYAN, LV_PART_ITEMS);
        lv_obj_set_style_text_font(keypad, tritium_shell::uiFont(), LV_PART_ITEMS);
        lv_obj_set_style_border_color(keypad, T_CYAN, LV_PART_ITEMS);
        lv_obj_set_style_border_width(keypad, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_opa(keypad, LV_OPA_30, LV_PART_ITEMS);
        lv_obj_set_style_radius(keypad, 4, LV_PART_ITEMS);

        lv_obj_add_event_cb(keypad, key_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    update_dots();
}

bool is_locked() {
    return s_locked;
}

bool is_enabled() {
    return s_stored_pin[0] != '\0';
}

void set_pin(const char* pin) {
    if (!pin || strlen(pin) < MIN_PIN_LEN) {
        s_stored_pin[0] = '\0';
    } else {
        strncpy(s_stored_pin, pin, MAX_PIN_LEN);
        s_stored_pin[MAX_PIN_LEN] = '\0';
    }

#if SETTINGS_AVAILABLE
    TritiumSettings::instance().setString(
        SettingsDomain::SYSTEM, "lock_pin", s_stored_pin);
#endif
}

void tick() {
    if (!s_locked || !s_overlay) return;

    // Update clock every second
    static uint32_t last_tick = 0;
    uint32_t now = lv_tick_get();
    if (now - last_tick < 1000) return;
    last_tick = now;

    if (s_clock_lbl) {
        struct tm timeinfo;
        time_t t = time(nullptr);
        if (localtime_r(&t, &timeinfo) && timeinfo.tm_year > 100) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
            lv_label_set_text(s_clock_lbl, buf);

            if (s_date_lbl) {
                static const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                               "Jul","Aug","Sep","Oct","Nov","Dec"};
                char dbuf[24];
                snprintf(dbuf, sizeof(dbuf), "%s %s %d",
                         days[timeinfo.tm_wday], months[timeinfo.tm_mon], timeinfo.tm_mday);
                lv_label_set_text(s_date_lbl, dbuf);
            }
        }
    }

    // Update lockout status
    if (s_lockout_until > 0 && s_status_lbl) {
        if (now >= s_lockout_until) {
            s_lockout_until = 0;
            s_attempts = 0;
            lv_label_set_text(s_status_lbl, "Enter PIN");
            lv_obj_set_style_text_color(s_status_lbl, T_GHOST, 0);
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "Locked (%ds)",
                     (int)((s_lockout_until - now) / 1000));
            lv_label_set_text(s_status_lbl, buf);
        }
    }
}

}  // namespace lock_screen
