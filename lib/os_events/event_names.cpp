/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "os_events.h"

const char* eventIdToName(EventId id) {
    switch (id) {
        // System
        case EVT_SYSTEM_BOOT:          return "system.boot";
        case EVT_SYSTEM_SLEEP:         return "system.sleep";
        case EVT_SYSTEM_WAKE:          return "system.wake";
        case EVT_SYSTEM_LOW_MEMORY:    return "system.low_memory";
        case EVT_SYSTEM_WATCHDOG:      return "system.watchdog";

        // WiFi
        case EVT_WIFI_CONNECTED:       return "wifi.connected";
        case EVT_WIFI_DISCONNECTED:    return "wifi.disconnected";
        case EVT_WIFI_SCAN_DONE:       return "wifi.scan_done";
        case EVT_WIFI_AP_START:        return "wifi.ap_start";
        case EVT_WIFI_AP_CLIENT:       return "wifi.ap_client";
        case EVT_WIFI_IP_CHANGED:      return "wifi.ip_changed";

        // BLE
        case EVT_BLE_DEVICE_FOUND:     return "ble.device_found";
        case EVT_BLE_DEVICE_LOST:      return "ble.device_lost";
        case EVT_BLE_CONNECTED:        return "ble.connected";
        case EVT_BLE_DISCONNECTED:     return "ble.disconnected";
        case EVT_BLE_DATA_RECEIVED:    return "ble.data_received";

        // Mesh
        case EVT_MESH_PEER_JOINED:     return "mesh.peer_joined";
        case EVT_MESH_PEER_LEFT:       return "mesh.peer_left";
        case EVT_MESH_MESSAGE:         return "mesh.message";
        case EVT_MESH_TOPOLOGY:        return "mesh.topology";

        // Display
        case EVT_DISPLAY_READY:        return "display.ready";
        case EVT_DISPLAY_SLEEP:        return "display.sleep";
        case EVT_DISPLAY_WAKE:         return "display.wake";

        // Touch
        case EVT_TOUCH_TAP:            return "touch.tap";
        case EVT_TOUCH_LONG_PRESS:     return "touch.long_press";
        case EVT_TOUCH_SWIPE:          return "touch.swipe";
        case EVT_TOUCH_RELEASE:        return "touch.release";

        // App
        case EVT_APP_SWITCH:           return "app.switch";
        case EVT_APP_NOTIFICATION:     return "app.notification";
        case EVT_APP_FOREGROUND:       return "app.foreground";
        case EVT_APP_BACKGROUND:       return "app.background";

        // OTA
        case EVT_OTA_AVAILABLE:        return "ota.available";
        case EVT_OTA_PROGRESS:         return "ota.progress";
        case EVT_OTA_COMPLETE:         return "ota.complete";
        case EVT_OTA_FAILED:           return "ota.failed";

        // Power
        case EVT_POWER_LOW_BATTERY:    return "power.low_battery";
        case EVT_POWER_CHARGING:       return "power.charging";
        case EVT_POWER_USB_CONNECT:    return "power.usb_connect";
        case EVT_POWER_USB_DISCONNECT: return "power.usb_disconnect";

        // Settings
        case EVT_SETTINGS_CHANGED:     return "settings.changed";
        case EVT_SETTINGS_RESET:       return "settings.reset";

        default:                       return "unknown";
    }
}
