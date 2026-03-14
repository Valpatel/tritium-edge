// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// RF Motion Monitor — tracks RSSI variance between Tritium peer nodes
// to detect physical motion in the RF environment. When a person or object
// moves between two nodes, the RSSI between them fluctuates due to multipath
// interference. High variance = motion detected.
//
// Data source: ESP-NOW mesh peer RSSI values (already tracked by EspNowHAL).
// Output: JSON array suitable for MQTT sighting topic and SC RF motion plugin.

#include <cstdint>
#include <cstddef>

namespace hal_rf_monitor {

static constexpr int MAX_PEERS = 16;
static constexpr int WINDOW_SIZE = 60;      // Sliding window of RSSI samples
static constexpr float DEFAULT_MOTION_THRESHOLD = 5.0f;  // dBm variance

// Peer info provided by the caller (decouples from EspNowHAL)
struct RFPeer {
    uint8_t mac[6];
    int8_t  rssi;
    bool    is_direct;      // Only direct (1-hop) peers are tracked
};

// Callback type: fills peers array, returns count
typedef int (*PeerProvider)(RFPeer* peers, int max_peers);

struct RFMonitorConfig {
    uint32_t sample_interval_ms = 1000;     // How often to sample peer RSSI
    float    motion_threshold = DEFAULT_MOTION_THRESHOLD;
    PeerProvider peer_provider = nullptr;   // Required: provides current peer list
};

struct PeerRFStats {
    uint8_t  mac[6];
    int8_t   rssi;              // Most recent RSSI reading
    float    variance;          // RSSI variance over sliding window
    bool     motion_detected;   // variance > threshold
    uint16_t sample_count;      // How many samples collected so far
};

// Initialize the RF monitor. Call after ESP-NOW is initialized.
bool init(const RFMonitorConfig& config = {});

// Call from main loop. Samples peer RSSI at configured interval.
void tick();

// Check if monitor is active.
bool is_active();

// Get number of tracked peers with RF data.
int get_peer_count();

// Get RF stats for all tracked peers.
// Returns number of peers written to output array.
int get_peer_stats(PeerRFStats* out, int max_count);

// Serialize peer RF data to JSON array:
// [{"peer_id":"AA:BB:CC:DD:EE:FF","rssi":-45,"variance":3.2,"motion":false}, ...]
int get_peer_rssi_json(char* buf, size_t size);

// Get a summary line for heartbeat: {"rf_peers":N,"motion_peers":N}
int get_summary_json(char* buf, size_t size);

// Update motion threshold at runtime.
void set_motion_threshold(float threshold);

// Get current motion threshold.
float get_motion_threshold();

}  // namespace hal_rf_monitor
