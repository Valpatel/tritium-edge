/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "os_events.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <cstring>

// ---- Configuration ----
static constexpr int MAX_SUBSCRIBERS   = 48;
static constexpr int HISTORY_SIZE      = 64;   // Must be power of 2
static constexpr int QUEUE_DEPTH       = 32;

// ---- Subscriber entry ----
enum SubscriberMode : uint8_t {
    SUB_UNUSED   = 0,
    SUB_EVENT    = 1,   // Match a single EventId
    SUB_CATEGORY = 2,   // Match all events in a category
    SUB_ALL      = 3,   // Match every event
};

struct Subscriber {
    SubscriberMode mode;
    uint16_t filter;        // EventId or EventCategory depending on mode
    EventCallback callback;
    void* user_data;
    int id;                 // Unique subscriber id
};

// ---- Internal state ----
static Subscriber     _subs[MAX_SUBSCRIBERS];
static int            _next_sub_id = 1;

static TritiumEvent   _history[HISTORY_SIZE];
static int            _history_head  = 0;    // Next write position
static int            _history_count = 0;

static uint32_t       _total_published = 0;
static uint32_t       _total_dropped   = 0;

static SemaphoreHandle_t _mutex  = nullptr;
static QueueHandle_t     _queue  = nullptr;

// ---- Category lookup table for fast filtering ----
// Bitmask per category: which subscriber indices listen to it.
// Supports up to 48 subscribers via a uint64_t bitmask.
static uint64_t _cat_mask[16];   // Index by EventCategory (0x00..0x0F)
static uint64_t _all_mask = 0;   // Subscribers that want every event

static void rebuildMasks() {
    memset(_cat_mask, 0, sizeof(_cat_mask));
    _all_mask = 0;
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (_subs[i].mode == SUB_UNUSED) continue;
        uint64_t bit = (uint64_t)1 << i;
        if (_subs[i].mode == SUB_ALL) {
            _all_mask |= bit;
        } else if (_subs[i].mode == SUB_CATEGORY) {
            uint8_t cat = _subs[i].filter & 0xFF;
            if (cat < 16) _cat_mask[cat] |= bit;
        } else if (_subs[i].mode == SUB_EVENT) {
            uint8_t cat = (_subs[i].filter >> 8) & 0xFF;
            if (cat < 16) _cat_mask[cat] |= bit;
        }
    }
}

// ---- Convenience data helpers for TritiumEvent ----
int32_t TritiumEvent::asInt() const {
    if (data_len < 4) return 0;
    int32_t v;
    memcpy(&v, data, sizeof(v));
    return v;
}

float TritiumEvent::asFloat() const {
    if (data_len < 4) return 0.0f;
    float v;
    memcpy(&v, data, sizeof(v));
    return v;
}

const char* TritiumEvent::asString() const {
    return (const char*)data;
}

// ---- Singleton ----
TritiumEventBus& TritiumEventBus::instance() {
    static TritiumEventBus bus;
    return bus;
}

TritiumEventBus::TritiumEventBus() {
    _mutex = xSemaphoreCreateMutex();
    _queue = xQueueCreate(QUEUE_DEPTH, sizeof(TritiumEvent));
    memset(_subs, 0, sizeof(_subs));
    memset(_history, 0, sizeof(_history));
    memset(_cat_mask, 0, sizeof(_cat_mask));
}

// ---- Publish overloads ----
void TritiumEventBus::publish(EventId id) {
    TritiumEvent evt = {};
    evt.id = id;
    evt.timestamp_ms = millis();
    evt.data_len = 0;
    publish(evt);
}

void TritiumEventBus::publish(EventId id, int32_t value) {
    TritiumEvent evt = {};
    evt.id = id;
    evt.timestamp_ms = millis();
    evt.data_len = sizeof(value);
    memcpy(evt.data, &value, sizeof(value));
    publish(evt);
}

void TritiumEventBus::publish(EventId id, float value) {
    TritiumEvent evt = {};
    evt.id = id;
    evt.timestamp_ms = millis();
    evt.data_len = sizeof(value);
    memcpy(evt.data, &value, sizeof(value));
    publish(evt);
}

void TritiumEventBus::publish(EventId id, const char* str) {
    TritiumEvent evt = {};
    evt.id = id;
    evt.timestamp_ms = millis();
    if (str) {
        size_t len = strlen(str);
        if (len > sizeof(evt.data) - 1) len = sizeof(evt.data) - 1;
        memcpy(evt.data, str, len);
        evt.data[len] = '\0';
        evt.data_len = (uint8_t)(len + 1);
    } else {
        evt.data[0] = '\0';
        evt.data_len = 1;
    }
    publish(evt);
}

