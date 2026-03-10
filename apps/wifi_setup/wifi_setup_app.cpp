#include "wifi_setup_app.h"
#include "ui_init.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include <cstdio>

#ifdef SIMULATOR
#include <SDL2/SDL.h>
static uint32_t millis() { return SDL_GetTicks(); }
#else
#include "tritium_compat.h"
#endif

WifiSetupApp* WifiSetupApp::_instance = nullptr;

// Signal strength to icon helper
static const char* rssi_icon(int32_t rssi) {
    if (rssi >= -50) return LV_SYMBOL_WIFI;
    if (rssi >= -70) return LV_SYMBOL_WIFI;
    return LV_SYMBOL_WIFI;
}

static lv_color_t rssi_color(int32_t rssi) {
    if (rssi >= -50) return lv_color_hex(0x4CAF50); // green
    if (rssi >= -70) return lv_color_hex(0xFF9800); // orange
    return lv_color_hex(0xF44336); // red
}

static const char* auth_icon(WifiAuth auth) {
    return (auth == WifiAuth::OPEN) ? "" : LV_SYMBOL_EYE_CLOSE " ";
}

// --- Setup and Loop ---

void WifiSetupApp::setup(LGFX& display) {
    _instance = this;

    lv_display_t* disp = ui_init(display);
    ui_apply_dark_theme(disp);

    _scale = ui_compute_scale(display.width(), display.height());

    _wifi.init();
    _wifi.onStateChange(onWifiStateChange);

    createMainScreen();

    // Auto-scan on startup
    _wifi.startScan();
    _scan_pending = true;
    _scan_timer = millis();
    _status_timer = millis();
}

void WifiSetupApp::loop(LGFX& display) {
    ui_tick();

    uint32_t now = millis();

    // Check if scan completed
    if (_scan_pending && _wifi.getState() != WifiState::SCANNING) {
        _scan_pending = false;
        refreshScanList();
    }

    // Update status bar every 2s
    if (now - _status_timer >= 2000) {
        _status_timer = now;
        refreshStatusBar();
    }
}

// --- Screen creation ---

