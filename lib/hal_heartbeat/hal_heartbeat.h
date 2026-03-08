// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

#include <cstdint>

namespace hal_heartbeat {

struct HeartbeatConfig {
    const char* server_url = nullptr;  // e.g., "http://192.168.1.100:8080"
    const char* device_id = nullptr;   // e.g., "esp32-aabbccddeeff"
    uint32_t interval_ms = 60000;      // Default 60s
    bool cot_enabled = false;          // Also send CoT position on each heartbeat tick
};

// Initialize heartbeat system. Call once after WiFi is connected.
// If server_url is null, tries to load from NVS (hal_provision).
// Returns true if configured and ready to send.
bool init(const HeartbeatConfig& config = {});

// Call from loop(). Sends heartbeat if interval has elapsed.
// Non-blocking — returns immediately if not time yet.
// Returns true if a heartbeat was successfully sent.
bool tick();

// Force send a heartbeat now (ignores interval timer).
bool send_now();

// Check if heartbeat is configured and active.
bool is_active();

// Get the server-recommended interval (may differ from config).
uint32_t get_interval_ms();

}  // namespace hal_heartbeat
