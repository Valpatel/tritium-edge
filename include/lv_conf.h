#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// Color settings
#define LV_COLOR_DEPTH 16

// Disable ARM-specific draw backends (not available on Xtensa/ESP32)
#define LV_USE_DRAW_ARM2D 0
#define LV_USE_DRAW_HELIUM 0

// Memory
#define LV_MEM_CUSTOM 1
#define LV_MEM_SIZE (64 * 1024)

// Display
#define LV_DEF_REFR_PERIOD 16  // ~60 FPS target

// Tick source: registered via lv_tick_set_cb() in lvgl_driver.cpp
// Note: LV_TICK_CUSTOM macros are not used in LVGL 9.2+

// Logging
#define LV_USE_LOG 0

// Fonts - enable built-in fonts
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// Widgets
#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 1
#define LV_USE_BAR 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_LIST 1
#define LV_USE_MSGBOX 1
#define LV_USE_ROLLER 1
#define LV_USE_DROPDOWN 1
#define LV_USE_TEXTAREA 1
#define LV_USE_TABLE 1
#define LV_USE_ARC 1
#define LV_USE_SPINNER 1
#define LV_USE_IMG 1
#define LV_USE_LINE 1
#define LV_USE_CHECKBOX 1

// Layouts
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

// Themes
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

// Built-in symbols/icons
#define LV_USE_FONT_PLACEHOLDER 1

#endif // LV_CONF_H
