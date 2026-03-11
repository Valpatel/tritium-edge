/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "shell_theme.h"
#include "os_shell.h"
#include "themes/lv_theme_private.h"
#include <cstdio>

namespace tritium_theme {

// ---------------------------------------------------------------------------
// Theme application — custom callback wrapper
// ---------------------------------------------------------------------------

// Style that neutralizes the LVGL default theme's button "grow" on press.
// The default theme applies transform_width/height + translate_x/y on
// LV_STATE_PRESSED which visually shifts button content. We add this style
// AFTER the default theme so it wins the cascade (last-added = highest prio).
static lv_style_t s_btn_no_grow;
static bool s_btn_no_grow_inited = false;

// Original theme callback from lv_theme_default_init().
static lv_theme_apply_cb_t s_original_apply_cb = nullptr;

// Our wrapper callback: let the default theme apply first, then for buttons
// add the no-grow override so pressed state doesn't shift content.
static void tritium_theme_apply_cb(lv_theme_t* th, lv_obj_t* obj) {
    // Run the original default theme callback
    if (s_original_apply_cb) {
        s_original_apply_cb(th, obj);
    }

    // For buttons: neutralize grow/translate on pressed state
    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &s_btn_no_grow,
                         (lv_style_selector_t)(LV_PART_MAIN | LV_STATE_PRESSED));
    }
}

void apply() {
    lv_display_t* disp = lv_display_get_default();
    if (!disp) return;

    // Initialize the no-grow style once
    if (!s_btn_no_grow_inited) {
        lv_style_init(&s_btn_no_grow);
        lv_style_set_transform_width(&s_btn_no_grow, 0);
        lv_style_set_transform_height(&s_btn_no_grow, 0);
        lv_style_set_translate_x(&s_btn_no_grow, 0);
        lv_style_set_translate_y(&s_btn_no_grow, 0);
        lv_style_set_shadow_ofs_y(&s_btn_no_grow, 0);
        s_btn_no_grow_inited = true;
    }

    // Initialize the built-in dark theme with Tritium primary/secondary colors
    lv_theme_t* theme = lv_theme_default_init(
        disp,
        T_CYAN,     // primary
        T_MAGENTA,  // secondary
        true,       // dark mode
        LV_FONT_DEFAULT
    );

    // Wrap the theme callback: save original, install ours
    s_original_apply_cb = theme->apply_cb;
    lv_theme_set_apply_cb(theme, tritium_theme_apply_cb);

    lv_display_set_theme(disp, theme);

    // Set active screen to void black
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, T_VOID, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Global scrollbar style: thin, cyan, rounded
    static lv_style_t style_scrollbar;
    lv_style_init(&style_scrollbar);
    lv_style_set_width(&style_scrollbar, 3);
    lv_style_set_bg_color(&style_scrollbar, T_CYAN);
    lv_style_set_bg_opa(&style_scrollbar, LV_OPA_40);
    lv_style_set_radius(&style_scrollbar, 2);
    lv_obj_add_style(scr, &style_scrollbar, LV_PART_SCROLLBAR);
}

// ---------------------------------------------------------------------------
// Themed widget constructors
// ---------------------------------------------------------------------------

lv_obj_t* createPanel(lv_obj_t* parent, const char* title) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_style_bg_color(panel, T_SURFACE2, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, T_CYAN, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_20, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);

    if (title) {
        lv_obj_t* lbl = lv_label_create(panel);
        lv_label_set_text(lbl, title);
        lv_obj_set_style_text_color(lbl, T_BRIGHT, 0);
        lv_obj_set_style_text_font(lbl, tritium_shell::uiFont(), 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    return panel;
}

lv_obj_t* createButton(lv_obj_t* parent, const char* label, lv_color_t accent) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(btn, accent, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_style_pad_ver(btn, 6, 0);

    // Pressed state: surface-3 fill
    lv_obj_set_style_bg_color(btn, T_SURFACE3, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);

    // Label
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, accent, 0);
    lv_obj_set_style_text_font(lbl, tritium_shell::uiFont(), 0);
    lv_obj_center(lbl);

    return btn;
}

lv_obj_t* createLabel(lv_obj_t* parent, const char* text, bool mono) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, T_TEXT, 0);

    if (mono) {
        // Use Montserrat 12 as fallback — true monospace requires custom font.
        // LVGL 9.2 built-in Unscii is only available if LV_FONT_UNSCII_8/16
        // are enabled in lv_conf.h. We use the smallest Montserrat as a
        // reasonable substitute that's always available.
        lv_obj_set_style_text_font(lbl, tritium_shell::uiSmallFont(), 0);
    }

    return lbl;
}

lv_obj_t* createStatusDot(lv_obj_t* parent, lv_color_t color) {
    lv_obj_t* dot = lv_obj_create(parent);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_shadow_color(dot, color, 0);
    lv_obj_set_style_shadow_width(dot, 6, 0);
    lv_obj_set_style_shadow_opa(dot, LV_OPA_30, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

lv_obj_t* createSlider(lv_obj_t* parent, int min, int max, int initial) {
    lv_obj_t* slider = lv_slider_create(parent);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, initial, LV_ANIM_OFF);

    // Track: surface-3
    lv_obj_set_style_bg_color(slider, T_SURFACE3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);

    // Indicator: cyan
    lv_obj_set_style_bg_color(slider, T_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);

    // Knob: cyan with glow shadow
    lv_obj_set_style_bg_color(slider, T_CYAN, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(slider, T_CYAN, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);

    return slider;
}

lv_obj_t* createSwitch(lv_obj_t* parent, bool initial) {
    lv_obj_t* sw = lv_switch_create(parent);

    // Off state: ghost
    lv_obj_set_style_bg_color(sw, T_GHOST, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_40, LV_PART_MAIN);

    // On state: cyan indicator
    lv_obj_set_style_bg_color(sw, T_CYAN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);

    // Knob
    lv_obj_set_style_bg_color(sw, T_BRIGHT, LV_PART_KNOB);

    if (initial) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

    return sw;
}

}  // namespace tritium_theme
