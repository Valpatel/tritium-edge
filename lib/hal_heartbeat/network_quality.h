// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

#include <cstdint>

namespace network_quality {

struct NetworkStats {
    float avg_latency_ms;       // Average MQTT broker ping latency
    float max_latency_ms;       // Peak latency in measurement window
    float min_latency_ms;       // Minimum latency in measurement window
    float packet_loss_pct;      // Packet loss percentage (0-100)
    uint32_t reconnect_count;   // WiFi reconnection count since boot
    uint32_t dns_failures;      // DNS resolution failures
    int32_t rssi;               // Current WiFi RSSI
    uint32_t uptime_ms;         // Time since last WiFi connection established
    bool stable;                // Connection considered stable (loss < 5%, latency < 500ms)
};

// Initialize network quality monitoring.
// Call once after WiFi + MQTT are ready.
void init();

// Call periodically (e.g., every heartbeat tick) to run measurements.
// Performs a non-blocking TCP connect test to the MQTT broker to measure RTT.
void tick();

// Get current network quality stats.
NetworkStats get_stats();

// Format stats as JSON fragment (no outer braces).
// Returns bytes written.
int to_json(char* buf, int buf_size);

}  // namespace network_quality
