/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

// Event categories (high byte of event_id for fast filtering)
enum EventCategory : uint8_t {
    EVT_CAT_SYSTEM   = 0x01,
    EVT_CAT_WIFI     = 0x02,
    EVT_CAT_BLE      = 0x03,
    EVT_CAT_MESH     = 0x04,
    EVT_CAT_DISPLAY  = 0x05,
    EVT_CAT_TOUCH    = 0x06,
    EVT_CAT_APP      = 0x07,
    EVT_CAT_OTA      = 0x08,
    EVT_CAT_POWER    = 0x09,
    EVT_CAT_SETTINGS = 0x0A,
};

// Event IDs (category << 8 | specific)
enum EventId : uint16_t {
    // System
    EVT_SYSTEM_BOOT       = 0x0101,
    EVT_SYSTEM_SLEEP      = 0x0102,
    EVT_SYSTEM_WAKE       = 0x0103,
    EVT_SYSTEM_LOW_MEMORY = 0x0104,
    EVT_SYSTEM_WATCHDOG   = 0x0105,

    // WiFi
    EVT_WIFI_CONNECTED     = 0x0201,
    EVT_WIFI_DISCONNECTED  = 0x0202,
    EVT_WIFI_SCAN_DONE     = 0x0203,
    EVT_WIFI_AP_START      = 0x0204,
    EVT_WIFI_AP_CLIENT     = 0x0205,
    EVT_WIFI_IP_CHANGED    = 0x0206,

    // BLE
    EVT_BLE_DEVICE_FOUND  = 0x0301,
    EVT_BLE_DEVICE_LOST   = 0x0302,
    EVT_BLE_CONNECTED     = 0x0303,
    EVT_BLE_DISCONNECTED  = 0x0304,
    EVT_BLE_DATA_RECEIVED = 0x0305,

    // Mesh
    EVT_MESH_PEER_JOINED  = 0x0401,
    EVT_MESH_PEER_LEFT    = 0x0402,
    EVT_MESH_MESSAGE      = 0x0403,
    EVT_MESH_TOPOLOGY     = 0x0404,

    // Display
    EVT_DISPLAY_READY     = 0x0501,
    EVT_DISPLAY_SLEEP     = 0x0502,
    EVT_DISPLAY_WAKE      = 0x0503,

    // Touch
    EVT_TOUCH_TAP         = 0x0601,
    EVT_TOUCH_LONG_PRESS  = 0x0602,
    EVT_TOUCH_SWIPE       = 0x0603,
    EVT_TOUCH_RELEASE     = 0x0604,

    // App
    EVT_APP_SWITCH        = 0x0701,
    EVT_APP_NOTIFICATION  = 0x0702,
    EVT_APP_FOREGROUND    = 0x0703,
    EVT_APP_BACKGROUND    = 0x0704,

    // OTA
    EVT_OTA_AVAILABLE     = 0x0801,
    EVT_OTA_PROGRESS      = 0x0802,
    EVT_OTA_COMPLETE      = 0x0803,
    EVT_OTA_FAILED        = 0x0804,

    // Power
    EVT_POWER_LOW_BATTERY    = 0x0901,
    EVT_POWER_CHARGING       = 0x0902,
    EVT_POWER_USB_CONNECT    = 0x0903,
    EVT_POWER_USB_DISCONNECT = 0x0904,

    // Settings
    EVT_SETTINGS_CHANGED  = 0x0A01,
    EVT_SETTINGS_RESET    = 0x0A02,
};

// Event data (up to 32 bytes inline, avoids heap allocation)
struct TritiumEvent {
    EventId id;
    uint32_t timestamp_ms;     // millis() at publish time
    uint8_t data_len;
    uint8_t data[32];          // Inline payload (no malloc)

    // Convenience getters
    int32_t asInt() const;
    float asFloat() const;
    const char* asString() const;
    EventCategory category() const { return (EventCategory)(id >> 8); }
};

// Callback types
typedef void (*EventCallback)(const TritiumEvent& event, void* user_data);

class TritiumEventBus {
public:
    static TritiumEventBus& instance();

    // Publish (thread-safe, ISR-safe for simple events)
    void publish(EventId id);
    void publish(EventId id, int32_t value);
    void publish(EventId id, float value);
    void publish(EventId id, const char* str);
    void publish(EventId id, const void* data, uint8_t len);
    void publish(const TritiumEvent& event);

    // Subscribe (returns subscriber_id for unsubscribe)
    int subscribe(EventId id, EventCallback cb, void* user_data = nullptr);
    int subscribeCategory(EventCategory cat, EventCallback cb, void* user_data = nullptr);
    int subscribeAll(EventCallback cb, void* user_data = nullptr);
    void unsubscribe(int subscriber_id);

    // Event history (ring buffer)
    const TritiumEvent* getHistory(int* count);  // Returns pointer to ring buffer

    // Stats
    uint32_t totalPublished() const;
    uint32_t totalDropped() const;  // Events with no subscribers

    // Process queued events (call from main loop or dedicated task)
    void processQueue();

private:
    TritiumEventBus();
};

// Human-readable event name (defined in event_names.cpp)
const char* eventIdToName(EventId id);
