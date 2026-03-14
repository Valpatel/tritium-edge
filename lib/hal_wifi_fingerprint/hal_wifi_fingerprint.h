// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#pragma once

// WiFi RSSI Fingerprint Collection -- indoor positioning support
//
// Walk through a building, stop at known positions, record WiFi RSSI
// from all visible access points.  The collected fingerprints are
// published via MQTT for the command center to build an indoor
// positioning database.
//
// Workflow:
//   1. Start collection mode (set_collecting(true))
//   2. Walk to a known position in the building
//   3. Call record_fingerprint(lat, lon, floor, room_id) to capture
//   4. The device scans all visible APs, records BSSID/RSSI pairs
//   5. Fingerprint is published via MQTT to tritium/{device}/wifi_fingerprint
//   6. Repeat at different positions
//   7. Stop collection mode
//
// The command center uses these fingerprints for WiFi-based indoor
// positioning when BLE trilateration is insufficient.

#include <cstdint>
#include <cstddef>

namespace hal_wifi_fingerprint {

static constexpr int FP_MAX_APS = 32;          // Max APs per fingerprint
static constexpr int FP_MAX_SAMPLES = 3;       // Scans to average per point
static constexpr int FP_MAX_STORED = 100;      // Max fingerprints stored locally

struct APReading {
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
};

struct Fingerprint {
    uint32_t id;                    // Auto-incrementing ID
    double lat;                     // Known position latitude
    double lon;                     // Known position longitude
    int8_t floor_level;             // Floor number
    char room_id[33];               // Optional room identifier
    char plan_id[33];               // Optional floor plan ID
    APReading aps[FP_MAX_APS];      // Visible APs and their RSSI
    uint8_t ap_count;               // Number of APs recorded
    uint32_t timestamp;             // millis() when captured
    bool published;                 // Has this been sent via MQTT?
};

struct FingerprintConfig {
    bool enabled = false;           // Collection mode is off by default
    uint8_t scan_count = FP_MAX_SAMPLES;  // Number of scans to average
    uint32_t scan_delay_ms = 500;   // Delay between averaged scans
};

// Initialize fingerprint collection (call after WiFi is connected)
bool init(const FingerprintConfig& config = FingerprintConfig());
void shutdown();

// Collection mode control
void set_collecting(bool active);
bool is_collecting();

// Record a fingerprint at the current position
// Performs multiple WiFi scans, averages RSSI, stores result
// Returns fingerprint ID on success, 0 on failure
uint32_t record_fingerprint(double lat, double lon, int8_t floor_level,
                            const char* room_id = nullptr,
                            const char* plan_id = nullptr);

// Get stored fingerprints
int get_fingerprints(Fingerprint* out, int max_count);
int get_stored_count();
int get_unpublished_count();

// Mark fingerprints as published (after MQTT send)
void mark_published(uint32_t id);
void mark_all_published();

// Clear all stored fingerprints
void clear();

// JSON output for MQTT publishing
// Generates JSON for a single fingerprint
int get_fingerprint_json(const Fingerprint& fp, char* buf, size_t size);

// Generate batch JSON of all unpublished fingerprints
int get_unpublished_json(char* buf, size_t size);

}  // namespace hal_wifi_fingerprint
