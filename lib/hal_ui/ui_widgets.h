#pragma once
#include <lvgl.h>
#include "ui_scaling.h"

// Create a status bar at the top of the screen.
// Shows a title label on the left and a status label on the right.
// Returns the status bar container. Use ui_status_bar_set_*() to update.
lv_obj_t* ui_create_status_bar(lv_obj_t* parent, const UiScale& scale);

// Update status bar fields
void ui_status_bar_set_title(lv_obj_t* bar, const char* title);
void ui_status_bar_set_status(lv_obj_t* bar, const char* status);

// Create a scrollable settings-style list.
// Returns the list object. Add items with lv_list_add_btn/text().
lv_obj_t* ui_create_settings_list(lv_obj_t* parent, const UiScale& scale);

// Show a notification popup with title, message, and a close button.
// Auto-closes after timeout_ms (0 = manual close only).
// Returns the msgbox object.
lv_obj_t* ui_show_notification(const char* title, const char* message,
                                uint32_t timeout_ms);

// Create a battery icon label that can be updated with ui_battery_update().
// Place it in the status bar or anywhere. Returns the label object.
lv_obj_t* ui_create_battery_icon(lv_obj_t* parent);

// Update battery icon with current percentage and charging state.
// Selects appropriate LVGL symbol and color.
void ui_battery_update(lv_obj_t* icon, int percentage, bool charging);

// Show a low battery warning overlay. Call once when battery is critical.
void ui_show_low_battery_warning(int percentage);
