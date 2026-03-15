// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Autonomous Scan Optimizer — adapts BLE/WiFi scan intervals to local activity
//
// Monitors sighting rates over a sliding window and automatically adjusts
// scan frequency without requiring commands from the command center:
//
//   - Quiet area (low sighting rate)  -> reduce scan frequency to save power
//   - Busy area (high sighting rate)  -> increase scan frequency for coverage
//   - Spike detected (sudden increase) -> burst mode for rapid characterization
//
// The optimizer respects power-saver overrides: if PowerSaver is active,
// scan intervals are clamped to the power-saver minimums.
//
// Usage:
//   hal_scan_optimizer::Config cfg;
//   hal_scan_optimizer::init(cfg);
//   // Call tick() from main loop or heartbeat task every ~1 second
//   hal_scan_optimizer::tick();
//
// The optimizer publishes its decisions via JSON in the heartbeat payload
// so the command center can see what the edge is doing autonomously.

#include <cstdint>
#include <cstddef>

namespace hal_scan_optimizer {

// Activity classification
enum class ActivityLevel : uint8_t {
    QUIET,      // Very few sightings — long scan intervals
    NORMAL,     // Typical sighting rate — default intervals
    BUSY,       // Above-average sightings — shorter intervals
    SPIKE,      // Sudden increase detected — burst scan mode
};

// Configuration
struct Config {
    // Sliding window for rate calculation (seconds)
    uint32_t window_seconds = 60;

    // Sighting rate thresholds (sightings per minute)
    float quiet_threshold = 2.0f;     // Below this = quiet
    float busy_threshold = 15.0f;     // Above this = busy
    float spike_threshold = 30.0f;    // Above this = spike

    // Spike detection: rate increase factor over previous window
    float spike_ratio = 3.0f;

    // BLE scan intervals for each activity level (ms)
    uint32_t ble_interval_quiet_ms  = 30000;   // 30s in quiet areas
    uint32_t ble_interval_normal_ms = 10000;   // 10s normal
    uint32_t ble_interval_busy_ms   = 5000;    // 5s busy
    uint32_t ble_interval_spike_ms  = 2000;    // 2s spike (burst)

    // WiFi scan intervals for each activity level (ms)
    uint32_t wifi_interval_quiet_ms  = 120000;  // 2min quiet
    uint32_t wifi_interval_normal_ms = 30000;   // 30s normal
    uint32_t wifi_interval_busy_ms   = 15000;   // 15s busy
    uint32_t wifi_interval_spike_ms  = 5000;    // 5s spike

    // Minimum time at each level before transitioning (prevent oscillation)
    uint32_t level_hold_ms = 10000;  // Hold level for at least 10s

    // Spike cooldown: don't re-enter spike mode for N seconds after spike ends
    uint32_t spike_cooldown_ms = 30000;

    // Enable autonomous adjustment (false = report only, don't change intervals)
    bool enabled = true;
};

// Sighting rate statistics
struct ScanStats {
    float ble_rate_per_min;      // BLE sightings per minute (current window)
    float wifi_rate_per_min;     // WiFi sightings per minute (current window)
    float combined_rate;         // Combined sighting rate
    float prev_combined_rate;    // Previous window rate (for spike detection)
    uint32_t ble_total;          // Total BLE sightings since boot
    uint32_t wifi_total;         // Total WiFi sightings since boot
    uint32_t window_ble_count;   // BLE sightings in current window
    uint32_t window_wifi_count;  // WiFi sightings in current window
};

// Initialize the scan optimizer. Does not start scanning itself —
// it monitors counters from hal_ble_scanner and hal_wifi_scanner.
bool init(const Config& config = Config());

// Shutdown and reset.
void shutdown();

// Call every ~1 second from main loop or heartbeat task.
// Reads current sighting counts, computes rates, adjusts intervals.
void tick();

// Record a BLE sighting. Call this from the BLE scanner callback
// or whenever a new BLE device is detected.
void record_ble_sighting();

// Record a WiFi sighting. Call from WiFi scanner callback.
void record_wifi_sighting();

// Record multiple sightings at once (e.g., after a batch scan completes).
void record_ble_sightings(uint32_t count);
void record_wifi_sightings(uint32_t count);

// Get current activity level.
ActivityLevel get_activity_level();

// Get current recommended BLE scan interval (ms).
uint32_t get_ble_interval_ms();

// Get current recommended WiFi scan interval (ms).
uint32_t get_wifi_interval_ms();

// Get sighting rate statistics.
ScanStats get_stats();

// Is optimizer active?
bool is_active();

// Get JSON status for heartbeat payload.
// Format: {"optimizer":{"level":"normal","ble_rate":5.2,"wifi_rate":1.3,
//          "ble_interval_ms":10000,"wifi_interval_ms":30000,"adjustments":12}}
int get_status_json(char* buf, size_t buf_size);

// Get human-readable activity level string.
const char* activity_level_str(ActivityLevel level);

}  // namespace hal_scan_optimizer