void WifiSetupApp::createMainScreen() {
    _main_screen = lv_screen_active();
    lv_obj_set_flex_flow(_main_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_main_screen, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(_main_screen, 0, 0);
    lv_obj_set_style_pad_gap(_main_screen, 0, 0);

    // Status bar
    _status_bar = ui_create_status_bar(_main_screen, _scale);
    ui_status_bar_set_title(_status_bar, "WiFi Setup");
    refreshStatusBar();

    // Button row: Scan + Saved
    lv_obj_t* btn_row = lv_obj_create(_main_screen);
    lv_obj_set_size(btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(btn_row, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_radius(btn_row, 0, 0);
    lv_obj_set_style_pad_ver(btn_row, _scale.padding / 2, 0);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* scan_btn = lv_btn_create(btn_row);
    lv_obj_t* scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, LV_SYMBOL_REFRESH " Scan");
    lv_obj_add_event_cb(scan_btn, onScanBtnClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* saved_btn = lv_btn_create(btn_row);
    lv_obj_t* saved_lbl = lv_label_create(saved_btn);
    lv_label_set_text(saved_lbl, LV_SYMBOL_LIST " Saved");
    lv_obj_add_event_cb(saved_btn, onSavedBtnClicked, LV_EVENT_CLICKED, this);

    // Scan results list
    _scan_list = lv_list_create(_main_screen);
    lv_obj_set_size(_scan_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(_scan_list, 1);
    lv_obj_set_style_bg_color(_scan_list, lv_color_black(), 0);
    lv_obj_set_style_border_width(_scan_list, 0, 0);
    lv_obj_set_style_radius(_scan_list, 0, 0);
    lv_obj_set_style_pad_all(_scan_list, 0, 0);

    lv_list_add_text(_scan_list, "Scanning...");
}

void WifiSetupApp::createSavedScreen() {
    _saved_screen = lv_obj_create(NULL);
    lv_obj_set_flex_flow(_saved_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(_saved_screen, 0, 0);
    lv_obj_set_style_pad_gap(_saved_screen, 0, 0);

    // Header with back button
    lv_obj_t* header = lv_obj_create(_saved_screen);
    lv_obj_set_size(header, lv_pct(100), _scale.status_bar_h);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_hor(header, _scale.padding, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back_btn = lv_btn_create(header);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_t* back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0x2196F3), 0);
    lv_obj_add_event_cb(back_btn, onBackBtnClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Saved Networks");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    // Saved list
    _saved_list = lv_list_create(_saved_screen);
    lv_obj_set_size(_saved_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(_saved_list, 1);
    lv_obj_set_style_bg_color(_saved_list, lv_color_black(), 0);
    lv_obj_set_style_border_width(_saved_list, 0, 0);
    lv_obj_set_style_radius(_saved_list, 0, 0);
    lv_obj_set_style_pad_all(_saved_list, 0, 0);

    refreshSavedList();
}

// --- Refresh lists ---

void WifiSetupApp::refreshScanList() {
    if (!_scan_list) return;
    lv_obj_clean(_scan_list);

    ScanResult results[WIFI_MAX_SCAN_RESULTS];
    int count = _wifi.getScanResults(results, WIFI_MAX_SCAN_RESULTS);

    if (count == 0) {
        lv_list_add_text(_scan_list, "No networks found");
        return;
    }

    char label_buf[64];
    for (int i = 0; i < count; i++) {
        snprintf(label_buf, sizeof(label_buf), "%s%s (%ddBm)",
                 auth_icon(results[i].auth),
                 results[i].ssid,
                 (int)results[i].rssi);

        lv_obj_t* btn = lv_list_add_btn(_scan_list, rssi_icon(results[i].rssi), label_buf);

        // Color the wifi icon by signal strength
        lv_obj_t* icon = lv_obj_get_child(btn, 0);
        if (icon) {
            lv_obj_set_style_text_color(icon, rssi_color(results[i].rssi), 0);
        }

        // Store SSID index as user data
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, onApItemClicked, LV_EVENT_CLICKED, this);
    }
}

void WifiSetupApp::refreshSavedList() {
    if (!_saved_list) return;
    lv_obj_clean(_saved_list);

    SavedNetwork saved[WIFI_MAX_SAVED_NETWORKS];
    int count = _wifi.getSavedNetworks(saved, WIFI_MAX_SAVED_NETWORKS);

    if (count == 0) {
        lv_list_add_text(_saved_list, "No saved networks");
        return;
    }

    const char* connected_ssid = _wifi.isConnected() ? _wifi.getSSID() : "";

    for (int i = 0; i < count; i++) {
        const char* icon = (strcmp(saved[i].ssid, connected_ssid) == 0)
                           ? LV_SYMBOL_OK : LV_SYMBOL_WIFI;

        lv_obj_t* btn = lv_list_add_btn(_saved_list, icon, saved[i].ssid);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, onSavedItemClicked, LV_EVENT_LONG_PRESSED, this);
    }

    lv_list_add_text(_saved_list, "Long press to delete");
}

void WifiSetupApp::refreshStatusBar() {
    if (!_status_bar) return;

    char buf[48];
    if (_wifi.isConnected()) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s", _wifi.getSSID());
    } else if (_wifi.getState() == WifiState::CONNECTING) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH " Connecting...");
    } else if (_wifi.getState() == WifiState::SCANNING) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH " Scanning...");
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_CLOSE " Disconnected");
    }
    ui_status_bar_set_status(_status_bar, buf);
}

// --- Password dialog ---

