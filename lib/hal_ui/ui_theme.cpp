#include "ui_theme.h"

void ui_apply_dark_theme(lv_display_t* disp) {
    // Use LVGL's built-in dark theme
    lv_theme_t* theme = lv_theme_default_init(
        disp,
        lv_color_hex(0x2196F3),  // primary: Material Blue
        lv_color_hex(0xFF9800),  // secondary: Material Orange
        true,                     // dark mode
        LV_FONT_DEFAULT
    );
    lv_display_set_theme(disp, theme);

    // Set global black background (AMOLED power savings)
    lv_obj_t* scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}
