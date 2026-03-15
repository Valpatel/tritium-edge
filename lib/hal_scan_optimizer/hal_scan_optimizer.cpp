// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_scan_optimizer.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

// Use millis() from Arduino framework
#ifdef ARDUINO
#include <Arduino.h>
#else
// Stub for non-Arduino builds (unit tests, etc.)
static uint32_t _stub_millis = 0;
static uint32_t millis() { return _stub_millis; }
#endif

namespace hal_scan_optimizer {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static Config _config;
static bool _initialized = false;

// Circular buffer for sighting timestamps (BLE + WiFi separate)
static constexpr int RING_SIZE = 256;

struct SightingRing {
    uint32_t timestamps[RING_SIZE];
    uint16_t head = 0;
    uint16_t count = 0;
    uint32_t total = 0;

    void record(uint32_t now) {
        timestamps[head] = now;
        head = (head + 1) % RING_SIZE;
        if (count < RING_SIZE) count++;
        total++;
    }

    void record_many(uint32_t now, uint32_t n) {
        for (uint32_t i = 0; i < n && i < RING_SIZE; i++) {
            record(now);
        }
        // If n > RING_SIZE, just add to total
        if (n > RING_SIZE) {
            total += (n - RING_SIZE);
        }
    }

    // Count sightings within the last window_ms
    uint32_t count_in_window(uint32_t now, uint32_t window_ms) const {
        uint32_t cnt = 0;
        uint32_t cutoff = (now > window_ms) ? (now - window_ms) : 0;
        for (uint16_t i = 0; i < count; i++) {
            uint16_t idx = (head + RING_SIZE - 1 - i) % RING_SIZE;
            if (timestamps[idx] >= cutoff) {
                cnt++;
            } else {
                break;  // Ring is ordered by insertion time
            }
        }
        return cnt;
    }
};

static SightingRing _ble_ring;
static SightingRing _wifi_ring;

// Current state
static ActivityLevel _level = ActivityLevel::NORMAL;
static uint32_t _level_entered_ms = 0;
static uint32_t _spike_ended_ms = 0;
static uint32_t _adjustment_count = 0;

// Previous window rate for spike detection
static float _prev_combined_rate = 0.0f;
static uint32_t _last_rate_calc_ms = 0;

// Current recommended intervals
static uint32_t _ble_interval = 10000;
static uint32_t _wifi_interval = 30000;


// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

bool init(const Config& config) {
    _config = config;
    _initialized = true;

    memset(&_ble_ring, 0, sizeof(_ble_ring));
    memset(&_wifi_ring, 0, sizeof(_wifi_ring));

    _level = ActivityLevel::NORMAL;
    _level_entered_ms = millis();
    _spike_ended_ms = 0;
    _adjustment_count = 0;
    _prev_combined_rate = 0.0f;
    _last_rate_calc_ms = millis();

    _ble_interval = config.ble_interval_normal_ms;
    _wifi_interval = config.wifi_interval_normal_ms;

    return true;
}

void shutdown() {
    _initialized = false;
}

void record_ble_sighting() {
    if (!_initialized) return;
    _ble_ring.record(millis());
}

void record_wifi_sighting() {
    if (!_initialized) return;
    _wifi_ring.record(millis());
}

void record_ble_sightings(uint32_t count) {
    if (!_initialized || count == 0) return;
    _ble_ring.record_many(millis(), count);
}

void record_wifi_sightings(uint32_t count) {
    if (!_initialized || count == 0) return;
    _wifi_ring.record_many(millis(), count);
}

static void _apply_level(ActivityLevel new_level) {
    uint32_t now = millis();

    // Check hold time to prevent oscillation
    if (now - _level_entered_ms < _config.level_hold_ms && new_level != ActivityLevel::SPIKE) {
        return;
    }

    // Check spike cooldown
    if (new_level == ActivityLevel::SPIKE && _spike_ended_ms > 0) {
        if (now - _spike_ended_ms < _config.spike_cooldown_ms) {
            // In cooldown, treat as BUSY instead
            new_level = ActivityLevel::BUSY;
        }
    }

    if (new_level == _level) return;

    // Track spike exit for cooldown
    if (_level == ActivityLevel::SPIKE && new_level != ActivityLevel::SPIKE) {
        _spike_ended_ms = now;
    }

    _level = new_level;
    _level_entered_ms = now;
    _adjustment_count++;

    if (!_config.enabled) return;

    // Apply new intervals
    switch (new_level) {
        case ActivityLevel::QUIET:
            _ble_interval = _config.ble_interval_quiet_ms;
            _wifi_interval = _config.wifi_interval_quiet_ms;
            break;
        case ActivityLevel::NORMAL:
            _ble_interval = _config.ble_interval_normal_ms;
            _wifi_interval = _config.wifi_interval_normal_ms;
            break;
        case ActivityLevel::BUSY:
            _ble_interval = _config.ble_interval_busy_ms;
            _wifi_interval = _config.wifi_interval_busy_ms;
            break;
        case ActivityLevel::SPIKE:
            _ble_interval = _config.ble_interval_spike_ms;
            _wifi_interval = _config.wifi_interval_spike_ms;
            break;
    }
}

void tick() {
    if (!_initialized) return;

    uint32_t now = millis();
    uint32_t window_ms = _config.window_seconds * 1000;

    // Count sightings in current window
    uint32_t ble_count = _ble_ring.count_in_window(now, window_ms);
    uint32_t wifi_count = _wifi_ring.count_in_window(now, window_ms);

    // Compute rates (per minute)
    float window_minutes = _config.window_seconds / 60.0f;
    if (window_minutes < 0.001f) window_minutes = 1.0f;

    float ble_rate = (float)ble_count / window_minutes;
    float wifi_rate = (float)wifi_count / window_minutes;
    float combined = ble_rate + wifi_rate;

    // Update previous rate periodically (every half window)
    uint32_t half_window_ms = window_ms / 2;
    if (now - _last_rate_calc_ms >= half_window_ms) {
        _prev_combined_rate = combined;
        _last_rate_calc_ms = now;
    }

    // Classify activity level
    ActivityLevel new_level;

    // Check for spike first (rate increase by spike_ratio)
    if (_prev_combined_rate > 0.001f &&
        combined / _prev_combined_rate >= _config.spike_ratio &&
        combined >= _config.spike_threshold) {
        new_level = ActivityLevel::SPIKE;
    } else if (combined >= _config.spike_threshold) {
        new_level = ActivityLevel::SPIKE;
    } else if (combined >= _config.busy_threshold) {
        new_level = ActivityLevel::BUSY;
    } else if (combined <= _config.quiet_threshold) {
        new_level = ActivityLevel::QUIET;
    } else {
        new_level = ActivityLevel::NORMAL;
    }

    _apply_level(new_level);
}

ActivityLevel get_activity_level() {
    return _level;
}

uint32_t get_ble_interval_ms() {
    return _ble_interval;
}

uint32_t get_wifi_interval_ms() {
    return _wifi_interval;
}

bool is_active() {
    return _initialized;
}

ScanStats get_stats() {
    ScanStats s;
    uint32_t now = millis();
    uint32_t window_ms = _config.window_seconds * 1000;
    float window_minutes = _config.window_seconds / 60.0f;
    if (window_minutes < 0.001f) window_minutes = 1.0f;

    s.window_ble_count = _ble_ring.count_in_window(now, window_ms);
    s.window_wifi_count = _wifi_ring.count_in_window(now, window_ms);
    s.ble_rate_per_min = (float)s.window_ble_count / window_minutes;
    s.wifi_rate_per_min = (float)s.window_wifi_count / window_minutes;
    s.combined_rate = s.ble_rate_per_min + s.wifi_rate_per_min;
    s.prev_combined_rate = _prev_combined_rate;
    s.ble_total = _ble_ring.total;
    s.wifi_total = _wifi_ring.total;
    return s;
}

const char* activity_level_str(ActivityLevel level) {
    switch (level) {
        case ActivityLevel::QUIET:  return "quiet";
        case ActivityLevel::NORMAL: return "normal";
        case ActivityLevel::BUSY:   return "busy";
        case ActivityLevel::SPIKE:  return "spike";
        default:                    return "unknown";
    }
}

int get_status_json(char* buf, size_t buf_size) {
    if (!_initialized) {
        return snprintf(buf, buf_size, "{\"optimizer\":{\"active\":false}}");
    }

    ScanStats s = get_stats();
    return snprintf(buf, buf_size,
        "{\"optimizer\":{"
        "\"active\":true,"
        "\"level\":\"%s\","
        "\"ble_rate\":%.1f,"
        "\"wifi_rate\":%.1f,"
        "\"combined_rate\":%.1f,"
        "\"ble_interval_ms\":%lu,"
        "\"wifi_interval_ms\":%lu,"
        "\"adjustments\":%lu,"
        "\"ble_total\":%lu,"
        "\"wifi_total\":%lu"
        "}}",
        activity_level_str(_level),
        s.ble_rate_per_min,
        s.wifi_rate_per_min,
        s.combined_rate,
        (unsigned long)_ble_interval,
        (unsigned long)_wifi_interval,
        (unsigned long)_adjustment_count,
        (unsigned long)s.ble_total,
        (unsigned long)s.wifi_total
    );
}

}  // namespace hal_scan_optimizer
