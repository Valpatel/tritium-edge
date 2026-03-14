// Tritium-OS Power Consumption Tracker — implementation
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "power_tracker.h"

#include <cstring>
#include <cstdio>

#ifndef SIMULATOR
#include <Arduino.h>
#else
static uint32_t millis() { return 0; }
#endif

// ============================================================================
// Singleton
// ============================================================================

PowerTracker& PowerTracker::instance() {
    static PowerTracker s;
    return s;
}

// ============================================================================
// Consumer registration
// ============================================================================

int PowerTracker::registerConsumer(const char* name, uint16_t draw_ma) {
    if (_count >= MAX_POWER_CONSUMERS || !name) return -1;

    // Check for duplicate
    int existing = findByName(name);
    if (existing >= 0) {
        _consumers[existing].draw_ma = draw_ma;
        return existing;
    }

    int idx = _count;
    strncpy(_consumers[idx].name, name, sizeof(_consumers[idx].name) - 1);
    _consumers[idx].name[sizeof(_consumers[idx].name) - 1] = '\0';
    _consumers[idx].draw_ma = draw_ma;
    _consumers[idx].active = false;
    _count++;

    if (_bootTimeMs == 0) _bootTimeMs = millis();

    return idx;
}

void PowerTracker::setActive(const char* name, bool active) {
    int idx = findByName(name);
    if (idx >= 0) _consumers[idx].active = active;
}

void PowerTracker::setActive(int index, bool active) {
    if (index >= 0 && index < _count) {
        _consumers[index].active = active;
    }
}

void PowerTracker::setDraw(const char* name, uint16_t draw_ma) {
    int idx = findByName(name);
    if (idx >= 0) _consumers[idx].draw_ma = draw_ma;
}

// ============================================================================
// Tick — accumulate mAh
// ============================================================================

void PowerTracker::tick() {
    uint32_t now = millis();

    if (_lastTickMs == 0) {
        _lastTickMs = now;
        return;
    }

    uint32_t elapsed_ms = now - _lastTickMs;
    _lastTickMs = now;

    // Avoid huge jumps (e.g., after sleep)
    if (elapsed_ms > 10000) elapsed_ms = 10000;

    uint16_t total_ma = getCurrentDrawMa();

    // Convert: mA * ms -> mAh
    // mAh = mA * (ms / 3600000)
    float mah_delta = (float)total_ma * (float)elapsed_ms / 3600000.0f;
    _consumed_mah += mah_delta;
}

// ============================================================================
// Queries
// ============================================================================

uint16_t PowerTracker::getCurrentDrawMa() const {
    uint16_t total = 0;
    for (int i = 0; i < _count; i++) {
        if (_consumers[i].active) {
            total += _consumers[i].draw_ma;
        }
    }
    // Add baseline ESP32-S3 idle draw (~40mA)
    total += 40;
    return total;
}

float PowerTracker::getConsumedMah() const {
    return _consumed_mah;
}

int PowerTracker::getEstimatedRuntimeMin(float battery_mah, int battery_pct) const {
    if (battery_mah <= 0 || battery_pct <= 0) return -1;

    uint16_t draw = getCurrentDrawMa();
    if (draw == 0) return -1;

    float remaining_mah = battery_mah * ((float)battery_pct / 100.0f);
    float hours = remaining_mah / (float)draw;
    return (int)(hours * 60.0f);
}

int PowerTracker::getConsumerCount() const { return _count; }

const PowerConsumer* PowerTracker::getConsumer(int index) const {
    if (index < 0 || index >= _count) return nullptr;
    return &_consumers[index];
}

void PowerTracker::reset() {
    _consumed_mah = 0.0f;
    _lastTickMs = millis();
}

int PowerTracker::findByName(const char* name) const {
    for (int i = 0; i < _count; i++) {
        if (strcmp(_consumers[i].name, name) == 0) return i;
    }
    return -1;
}

// ============================================================================
// JSON serialization
// ============================================================================

int PowerTracker::toJson(char* buf, size_t size) const {
    uint16_t draw = getCurrentDrawMa();
    uint32_t uptime_s = millis() / 1000;

    // Count active consumers
    int active_count = 0;
    for (int i = 0; i < _count; i++) {
        if (_consumers[i].active) active_count++;
    }

    int pos = snprintf(buf, size,
        "{\"current_draw_ma\":%u,"
        "\"consumed_mah\":%.1f,"
        "\"uptime_s\":%lu,"
        "\"active_consumers\":%d,"
        "\"total_consumers\":%d,"
        "\"consumers\":[",
        (unsigned)draw,
        (double)_consumed_mah,
        (unsigned long)uptime_s,
        active_count,
        _count);

    if (pos < 0 || (size_t)pos >= size) {
        if (size > 0) buf[0] = '\0';
        return -1;
    }

    // Append active consumer details
    bool first = true;
    for (int i = 0; i < _count && (size_t)pos < size - 60; i++) {
        if (!_consumers[i].active) continue;
        pos += snprintf(buf + pos, size - pos,
            "%s{\"name\":\"%s\",\"draw_ma\":%u}",
            first ? "" : ",",
            _consumers[i].name,
            (unsigned)_consumers[i].draw_ma);
        first = false;
    }

    pos += snprintf(buf + pos, size - pos, "]}");

    if (pos < 0 || (size_t)pos >= size) {
        if (size > 0) buf[0] = '\0';
        return -1;
    }

    return pos;
}
