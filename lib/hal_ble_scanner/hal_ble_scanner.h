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

// RSSI history depth per device (circular buffer)
static constexpr int BLE_RSSI_HISTORY_SIZE = 30;

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

// RSSI reading with timestamp for trend analysis
struct RssiReading {
    int8_t rssi;            // RSSI value in dBm
    uint32_t timestamp;     // millis() when recorded
};

// MAC randomization correlation — groups devices that rotate their MAC.
// When a device rotates, the old MAC disappears and a new one appears
// within seconds at a similar RSSI.  We link them via a correlation group ID.
static constexpr int MAC_ROTATION_MAX_GROUPS = 16;
static constexpr uint32_t MAC_ROTATION_WINDOW_MS = 5000;  // 5 seconds
static constexpr int8_t MAC_ROTATION_RSSI_TOLERANCE = 10; // dBm

// Maximum raw advertisement payload size (BLE spec max = 31 bytes, extended = 255)
static constexpr int BLE_RAW_ADV_MAX_LEN = 62;

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
    char manufacturer[16];          // Manufacturer name (Apple, Samsung, Google, Microsoft, Fitbit)
    bool is_random_mac;             // true if locally administered bit is set
    int8_t rotation_group;          // -1 = ungrouped, >= 0 = correlation group ID

    // Raw advertisement payload for SC-side deep parsing
    uint8_t raw_adv[BLE_RAW_ADV_MAX_LEN];
    uint8_t raw_adv_len;            // Bytes used in raw_adv (0 = not captured)

    // Stable fingerprint hash of advertisement payload (FNV-1a, excludes RSSI/timestamp)
    // Two scans of the same device produce the same hash for dedup and MAC rotation tracking
    uint32_t adv_hash;              // 0 = not computed

    // RSSI history — circular buffer of last N readings for trend analysis
    RssiReading rssi_history[BLE_RSSI_HISTORY_SIZE];
    uint8_t rssi_history_head;      // Next write position
    uint8_t rssi_history_count;     // Entries used (0..BLE_RSSI_HISTORY_SIZE)
    int8_t rssi_min;                // Minimum RSSI ever recorded
    int8_t rssi_max;                // Maximum RSSI ever recorded
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
    uint32_t cache_ttl_ms = 30000;    // How long cached results stay valid (30s default)
    uint8_t batch_size = 10;          // Devices per batch in batch JSON output

    // Burst mode — works with hal_radio_scheduler for WiFi/BLE time-division.
    // When enabled, scanner does a quick active scan burst then yields the radio.
    bool burst_mode = false;              // Enable burst scan mode
    uint32_t burst_scan_ms = 5000;        // Burst scan duration (default 5s)
    uint32_t burst_interval_ms = 30000;   // Time between bursts (default 30s)
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

// Get JSON array of devices in batches for MQTT publishing.
// Returns up to batch_size devices per call. Call repeatedly with offset.
// offset: start index into visible device list. Returns 0 when no more.
int get_devices_json_batch(char* buf, size_t buf_size, int offset, int batch_size = 0);

// Get count of batches needed to send all visible devices.
// Uses configured batch_size or override.
int get_batch_count(int batch_size = 0);

// Check if cached scan results are still valid (within cache_ttl_ms).
// Returns true if results are fresh, false if stale.
bool is_cache_valid();

// Get milliseconds since last scan completed.
uint32_t cache_age_ms();

// Get RSSI history for a specific device as JSON.
// Format: {"mac":"AA:BB:CC:DD:EE:FF","count":N,"min":-80,"max":-40,
//          "readings":[{"rssi":-50,"age_ms":1234},...],"trend":"approaching"|"departing"|"stable"}
// Returns bytes written to buf, or 0 if device not found.
int get_rssi_history_json(const uint8_t addr[6], char* buf, size_t buf_size);

// Get RSSI history for a device by MAC string "AA:BB:CC:DD:EE:FF".
// Convenience wrapper around get_rssi_history_json.
int get_rssi_history_json_by_mac(const char* mac_str, char* buf, size_t buf_size);

// Check if a MAC address is locally administered (randomized).
// Bit 1 of the first octet indicates locally administered.
inline bool is_locally_administered(const uint8_t addr[6]) {
    return (addr[0] & 0x02) != 0;
}

// Get the count of MAC rotation correlation groups detected.
int get_rotation_group_count();

// Get extended JSON for a device including raw advertisement payload (base64).
// Returns bytes written to buf, or 0 if device not found.
int get_device_extended_json(const uint8_t addr[6], char* buf, size_t buf_size);

// --- Scan statistics ---

struct ScanStats {
    uint32_t total_scans;           // Total scan cycles completed
    uint32_t total_unique_devices;  // Unique MACs ever seen this session
    uint32_t total_devices_found;   // Sum of devices found per scan (for averaging)
    uint32_t scan_failures;         // Scans that returned 0 devices or failed
    uint32_t last_scan_devices;     // Devices found in most recent scan
    uint32_t last_scan_duration_ms; // Duration of most recent scan in ms

    float avg_devices_per_scan() const {
        return total_scans > 0 ? (float)total_devices_found / total_scans : 0.0f;
    }
    float success_rate() const {
        return total_scans > 0 ? (float)(total_scans - scan_failures) / total_scans : 0.0f;
    }
};

// Get current scan statistics.
ScanStats get_scan_stats();

// Get scan statistics as JSON for heartbeat.
// Format: {"total_scans":N,"unique_devices":N,"avg_devices":1.5,"success_rate":0.95,...}
// Returns bytes written to buf.
int get_scan_stats_json(char* buf, size_t buf_size);

// Is scanner running?
bool is_active();

// --- Burst mode API (for radio scheduler integration) ---

// Trigger an immediate burst scan. If burst_mode is enabled in config,
// the scanner does a quick active scan for burst_scan_ms then stops,
// leaving the radio free for WiFi. Call this from the radio scheduler
// when a BLE time slot starts.
//
// Returns true if the burst was started, false if scanner is not initialized
// or a burst is already in progress.
bool start_burst();

// Check if a burst scan is currently in progress.
bool is_burst_active();

// Get milliseconds remaining in the current burst, or 0 if no burst active.
uint32_t burst_remaining_ms();

// Get number of completed burst cycles since init.
uint32_t get_burst_count();

}  // namespace hal_ble_scanner
