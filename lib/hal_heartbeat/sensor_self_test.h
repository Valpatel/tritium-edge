// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Sensor self-test — runs periodic diagnostics and includes results in heartbeat.
// Checks WiFi scan ability, BLE scan success, heap health, and uptime.
// Runs every hour (configurable) and caches results for heartbeat inclusion.

#include <cstdint>

namespace sensor_self_test {

struct SelfTestResult {
    bool wifi_scan_ok;       // WiFi scan returned >0 networks
    int wifi_scan_count;     // Number of networks found
    bool ble_scan_ok;        // BLE scan completed successfully (or N/A if disabled)
    bool heap_ok;            // Free heap > minimum threshold
    uint32_t free_heap;      // Free heap bytes at test time
    uint32_t min_free_heap;  // Minimum free heap since boot
    uint32_t largest_block;  // Largest contiguous free block
    float fragmentation_pct; // Heap fragmentation: (1 - largest/free) * 100
    bool ntp_sync_ok;        // NTP time is synchronized (year > 2024)
    uint32_t ntp_age_s;      // Seconds since last NTP sync (0 = unknown)
    bool overall_pass;       // All tests passed
    uint32_t last_run_ms;    // millis() when test last ran
    uint32_t run_count;      // Number of times self-test has run
};

// Initialize self-test system. Call once during setup.
void init(uint32_t interval_ms = 3600000);  // Default: 1 hour

// Call from loop() or heartbeat tick. Runs self-test if interval elapsed.
// Non-blocking — returns immediately if not time yet.
// Returns true if a self-test just ran.
bool tick();

// Force run self-test now (ignores interval timer).
void run_now();

// Get the latest self-test result.
const SelfTestResult& get_result();

// Write self-test results as JSON into buffer. Returns bytes written.
int to_json(char* buf, size_t buf_size);

}  // namespace sensor_self_test
