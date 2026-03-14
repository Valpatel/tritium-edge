// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

#include <cstdint>
#include <cstddef>

// Maximum number of GATT services discovered per device
static constexpr int BLE_INTERROGATOR_MAX_SERVICES = 16;

// Maximum number of characteristics per service
static constexpr int BLE_INTERROGATOR_MAX_CHARS = 8;

// Maximum interrogation queue depth
static constexpr int BLE_INTERROGATOR_QUEUE_SIZE = 16;

// Don't re-interrogate same MAC within this window (ms) — 1 hour
static constexpr uint32_t BLE_INTERROGATOR_COOLDOWN_MS = 3600000;

// Maximum connection time for interrogation (ms)
static constexpr uint32_t BLE_INTERROGATOR_TIMEOUT_MS = 2000;

// Standard BLE 16-bit service UUIDs
static constexpr uint16_t BLE_SVC_GAP                 = 0x1800;
static constexpr uint16_t BLE_SVC_GATT                = 0x1801;
static constexpr uint16_t BLE_SVC_DEVICE_INFO         = 0x180A;
static constexpr uint16_t BLE_SVC_BATTERY              = 0x180F;
static constexpr uint16_t BLE_SVC_HEART_RATE           = 0x180D;
static constexpr uint16_t BLE_SVC_BLOOD_PRESSURE       = 0x1810;
static constexpr uint16_t BLE_SVC_HEALTH_THERMO        = 0x1809;
static constexpr uint16_t BLE_SVC_GLUCOSE              = 0x1808;
static constexpr uint16_t BLE_SVC_RUNNING_SPEED        = 0x1814;
static constexpr uint16_t BLE_SVC_CYCLING_SPEED        = 0x1816;
static constexpr uint16_t BLE_SVC_CYCLING_POWER        = 0x1818;
static constexpr uint16_t BLE_SVC_HID                  = 0x1812;
static constexpr uint16_t BLE_SVC_SCAN_PARAMS          = 0x1813;
static constexpr uint16_t BLE_SVC_TX_POWER             = 0x1804;
static constexpr uint16_t BLE_SVC_CURRENT_TIME         = 0x1805;
static constexpr uint16_t BLE_SVC_LOCATION_NAV         = 0x1819;
static constexpr uint16_t BLE_SVC_ENVIRONMENTAL        = 0x181A;
static constexpr uint16_t BLE_SVC_AUTOMATION_IO         = 0x1815;
static constexpr uint16_t BLE_SVC_USER_DATA            = 0x181C;
static constexpr uint16_t BLE_SVC_WEIGHT_SCALE         = 0x181D;
static constexpr uint16_t BLE_SVC_BOND_MGMT            = 0x181E;
static constexpr uint16_t BLE_SVC_MESH_PROVISIONING    = 0x1827;
static constexpr uint16_t BLE_SVC_MESH_PROXY           = 0x1828;
static constexpr uint16_t BLE_SVC_IMMEDIATE_ALERT      = 0x1802;
static constexpr uint16_t BLE_SVC_LINK_LOSS            = 0x1803;
static constexpr uint16_t BLE_SVC_FIND_ME              = 0x1802;

// Device Information Service characteristic UUIDs (16-bit)
static constexpr uint16_t BLE_CHAR_MANUFACTURER_NAME   = 0x2A29;
static constexpr uint16_t BLE_CHAR_MODEL_NUMBER        = 0x2A24;
static constexpr uint16_t BLE_CHAR_SERIAL_NUMBER       = 0x2A25;
static constexpr uint16_t BLE_CHAR_HARDWARE_REV        = 0x2A27;
static constexpr uint16_t BLE_CHAR_FIRMWARE_REV        = 0x2A26;
static constexpr uint16_t BLE_CHAR_SOFTWARE_REV        = 0x2A28;
static constexpr uint16_t BLE_CHAR_SYSTEM_ID           = 0x2A23;
static constexpr uint16_t BLE_CHAR_PNP_ID              = 0x2A50;