void TritiumEventBus::publish(EventId id, const void* data, uint8_t len) {
    TritiumEvent evt = {};
    evt.id = id;
    evt.timestamp_ms = millis();
    if (len > sizeof(evt.data)) len = sizeof(evt.data);
    if (data && len > 0) memcpy(evt.data, data, len);
    evt.data_len = len;
    publish(evt);
}

void TritiumEventBus::publish(const TritiumEvent& event) {
    // Enqueue for async delivery. If the queue is full, the event is dropped.
    // This is safe to call from ISR context (xQueueSendFromISR) but we use
    // the non-ISR variant here; ISR callers should use xQueueSendFromISR
    // directly with _queue if needed.
    BaseType_t ok = xQueueSend(_queue, &event, 0);
    if (ok != pdTRUE) {
        _total_dropped++;
    }
}

// ---- Subscribe ----
int TritiumEventBus::subscribe(EventId id, EventCallback cb, void* user_data) {
    if (!cb) return -1;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (_subs[i].mode == SUB_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        xSemaphoreGive(_mutex);
        return -1;
    }
    int sid = _next_sub_id++;
    _subs[slot] = { SUB_EVENT, (uint16_t)id, cb, user_data, sid };
    rebuildMasks();
    xSemaphoreGive(_mutex);
    return sid;
}

int TritiumEventBus::subscribeCategory(EventCategory cat, EventCallback cb,
                                       void* user_data) {
    if (!cb) return -1;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (_subs[i].mode == SUB_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        xSemaphoreGive(_mutex);
        return -1;
    }
    int sid = _next_sub_id++;
    _subs[slot] = { SUB_CATEGORY, (uint16_t)cat, cb, user_data, sid };
    rebuildMasks();
    xSemaphoreGive(_mutex);
    return sid;
}

int TritiumEventBus::subscribeAll(EventCallback cb, void* user_data) {
    if (!cb) return -1;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (_subs[i].mode == SUB_UNUSED) { slot = i; break; }
    }
    if (slot < 0) {
        xSemaphoreGive(_mutex);
        return -1;
    }
    int sid = _next_sub_id++;
    _subs[slot] = { SUB_ALL, 0, cb, user_data, sid };
    rebuildMasks();
    xSemaphoreGive(_mutex);
    return sid;
}

void TritiumEventBus::unsubscribe(int subscriber_id) {
    if (subscriber_id < 0) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (_subs[i].mode != SUB_UNUSED && _subs[i].id == subscriber_id) {
            _subs[i].mode = SUB_UNUSED;
            _subs[i].callback = nullptr;
            break;
        }
    }
    rebuildMasks();
    xSemaphoreGive(_mutex);
}

// ---- Process queued events ----
void TritiumEventBus::processQueue() {
    TritiumEvent evt;
    while (xQueueReceive(_queue, &evt, 0) == pdTRUE) {
        // Record in history ring buffer
        _history[_history_head] = evt;
        _history_head = (_history_head + 1) & (HISTORY_SIZE - 1);
        if (_history_count < HISTORY_SIZE) _history_count++;
        _total_published++;

        // Build combined mask: category subscribers + all subscribers
        uint8_t cat = (evt.id >> 8) & 0x0F;
        uint64_t mask = _cat_mask[cat] | _all_mask;

        if (mask == 0) {
            _total_dropped++;
            continue;
        }

        // Take mutex to safely read subscriber table
        xSemaphoreTake(_mutex, portMAX_DELAY);

        // Iterate only set bits
        uint64_t remaining = mask;
        while (remaining) {
            int i = __builtin_ctzll(remaining);  // Index of lowest set bit
            remaining &= remaining - 1;          // Clear lowest set bit

            Subscriber& sub = _subs[i];
            if (sub.mode == SUB_UNUSED) continue;

            bool match = false;
            if (sub.mode == SUB_ALL) {
                match = true;
            } else if (sub.mode == SUB_CATEGORY) {
                match = true;  // Already filtered by category mask
            } else if (sub.mode == SUB_EVENT) {
                match = (sub.filter == (uint16_t)evt.id);
            }

            if (match && sub.callback) {
                sub.callback(evt, sub.user_data);
            }
        }

        xSemaphoreGive(_mutex);
    }
}

// ---- History ----
const TritiumEvent* TritiumEventBus::getHistory(int* count) {
    if (count) *count = _history_count;
    return _history;
}

// ---- Stats ----
uint32_t TritiumEventBus::totalPublished() const {
    return _total_published;
}

uint32_t TritiumEventBus::totalDropped() const {
    return _total_dropped;
}
