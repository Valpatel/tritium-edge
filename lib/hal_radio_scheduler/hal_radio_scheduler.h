// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Radio Scheduler HAL — time-division multiplexing for BLE/WiFi coexistence
//
// The ESP32-S3 cannot run NimBLE and WiFi simultaneously due to shared radio
// and memory constraints. This scheduler alternates between WiFi-active and
// BLE-active time slots, gracefully tearing down one stack before bringing
// up the other.
//
// Usage:
//   hal_radio_scheduler::Config cfg;
//   cfg.wifi_slot_ms = 25000;   // WiFi active for 25s (MQTT heartbeats, OTA)
//   cfg.ble_slot_ms  = 10000;   // BLE scan for 10s
//   hal_radio_scheduler::init(cfg);
//
// The scheduler publishes events via os_events so other services can react
// to radio mode changes (e.g., MQTT bridge pauses during BLE slot).

#include <cstdint>

namespace hal_radio_scheduler {

enum class RadioMode : uint8_t {
    IDLE,           // Neither stack active
    WIFI_ACTIVE,    // WiFi connected, BLE off
    BLE_SCANNING,   // BLE scanning, WiFi off
    TRANSITIONING   // Tearing down one, bringing up other
};

struct Config {
    uint32_t wifi_slot_ms   = 25000;  // WiFi active duration (ms)
    uint32_t ble_slot_ms    = 10000;  // BLE scan duration (ms)
    uint32_t transition_ms  = 2000;   // Time for teardown/startup
    bool     enable_ble     = true;   // Enable BLE slot
    bool     enable_wifi    = true;   // Enable WiFi slot
    bool     wifi_first     = true;   // Start with WiFi slot
};

// Initialize the radio scheduler. Starts background task.
bool init(const Config& config = {});

// Stop the scheduler and return to idle.
void shutdown();

// Get current radio mode.
RadioMode get_mode();

// Get milliseconds remaining in current slot.
uint32_t get_slot_remaining_ms();

// Force switch to a specific mode (overrides scheduler temporarily).
// Returns to scheduled rotation after one slot duration.
void force_mode(RadioMode mode);

// Check if WiFi is currently available for use.
bool is_wifi_available();

// Check if BLE is currently available for use.
bool is_ble_available();

// Is scheduler running?
bool is_active();

// Get JSON status for heartbeat.
int get_status_json(char* buf, size_t buf_size);

}  // namespace hal_radio_scheduler