// GAP characteristic UUIDs
static constexpr uint16_t BLE_CHAR_DEVICE_NAME         = 0x2A00;
static constexpr uint16_t BLE_CHAR_APPEARANCE          = 0x2A01;

// Battery Service characteristic UUIDs
static constexpr uint16_t BLE_CHAR_BATTERY_LEVEL       = 0x2A19;


// A single discovered GATT service
struct BleGattService {
    uint16_t uuid16;                 // 16-bit UUID (0 if 128-bit)
    uint8_t uuid128[16];             // 128-bit UUID (zeroed if 16-bit)
    bool is_standard;                // true if recognized 16-bit UUID
    char name[32];                   // Human-readable name
};

// Complete device profile from GATT interrogation
struct BleDeviceProfile {
    uint8_t addr[6];                 // MAC address
    uint8_t addr_type;               // 0=public, 1=random

    // Discovered services
    BleGattService services[BLE_INTERROGATOR_MAX_SERVICES];
    uint8_t service_count;

    // Device Information Service (0x180A)
    char manufacturer[48];           // Manufacturer Name String
    char model[48];                  // Model Number String
    char firmware_rev[32];           // Firmware Revision String
    char hardware_rev[32];           // Hardware Revision String
    char software_rev[32];           // Software Revision String
    char serial_number[32];          // Serial Number String

    // GAP (0x1800)
    char device_name[48];            // Device Name
    uint16_t appearance;             // GAP Appearance value

    // Battery Service (0x180F)
    int8_t battery_level;            // 0-100, or -1 if not available

    // Interrogation metadata
    uint32_t interrogated_at;        // millis() timestamp
    uint16_t connection_duration_ms; // How long the connection took
    bool success;                    // true if connection + read succeeded
    char error[64];                  // Error message if !success
};

// Interrogation result for MQTT publishing
struct BleInterrogationResult {
    BleDeviceProfile profile;
    bool success;
    uint16_t duration_ms;
};


namespace hal_ble_interrogator {

struct InterrogatorConfig {
    uint32_t cooldown_ms = BLE_INTERROGATOR_COOLDOWN_MS;
    uint32_t timeout_ms = BLE_INTERROGATOR_TIMEOUT_MS;
    uint8_t queue_size = BLE_INTERROGATOR_QUEUE_SIZE;
    bool auto_interrogate_unknown = true;  // Auto-queue unknown devices from scanner
};

// Initialize the BLE interrogator. Starts background task.
bool init(const InterrogatorConfig& config = {});

// Stop interrogator and free resources.
void shutdown();

// Queue a device for interrogation by MAC address.
// Returns true if queued, false if queue full or device on cooldown.
bool queue_interrogation(const uint8_t addr[6], uint8_t addr_type = 1);

// Queue a device for interrogation by MAC string "AA:BB:CC:DD:EE:FF".
bool queue_interrogation_by_mac(const char* mac_str, uint8_t addr_type = 1);

// Get the most recent interrogation result for a MAC address.
// Returns true if a result exists, false otherwise.
bool get_result(const uint8_t addr[6], BleDeviceProfile* out);

// Get result by MAC string.
bool get_result_by_mac(const char* mac_str, BleDeviceProfile* out);

// Get the number of completed interrogations.
int get_completed_count();

// Get the number of pending interrogations in the queue.
int get_pending_count();

// Check if a MAC is on cooldown (recently interrogated).
bool is_on_cooldown(const uint8_t addr[6]);

// Get JSON representation of a device profile.
// Returns bytes written to buf.
int profile_to_json(const BleDeviceProfile* profile, char* buf, size_t buf_size);

// Get JSON summary of interrogator status.
int get_status_json(char* buf, size_t buf_size);

// Get JSON array of all completed profiles.
int get_all_profiles_json(char* buf, size_t buf_size);

// Clear all cached results and cooldowns.
void clear_cache();

// Is interrogator running?
bool is_active();

// Get human-readable name for a 16-bit service UUID.
const char* service_uuid_name(uint16_t uuid16);

}  // namespace hal_ble_interrogator
