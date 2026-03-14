// Tritium-OS Power Consumption Tracker
// Estimates and logs power consumption over time based on active HALs
// and scan intervals. Included in heartbeat for fleet-wide power analysis.
//
// Each subsystem registers its estimated current draw (mA). The tracker
// samples active subsystems periodically and accumulates mAh consumed.
// This helps predict battery life and identify power-hungry configurations.
//
// Usage:
//   PowerTracker::instance().registerConsumer("wifi", 80);  // 80mA when active
//   PowerTracker::instance().setActive("wifi", true);
//   // In heartbeat JSON: PowerTracker::instance().toJson(buf, size)
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <cstddef>

// Maximum number of power consumers that can be registered
static constexpr int MAX_POWER_CONSUMERS = 24;

struct PowerConsumer {
    char name[24];          // Subsystem name: "wifi", "ble_scan", "display", etc.
    uint16_t draw_ma;       // Estimated current draw when active (milliamps)
    bool active;            // Whether this consumer is currently drawing power
};

struct PowerSnapshot {
    uint32_t timestamp_s;   // Uptime seconds when snapshot was taken
    uint16_t total_draw_ma; // Total estimated draw at that moment
    uint16_t consumer_count; // Number of active consumers
};

class PowerTracker {
public:
    static PowerTracker& instance();

    /// Register a power consumer with its estimated draw in mA.
    /// Returns consumer index, or -1 if full.
    int registerConsumer(const char* name, uint16_t draw_ma);

    /// Set whether a consumer is currently active (drawing power).
    void setActive(const char* name, bool active);
    void setActive(int index, bool active);

    /// Update estimated draw for a consumer (e.g., scan interval changed).
    void setDraw(const char* name, uint16_t draw_ma);

    /// Call periodically (e.g., every second from PowerService tick).
    /// Accumulates mAh based on currently active consumers.
    void tick();

    /// Get current total estimated draw in mA.
    uint16_t getCurrentDrawMa() const;

    /// Get accumulated consumption in mAh since boot.
    float getConsumedMah() const;

    /// Get estimated remaining runtime in minutes (requires battery_mah > 0).
    int getEstimatedRuntimeMin(float battery_mah, int battery_pct) const;

    /// Get number of registered consumers.
    int getConsumerCount() const;

    /// Get a specific consumer by index.
    const PowerConsumer* getConsumer(int index) const;

    /// Serialize power tracking data to JSON.
    /// Returns number of bytes written, or -1 on error.
    int toJson(char* buf, size_t size) const;

    /// Reset accumulated consumption (e.g., on full charge detected).
    void reset();

private:
    PowerTracker() = default;

    PowerConsumer _consumers[MAX_POWER_CONSUMERS] = {};
    int _count = 0;
    float _consumed_mah = 0.0f;          // Accumulated mAh since boot/reset
    uint32_t _lastTickMs = 0;
    uint32_t _bootTimeMs = 0;

    int findByName(const char* name) const;
};
