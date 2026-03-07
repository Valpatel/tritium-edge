#include "ui_widgets.h"
#include <cstdio>

// --- Status Bar ---

// Child indices for status bar
static const int STATUS_BAR_TITLE_IDX = 0;
static const int STATUS_BAR_STATUS_IDX = 1;

lv_obj_t* ui_create_status_bar(lv_obj_t* parent, const UiScale& scale) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), scale.status_bar_h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, scale.padding, 0);
    lv_obj_set_style_pad_ver(bar, 2, 0);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

    // Use flex layout: title left, status right
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Title label (left)
    const lv_font_t* font = &lv_font_montserrat_14;
    if (scale.font_size_normal >= 16) font = &lv_font_montserrat_16;
    else if (scale.font_size_normal <= 10) font = &lv_font_montserrat_10;

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, "ESP32");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, font, 0);

    // Status label (right)
    lv_obj_t* status = lv_label_create(bar);
    lv_label_set_text(status, LV_SYMBOL_WIFI " " LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(status, font, 0);

    return bar;
}

void ui_status_bar_set_title(lv_obj_t* bar, const char* title) {
    lv_obj_t* label = lv_obj_get_child(bar, STATUS_BAR_TITLE_IDX);
    if (label) lv_label_set_text(label, title);
}

void ui_status_bar_set_status(lv_obj_t* bar, const char* status) {
    lv_obj_t* label = lv_obj_get_child(bar, STATUS_BAR_STATUS_IDX);
    if (label) lv_label_set_text(label, status);
}

// --- Settings List ---

lv_obj_t* ui_create_settings_list(lv_obj_t* parent, const UiScale& scale) {
    lv_obj_t* list = lv_list_create(parent);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(list, 1);  // Fill remaining space
    lv_obj_set_style_bg_color(list, lv_color_black(), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);

    return list;
}

// --- Notification Popup ---

static void notification_close_cb(lv_event_t* e) {
    lv_obj_t* msgbox = (lv_obj_t*)lv_event_get_user_data(e);
    lv_msgbox_close(msgbox);
}

static void notification_timer_cb(lv_timer_t* timer) {
    lv_obj_t* msgbox = (lv_obj_t*)lv_timer_get_user_data(timer);
    if (msgbox && lv_obj_is_valid(msgbox)) {
        lv_msgbox_close(msgbox);
    }
    lv_timer_delete(timer);
}

lv_obj_t* ui_show_notification(const char* title, const char* message,
                                uint32_t timeout_ms) {
    lv_obj_t* msgbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(msgbox, title);
    lv_msgbox_add_text(msgbox, message);

    lv_obj_t* close_btn = lv_msgbox_add_close_button(msgbox);
    lv_obj_add_event_cb(close_btn, notification_close_cb, LV_EVENT_CLICKED, msgbox);

    lv_obj_center(msgbox);

    // Dark styling
    lv_obj_set_style_bg_color(msgbox, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_border_color(msgbox, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_border_width(msgbox, 2, 0);

    if (timeout_ms > 0) {
        lv_timer_create(notification_timer_cb, timeout_ms, msgbox);
    }

    return msgbox;
}

// --- Battery Icon ---

lv_obj_t* ui_create_battery_icon(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(label, lv_color_hex(0x4CAF50), 0);
    return label;
}

void ui_battery_update(lv_obj_t* icon, int percentage, bool charging) {
    if (!icon) return;

    const char* symbol;
    uint32_t color;

    if (charging) {
        symbol = LV_SYMBOL_CHARGE " " LV_SYMBOL_BATTERY_FULL;
        color = 0x2196F3; // blue
    } else if (percentage < 0) {
        symbol = LV_SYMBOL_USB;
        color = 0x888888;
    } else if (percentage >= 75) {
        symbol = LV_SYMBOL_BATTERY_FULL;
        color = 0x4CAF50; // green
    } else if (percentage >= 50) {
        symbol = LV_SYMBOL_BATTERY_3;
        color = 0x8BC34A; // light green
    } else if (percentage >= 25) {
        symbol = LV_SYMBOL_BATTERY_2;
        color = 0xFF9800; // orange
    } else if (percentage >= 10) {
        symbol = LV_SYMBOL_BATTERY_1;
        color = 0xFF5722; // deep orange
    } else {
        symbol = LV_SYMBOL_BATTERY_EMPTY;
        color = 0xF44336; // red
    }

    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
}

void ui_show_low_battery_warning(int percentage) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Battery at %d%%\nPlease connect charger.", percentage);
    ui_show_notification(LV_SYMBOL_WARNING " Low Battery", msg, 5000);
}