void WifiSetupApp::showPasswordDialog(const char* ssid) {
    strncpy(_selected_ssid, ssid, sizeof(_selected_ssid) - 1);

    _pwd_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_pwd_dialog, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_center(_pwd_dialog);
    lv_obj_set_style_bg_color(_pwd_dialog, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_border_color(_pwd_dialog, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_border_width(_pwd_dialog, 2, 0);
    lv_obj_set_style_radius(_pwd_dialog, 8, 0);
    lv_obj_set_style_pad_all(_pwd_dialog, _scale.padding, 0);
    lv_obj_set_flex_flow(_pwd_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_pwd_dialog, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(_pwd_dialog, _scale.padding / 2, 0);

    // Title
    char title[64];
    snprintf(title, sizeof(title), "Connect to %s", ssid);
    lv_obj_t* lbl = lv_label_create(_pwd_dialog);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));

    // Password input
    _pwd_input = lv_textarea_create(_pwd_dialog);
    lv_obj_set_width(_pwd_input, lv_pct(100));
    lv_textarea_set_placeholder_text(_pwd_input, "Password");
    lv_textarea_set_password_mode(_pwd_input, true);
    lv_textarea_set_one_line(_pwd_input, true);
    lv_textarea_set_max_length(_pwd_input, 63);

    // Show/hide password row
    lv_obj_t* show_row = lv_obj_create(_pwd_dialog);
    lv_obj_set_size(show_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(show_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(show_row, 0, 0);
    lv_obj_set_style_pad_all(show_row, 0, 0);
    lv_obj_set_flex_flow(show_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(show_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(show_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cb = lv_checkbox_create(show_row);
    lv_checkbox_set_text(cb, "Show password");
    lv_obj_set_style_text_color(cb, lv_color_hex(0x888888), 0);
    lv_obj_add_event_cb(cb, onPasswordShowToggle, LV_EVENT_VALUE_CHANGED, this);

    // Button row
    lv_obj_t* btn_row = lv_obj_create(_pwd_dialog);
    lv_obj_set_size(btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_row, _scale.padding, 0);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    // Cancel
    lv_obj_t* cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x333333), 0);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_add_event_cb(cancel_btn, onBackBtnClicked, LV_EVENT_CLICKED, this);

    // Connect
    lv_obj_t* connect_btn = lv_btn_create(btn_row);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x2196F3), 0);
    lv_obj_t* connect_lbl = lv_label_create(connect_btn);
    lv_label_set_text(connect_lbl, LV_SYMBOL_OK " Connect");
    lv_obj_add_event_cb(connect_btn, onConnectBtnClicked, LV_EVENT_CLICKED, this);

    // Open keyboard for the text area
    lv_obj_t* kb = lv_keyboard_create(_pwd_dialog);
    lv_keyboard_set_textarea(kb, _pwd_input);
    lv_obj_set_width(kb, lv_pct(100));
}

void WifiSetupApp::showConnecting(const char* ssid) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to %s...", ssid);
    ui_show_notification(LV_SYMBOL_WIFI " WiFi", msg, 3000);
}

// --- Event callbacks ---

void WifiSetupApp::onScanBtnClicked(lv_event_t* e) {
    auto* self = (WifiSetupApp*)lv_event_get_user_data(e);
    if (!self->_scan_pending) {
        self->_wifi.startScan();
        self->_scan_pending = true;
        if (self->_scan_list) {
            lv_obj_clean(self->_scan_list);
            lv_list_add_text(self->_scan_list, "Scanning...");
        }
    }
}

void WifiSetupApp::onSavedBtnClicked(lv_event_t* e) {
    auto* self = (WifiSetupApp*)lv_event_get_user_data(e);
    if (!self->_saved_screen) {
        self->createSavedScreen();
    } else {
        self->refreshSavedList();
    }
    lv_screen_load(self->_saved_screen);
}

void WifiSetupApp::onBackBtnClicked(lv_event_t* e) {
    auto* self = (WifiSetupApp*)lv_event_get_user_data(e);

    // Close password dialog if open
    if (self->_pwd_dialog) {
        lv_obj_delete(self->_pwd_dialog);
        self->_pwd_dialog = nullptr;
        self->_pwd_input = nullptr;
        return;
    }

    // Go back to main screen
    if (self->_main_screen) {
        lv_screen_load(self->_main_screen);
    }
}

void WifiSetupApp::onApItemClicked(lv_event_t* e) {
    auto* self = (WifiSetupApp*)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);

    ScanResult results[WIFI_MAX_SCAN_RESULTS];
    int count = self->_wifi.getScanResults(results, WIFI_MAX_SCAN_RESULTS);
    if (idx < 0 || idx >= count) return;

    if (results[idx].auth == WifiAuth::OPEN) {
        // Open network - connect directly
        self->_wifi.addNetwork(results[idx].ssid, "");
        self->_wifi.connect(results[idx].ssid);
        self->showConnecting(results[idx].ssid);
    } else {
        // Need password
        self->showPasswordDialog(results[idx].ssid);
    }
}

