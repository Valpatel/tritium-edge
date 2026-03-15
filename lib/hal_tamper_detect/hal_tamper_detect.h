// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Sensor tamper detection — monitors WiFi and BLE sighting counts.
// If sighting count drops to zero for >5 minutes while the radio is enabled,
// publishes a tamper alert via MQTT. This could indicate RF jamming,
// physical obstruction, or sensor malfunction.

#include <cstdint>
#include <cstddef>

namespace hal_tamper_detect {

enum class TamperType : uint8_t {
    NONE = 0,
    WIFI_SILENCE,    // WiFi scanner active but zero sightings
    BLE_SILENCE,     // BLE scanner active but zero sightings
    BOTH_SILENCE,    // Both WiFi and BLE silent — likely jamming
};

struct TamperAlert {
    TamperType type;
    uint32_t silence_duration_s;  // How long the sensor has been silent
    uint32_t timestamp_epoch;     // When the alert was generated
    bool published;               // Whether MQTT alert was sent
};

struct TamperConfig {
    uint32_t silence_threshold_ms = 300000;  // 5 minutes of silence before alert
    uint32_t check_interval_ms = 30000;      // Check every 30 seconds
    bool alert_on_wifi = true;               // Monitor WiFi scanner
    bool alert_on_ble = true;                // Monitor BLE scanner
};

// Initialize tamper detection. Call once during boot.
bool init(const TamperConfig& config = {});

// Call from main loop. Checks sighting counts and publishes alerts.
// Returns true if a tamper condition was newly detected this tick.
bool tick();

// Get current tamper state.
TamperType get_status();

// Is a tamper alert currently active?
bool is_alert_active();

// Get the current tamper alert details (valid only if is_alert_active()).
const TamperAlert& get_alert();

// Clear the tamper alert (e.g., after operator acknowledges).
void clear_alert();

// Serialize current state to JSON.
int to_json(char* buf, size_t size);

}  // namespace hal_tamper_detect
