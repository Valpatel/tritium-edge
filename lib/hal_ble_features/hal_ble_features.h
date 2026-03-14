// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// BLE Feature Extraction — computes a compact feature vector from BLE scan
// results for SC-side ML classification without sending raw advertisement data.
//
// Features extracted per device:
//   - RSSI histogram (3 bins: near/mid/far based on history)
//   - Timing pattern (inter-sighting interval mean/variance)
//   - Name hash (FNV-1a of device name, normalized)
//   - OUI hash (hash of first 3 MAC octets for manufacturer grouping)
//   - Service UUID presence bits (heart_rate, battery, HID, audio, find_my)
//   - Device class code from BLE classifier
//   - Random MAC flag
//   - Advertisement payload length
//
// The feature vector is serialized to JSON for inclusion in MQTT sightings.

#include <cstdint>
#include <cstddef>

#if __has_include("hal_ble_scanner.h")
#include "hal_ble_scanner.h"
#endif

namespace hal_ble_features {

// Number of features in the vector
static constexpr int FEATURE_COUNT = 14;

// Feature vector for a single BLE device
struct BleFeatureVector {
    float features[FEATURE_COUNT];
    // Feature indices:
    //  0: oui_hash         — normalized hash of OUI prefix (0.0 to 1.0)
    //  1: name_length      — device name length
    //  2: name_alpha_ratio — fraction of alpha chars in name
    //  3: name_digit_ratio — fraction of digit chars in name
    //  4: rssi_near_pct    — % of RSSI readings > -50 dBm
    //  5: rssi_mid_pct     — % of RSSI readings -50 to -70 dBm
    //  6: rssi_far_pct     — % of RSSI readings < -70 dBm
    //  7: has_heart_rate   — 1.0 if heart rate UUID detected
    //  8: has_battery      — 1.0 if battery service detected
    //  9: has_hid          — 1.0 if HID service detected
    // 10: has_audio        — 1.0 if audio service detected
    // 11: device_class     — BLE device class enum value (float)
    // 12: is_random_mac    — 1.0 if MAC is locally administered
    // 13: adv_length       — raw advertisement payload length
};

// Feature name strings for JSON serialization (matches SC feature names)
static constexpr const char* FEATURE_NAMES[FEATURE_COUNT] = {
    "oui_hash",
    "name_length",
    "name_alpha_ratio",
    "name_digit_ratio",
    "rssi_near_pct",
    "rssi_mid_pct",
    "rssi_far_pct",
    "has_heart_rate",
    "has_battery",
    "has_hid",
    "has_audio",
    "device_class",
    "is_random_mac",
    "adv_length",
};

// Extract feature vector from a BLE device scan result.
// Returns true if extraction succeeded.
bool extract(const BleDevice& dev, BleFeatureVector& out);

// Serialize a feature vector to JSON object string.
// Format: {"oui_hash":0.42,"name_length":8,...}
// Returns bytes written to buf, or 0 on failure.
int to_json(const BleFeatureVector& fv, char* buf, size_t buf_size);

// Serialize feature vectors for multiple devices to a JSON array.
// Each entry includes mac and features.
// Format: [{"mac":"AA:BB:..","features":{"oui_hash":0.42,...}},...]
// Returns bytes written to buf.
int to_json_array(const BleDevice* devs, int count, char* buf, size_t buf_size);

// Get the feature extraction version number (for compatibility tracking).
int get_version();

// Classification feedback cache — stores SC-provided classification for MACs.
// Max cached entries.
static constexpr int FEEDBACK_CACHE_SIZE = 32;

struct CachedClassification {
    uint8_t addr[6];
    char predicted_type[16];
    float confidence;
    uint32_t received_ms;   // millis() when received
    bool valid;
};

// Store a classification feedback from SC.
void cache_feedback(const uint8_t addr[6], const char* predicted_type, float confidence);

// Look up cached classification for a MAC. Returns nullptr if not cached.
const CachedClassification* get_cached_classification(const uint8_t addr[6]);

// Get count of cached classifications.
int get_feedback_count();

}  // namespace hal_ble_features