void WifiSetupApp::onSavedItemClicked(lv_event_t* e) {
    auto* self = (WifiSetupApp*)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);

    SavedNetwork saved[WIFI_MAX_SAVED_NETWORKS];
    int count = self->_wifi.getSavedNetworks(saved, WIFI_MAX_SAVED_NETWORKS);
    if (idx < 0 || idx >= count) return;

    // Confirm delete
    char msg[64];
    snprintf(msg, sizeof(msg), "Delete \"%s\"?", saved[idx].ssid);

    lv_obj_t* msgbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(msgbox, "Remove Network");
    lv_msgbox_add_text(msgbox, msg);
    lv_obj_t* yes_btn = lv_msgbox_add_footer_button(msgbox, "Delete");
    lv_obj_t* no_btn = lv_msgbox_add_footer_button(msgbox, "Cancel");

    // Store ssid to delete
    strncpy(self->_selected_ssid, saved[idx].ssid, sizeof(self->_selected_ssid) - 1);
    lv_obj_add_event_cb(yes_btn, onDeleteConfirm, LV_EVENT_CLICKED, self);
    lv_obj_add_event_cb(no_btn, [](lv_event_t* ev) {
        lv_obj_t* mbox = lv_obj_get_parent(lv_obj_get_parent((lv_obj_t*)lv_event_get_target(ev)));
        lv_msgbox_close(mbox);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_center(msgbox);
    lv_obj_set_style_bg_color(msgbox, lv_color_hex(0x1A1A2E), 0);
}

void WifiSetupApp::onConnectBtnClicked(lv_event_t* e) {
    auto* self = (WifiSetupApp*)lv_event_get_user_data(e);
    if (!self->_pwd_input) return;

    const char* password = lv_textarea_get_text(self->_pwd_input);
    self->_wifi.addNetwork(self->_selected_ssid, password);
    self->_wifi.connect(self->_selected_ssid);

    // Close dialog
    if (self->_pwd_dialog) {
        lv_obj_delete(self->_pwd_dialog);
        self->_pwd_dialog = nullptr;
        self->_pwd_input = nullptr;
    }

    self->showConnecting(self->_selected_ssid);
}

void WifiSetupApp::onPasswordShowToggle(lv_event_t* e) {
    auto* self = (WifiSetupApp*)lv_event_get_user_data(e);
    if (!self->_pwd_input) return;
    lv_obj_t* cb = (lv_obj_t*)lv_event_get_target(e);
    bool checked = lv_obj_has_state(cb, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(self->_pwd_input, !checked);
}

void WifiSetupApp::onDeleteConfirm(lv_event_t* e) {
    auto* self = (WifiSetupApp*)lv_event_get_user_data(e);
    self->_wifi.removeNetwork(self->_selected_ssid);

    // Close msgbox
    lv_obj_t* mbox = lv_obj_get_parent(lv_obj_get_parent((lv_obj_t*)lv_event_get_target(e)));
    lv_msgbox_close(mbox);

    self->refreshSavedList();
}

void WifiSetupApp::onWifiStateChange(WifiState state) {
    if (!_instance) return;

    switch (state) {
        case WifiState::CONNECTED: {
            char msg[64];
            snprintf(msg, sizeof(msg), "Connected!\nIP: %s", _instance->_wifi.getIP());
            ui_show_notification(LV_SYMBOL_OK " WiFi", msg, 3000);
            break;
        }
        case WifiState::FAILED:
            ui_show_notification(LV_SYMBOL_CLOSE " WiFi", "Connection failed.", 3000);
            break;
        case WifiState::DISCONNECTED:
            ui_show_notification(LV_SYMBOL_CLOSE " WiFi", "Disconnected.", 2000);
            break;
        default:
            break;
    }

    _instance->refreshStatusBar();
}
