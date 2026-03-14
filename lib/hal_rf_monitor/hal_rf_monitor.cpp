// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_rf_monitor.h"
#include "debug_log.h"

#include <cstring>
#include <cstdio>
#include <cmath>

static constexpr const char* TAG = "rf_mon";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace hal_rf_monitor {

bool init(const RFMonitorConfig&) { return false; }
void tick() {}
bool is_active() { return false; }
int get_peer_count() { return 0; }
int get_peer_stats(PeerRFStats*, int) { return 0; }
int get_peer_rssi_json(char* buf, size_t size) {
    if (size > 2) { buf[0] = '['; buf[1] = ']'; buf[2] = '\0'; return 2; }
    return 0;
}
int get_summary_json(char* buf, size_t size) {
    return snprintf(buf, size, "{\"rf_peers\":0,\"motion_peers\":0}");
}
void set_motion_threshold(float) {}
float get_motion_threshold() { return DEFAULT_MOTION_THRESHOLD; }

}  // namespace hal_rf_monitor

// ============================================================================
// ESP32 — real RF motion monitor using peer RSSI callback
// ============================================================================
#else

#include <Arduino.h>

namespace hal_rf_monitor {

// Per-peer RSSI history ring buffer
struct PeerTracker {
    uint8_t  mac[6];
    int8_t   rssi_history[WINDOW_SIZE];
    int      write_idx;         // Next write position in ring buffer
    int      sample_count;      // Total samples collected (capped at WINDOW_SIZE for stats)
    bool     active;            // Slot in use
};

// Module state
static bool _initialized = false;
static bool _active = false;
static float _motion_threshold = DEFAULT_MOTION_THRESHOLD;
static uint32_t _sample_interval_ms = 1000;
static uint32_t _last_sample_ms = 0;
static PeerProvider _peer_provider = nullptr;

static PeerTracker _trackers[MAX_PEERS] = {};
static int _tracker_count = 0;

// Find tracker index for a MAC, or -1
static int find_tracker(const uint8_t mac[6]) {
    for (int i = 0; i < _tracker_count; i++) {
        if (_trackers[i].active && memcmp(_trackers[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

// Add or update a tracker for a peer
static int add_tracker(const uint8_t mac[6]) {
    int idx = find_tracker(mac);
    if (idx >= 0) return idx;

    // Find empty slot or expand
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!_trackers[i].active) {
            memcpy(_trackers[i].mac, mac, 6);
            _trackers[i].write_idx = 0;
            _trackers[i].sample_count = 0;
            _trackers[i].active = true;
            memset(_trackers[i].rssi_history, 0, sizeof(_trackers[i].rssi_history));
            if (i >= _tracker_count) _tracker_count = i + 1;
            return i;
        }
    }
    return -1;  // Full
}

// Record an RSSI sample for a peer
static void record_rssi(int idx, int8_t rssi) {
    if (idx < 0 || idx >= MAX_PEERS) return;
    PeerTracker& t = _trackers[idx];
    t.rssi_history[t.write_idx] = rssi;
    t.write_idx = (t.write_idx + 1) % WINDOW_SIZE;
    if (t.sample_count < WINDOW_SIZE) t.sample_count++;
}

// Compute RSSI variance for a peer
static float compute_variance(int idx) {
    if (idx < 0 || idx >= MAX_PEERS) return 0.0f;
    const PeerTracker& t = _trackers[idx];
    int n = t.sample_count;
    if (n < 2) return 0.0f;

    // Compute mean
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += t.rssi_history[i];
    }
    float mean = sum / n;

    // Compute variance
    float var_sum = 0.0f;
    for (int i = 0; i < n; i++) {
        float diff = t.rssi_history[i] - mean;
        var_sum += diff * diff;
    }
    return var_sum / n;
}

// Remove stale peers not in current peer provider list
static void expire_stale_trackers() {
    if (!_peer_provider) return;

    RFPeer peers[MAX_PEERS];
    int peer_count = _peer_provider(peers, MAX_PEERS);

    for (int i = 0; i < _tracker_count; i++) {
        if (!_trackers[i].active) continue;

        bool found = false;
        for (int j = 0; j < peer_count; j++) {
            if (memcmp(_trackers[i].mac, peers[j].mac, 6) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            _trackers[i].active = false;
            DBG_DEBUG(TAG, "Expired peer %02X:%02X:%02X:%02X:%02X:%02X",
                      _trackers[i].mac[0], _trackers[i].mac[1],
                      _trackers[i].mac[2], _trackers[i].mac[3],
                      _trackers[i].mac[4], _trackers[i].mac[5]);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init(const RFMonitorConfig& config) {
    if (_initialized) return _active;
    _initialized = true;

    _sample_interval_ms = config.sample_interval_ms;
    _motion_threshold = config.motion_threshold;
    _peer_provider = config.peer_provider;

    if (!_peer_provider) {
        DBG_WARN(TAG, "No peer provider set, RF monitor inactive");
        _active = false;
        return false;
    }

    memset(_trackers, 0, sizeof(_trackers));
    _tracker_count = 0;
    _last_sample_ms = 0;
    _active = true;

    DBG_INFO(TAG, "Initialized: interval=%lums threshold=%.1f dBm",
             (unsigned long)_sample_interval_ms, _motion_threshold);
    return true;
}

void tick() {
    if (!_active || !_peer_provider) return;

    uint32_t now = millis();
    if (_last_sample_ms != 0 && (now - _last_sample_ms) < _sample_interval_ms) {
        return;
    }
    _last_sample_ms = now;

    // Sample RSSI from all peers via provider
    RFPeer peers[MAX_PEERS];
    int peer_count = _peer_provider(peers, MAX_PEERS);

    for (int i = 0; i < peer_count; i++) {
        if (!peers[i].is_direct) continue;  // Only track direct peers (1-hop)

        int idx = add_tracker(peers[i].mac);
        if (idx >= 0) {
            record_rssi(idx, peers[i].rssi);
        }
    }

    // Periodically expire stale trackers (every 30 samples)
    static int expire_counter = 0;
    if (++expire_counter >= 30) {
        expire_counter = 0;
        expire_stale_trackers();
    }
}

bool is_active() {
    return _active;
}

int get_peer_count() {
    int count = 0;
    for (int i = 0; i < _tracker_count; i++) {
        if (_trackers[i].active) count++;
    }
    return count;
}

int get_peer_stats(PeerRFStats* out, int max_count) {
    int written = 0;
    for (int i = 0; i < _tracker_count && written < max_count; i++) {
        if (!_trackers[i].active) continue;

        PeerRFStats& s = out[written];
        memcpy(s.mac, _trackers[i].mac, 6);

        // Most recent RSSI
        int last_idx = (_trackers[i].write_idx - 1 + WINDOW_SIZE) % WINDOW_SIZE;
        s.rssi = _trackers[i].rssi_history[last_idx];

        s.variance = compute_variance(i);
        s.motion_detected = (s.variance > _motion_threshold);
        s.sample_count = (uint16_t)_trackers[i].sample_count;
        written++;
    }
    return written;
}

int get_peer_rssi_json(char* buf, size_t size) {
    if (size < 3) return 0;

    int pos = 0;
    buf[pos++] = '[';

    bool first = true;
    for (int i = 0; i < _tracker_count; i++) {
        if (!_trackers[i].active) continue;

        int last_idx = (_trackers[i].write_idx - 1 + WINDOW_SIZE) % WINDOW_SIZE;
        int8_t rssi = _trackers[i].rssi_history[last_idx];
        float variance = compute_variance(i);
        bool motion = (variance > _motion_threshold);

        if (!first) {
            if (pos < (int)size - 1) buf[pos++] = ',';
        }
        first = false;

        int n = snprintf(buf + pos, size - pos,
            "{\"peer_id\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"rssi\":%d,\"variance\":%.1f,\"motion\":%s,\"samples\":%d}",
            _trackers[i].mac[0], _trackers[i].mac[1],
            _trackers[i].mac[2], _trackers[i].mac[3],
            _trackers[i].mac[4], _trackers[i].mac[5],
            rssi, variance, motion ? "true" : "false",
            _trackers[i].sample_count);

        if (n > 0 && pos + n < (int)size - 2) {
            pos += n;
        } else {
            break;  // Out of space
        }
    }

    if (pos < (int)size - 1) buf[pos++] = ']';
    buf[pos] = '\0';
    return pos;
}

int get_summary_json(char* buf, size_t size) {
    int total = 0;
    int motion_count = 0;
    for (int i = 0; i < _tracker_count; i++) {
        if (!_trackers[i].active) continue;
        total++;
        float variance = compute_variance(i);
        if (variance > _motion_threshold) motion_count++;
    }
    return snprintf(buf, size,
        "{\"rf_peers\":%d,\"motion_peers\":%d,\"threshold\":%.1f}",
        total, motion_count, _motion_threshold);
}

void set_motion_threshold(float threshold) {
    _motion_threshold = threshold;
    DBG_INFO(TAG, "Motion threshold set to %.1f dBm", _motion_threshold);
}

float get_motion_threshold() {
    return _motion_threshold;
}

}  // namespace hal_rf_monitor

#endif  // SIMULATOR
