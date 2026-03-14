// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

#include <cstdint>
#include <cstddef>

// Maximum tracked BLE devices
static constexpr int BLE_SCANNER_MAX_DEVICES = 64;

// How long before a device is considered "gone" (ms)
static constexpr uint32_t BLE_DEVICE_TIMEOUT_MS = 120000;  // 2 minutes

// Apple Continuity device types (from manufacturer-specific data, company 0x004C)
enum class AppleDeviceType : uint8_t {
    NONE        = 0x00,     // Not an Apple device / not parsed
    IPHONE      = 0x02,     // iPhone
    IPAD        = 0x06,     // iPad
    WATCH       = 0x0E,     // Apple Watch
    MACBOOK     = 0x03,     // MacBook
    AIRPODS     = 0x0A,     // AirPods / AirPods Pro / AirPods Max
    HOMEPOD     = 0x09,     // HomePod / HomePod mini
    APPLE_TV    = 0x05,     // Apple TV
    PENCIL      = 0x0F,     // Apple Pencil
    AIRTAG      = 0x12,     // AirTag (Find My)
    UNKNOWN_APPLE = 0xFF,   // Apple device but unknown subtype
};

// BLE device type classification (vendor-independent)
enum class BleDeviceClass : uint8_t {
    UNKNOWN = 0,
    PHONE,
    WATCH,
    TABLET,
    LAPTOP,
    HEADPHONES,
    SPEAKER,
    TV_DONGLE,
    TRACKER,        // AirTag, Tile, etc.
    IOT_DEVICE,
    BEACON,
    MEDICAL,
    FITNESS,
    PERIPHERAL,     // keyboard, mouse, gamepad
};

struct BleDevice {
    uint8_t addr[6];        // MAC address
    int8_t rssi;            // Last RSSI
    uint8_t addr_type;      // 0=public, 1=random
    uint32_t first_seen;    // millis() when first detected
    uint32_t last_seen;     // millis() when last seen
    uint16_t seen_count;    // How many times seen this session
    char name[32];          // Local name (if advertised)
    bool is_known;          // Matches a known device list
    AppleDeviceType apple_type;     // Apple Continuity device type (if detected)
    BleDeviceClass device_class;    // Classified device type
    char device_type[16];           // Human-readable device type string
};

struct BleDeviceMatch {
    uint8_t addr[6];
    char label[32];         // Human-readable name ("Matt's Watch", etc.)
};

namespace hal_ble_scanner {

struct ScanConfig {
    uint16_t scan_window_ms = 100;    // Active scan window
    uint16_t scan_interval_ms = 200;  // Scan interval
    uint32_t scan_duration_s = 5;     // Duration per scan cycle
    uint32_t pause_between_ms = 5000; // Pause between scans
    bool active_scan = false;         // false=passive (less intrusive)
};

// Initialize BLE scanner. Starts background scanning task.
bool init(const ScanConfig& config = {});

// Stop scanning and free resources.
void shutdown();

// Register known devices for identification.
// When a known device is seen, is_known flag is set.
void add_known_device(const uint8_t addr[6], const char* label);

// Get snapshot of currently tracked devices.
// Returns number of devices copied to out[].
int get_devices(BleDevice* out, int max_count);

// Get count of currently visible devices (seen within timeout).
int get_visible_count();

// Get count of known devices currently visible.
int get_known_visible_count();

// Check if a specific MAC is currently visible.
bool is_device_visible(const uint8_t addr[6]);

// Get JSON-formatted summary for heartbeat payload.
// Returns bytes written to buf.
int get_summary_json(char* buf, size_t buf_size);

// Get JSON array of visible devices for detailed heartbeat.
// Format: [{"mac":"AA:BB:CC:DD:EE:FF","rssi":-50,"name":"...","known":true},...]
// Returns bytes written to buf.
int get_devices_json(char* buf, size_t buf_size);

// Is scanner running?
bool is_active();

}  // namespace hal_ble_scanner
