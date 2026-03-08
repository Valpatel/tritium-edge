// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.

#include "hal_diaglog.h"
#include "debug_log.h"

#include <cstring>
#include <cstdio>

static constexpr const char* TAG = "diaglog";

// =============================================================================
// SIMULATOR — in-memory vector, no LittleFS
// =============================================================================
#ifdef SIMULATOR

#include <vector>
#include <mutex>

static std::mutex _sim_mutex;
static std::vector<DiagEvent> _sim_events;
static DiagLogHeader _sim_header = {};
static bool _sim_inited = false;

static const char* severity_str(DiagSeverity s) {
    switch (s) {
        case DiagSeverity::INFO:     return "INFO";
        case DiagSeverity::WARN:     return "WARN";
        case DiagSeverity::ERR:      return "ERROR";
        case DiagSeverity::CRITICAL: return "CRITICAL";
        default:                     return "?";
    }
}

bool diaglog_init() {
    std::lock_guard<std::mutex> lock(_sim_mutex);
    if (_sim_inited) return true;

    _sim_header.magic = DIAGLOG_MAGIC;
    _sim_header.version = 1;
    _sim_header.max_events = DIAGLOG_MAX_EVENTS;
    _sim_header.write_index = 0;
    _sim_header.total_count = 0;
    _sim_header.boot_count++;
    _sim_events.clear();
    _sim_events.reserve(DIAGLOG_MAX_EVENTS);
    _sim_inited = true;

    DBG_INFO(TAG, "Simulator diaglog initialized (boot #%u)", _sim_header.boot_count);
    return true;
}

void diaglog_write(DiagSeverity severity, const char* subsystem,
                   uint16_t code, const char* message, float value) {
    std::lock_guard<std::mutex> lock(_sim_mutex);
    if (!_sim_inited) return;

    DiagEvent ev = {};
    ev.epoch = 0;
    ev.uptime_ms = 0;  // no millis() in simulator
    ev.severity = severity;
    strncpy(ev.subsystem, subsystem, sizeof(ev.subsystem) - 1);
    ev.code = code;
    strncpy(ev.message, message, sizeof(ev.message) - 1);
    ev.value = value;

    if ((int)_sim_events.size() < DIAGLOG_MAX_EVENTS) {
        _sim_events.push_back(ev);
    } else {
        _sim_events[_sim_header.write_index % DIAGLOG_MAX_EVENTS] = ev;
    }
    _sim_header.write_index = (_sim_header.write_index + 1) % DIAGLOG_MAX_EVENTS;
    _sim_header.total_count++;
}

int diaglog_read(DiagEvent* events, int max_out, int offset) {
    std::lock_guard<std::mutex> lock(_sim_mutex);
    if (!_sim_inited || !events || max_out <= 0) return 0;

    int stored = (int)_sim_events.size();
    if (offset >= stored) return 0;
    int avail = stored - offset;
    int n = (avail < max_out) ? avail : max_out;

    // Events in the vector are stored in insertion order for simplicity
    // When the ring wraps, oldest is at write_index
    if (stored < DIAGLOG_MAX_EVENTS) {
        // Haven't wrapped yet — simple linear read
        memcpy(events, &_sim_events[offset], n * sizeof(DiagEvent));
    } else {
        // Wrapped — oldest is at write_index
        for (int i = 0; i < n; i++) {
            int idx = (_sim_header.write_index + offset + i) % DIAGLOG_MAX_EVENTS;
            events[i] = _sim_events[idx];
        }
    }
    return n;
}

int diaglog_count() {
    std::lock_guard<std::mutex> lock(_sim_mutex);
    return (int)_sim_events.size();
}

uint32_t diaglog_boot_count() {
    return _sim_header.boot_count;
}

void diaglog_clear() {
    std::lock_guard<std::mutex> lock(_sim_mutex);
    _sim_events.clear();
    _sim_header.write_index = 0;
    _sim_header.total_count = 0;
    DBG_INFO(TAG, "Log cleared");
}

int diaglog_get_json(char* buf, size_t buf_size, int offset, int count) {
    if (!buf || buf_size < 16) return -1;

    // Read events into temp buffer
    int max_read = (count < DIAGLOG_MAX_EVENTS) ? count : DIAGLOG_MAX_EVENTS;
    DiagEvent* tmp = new DiagEvent[max_read];
    int n = diaglog_read(tmp, max_read, offset);

    int pos = snprintf(buf, buf_size,
        "{\"boot_count\":%u,\"total\":%u,\"returned\":%d,\"events\":[",
        _sim_header.boot_count, _sim_header.total_count, n);

    for (int i = 0; i < n && pos < (int)buf_size - 256; i++) {
        const DiagEvent& ev = tmp[i];
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, buf_size - pos,
            "{\"epoch\":%u,\"uptime_ms\":%u,\"severity\":\"%s\","
            "\"subsystem\":\"%s\",\"code\":%u,\"message\":\"%s\",\"value\":%.3f}",
            (unsigned)ev.epoch, (unsigned)ev.uptime_ms,
            severity_str(ev.severity),
            ev.subsystem, (unsigned)ev.code, ev.message, ev.value);
    }
    pos += snprintf(buf + pos, buf_size - pos, "]}");

    delete[] tmp;
    return pos;
}

// =============================================================================
// ESP32 — LittleFS persistent ring buffer
// =============================================================================
#else

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t _mutex = nullptr;
static DiagLogHeader _header = {};
static bool _inited = false;

static const char* severity_str(DiagSeverity s) {
    switch (s) {
        case DiagSeverity::INFO:     return "INFO";
        case DiagSeverity::WARN:     return "WARN";
        case DiagSeverity::ERR:      return "ERROR";
        case DiagSeverity::CRITICAL: return "CRITICAL";
        default:                     return "?";
    }
}

// ── File helpers ─────────────────────────────────────────────────────────────

static size_t event_offset(uint32_t index) {
    // Header is at offset 0 (128 bytes), events start at offset 128
    return sizeof(DiagLogHeader) + (size_t)(index % DIAGLOG_MAX_EVENTS) * sizeof(DiagEvent);
}

static bool read_header() {
    File f = LittleFS.open(DIAGLOG_PATH, "r");
    if (!f) return false;
    size_t n = f.read((uint8_t*)&_header, sizeof(_header));
    f.close();
    return (n == sizeof(_header) && _header.magic == DIAGLOG_MAGIC);
}

static bool write_header() {
    File f = LittleFS.open(DIAGLOG_PATH, "r+");
    if (!f) {
        // File doesn't exist yet — create it
        f = LittleFS.open(DIAGLOG_PATH, "w");
        if (!f) return false;
    }
    f.seek(0);
    size_t n = f.write((const uint8_t*)&_header, sizeof(_header));
    f.close();
    return (n == sizeof(_header));
}

static bool write_event_at(uint32_t index, const DiagEvent& ev) {
    File f = LittleFS.open(DIAGLOG_PATH, "r+");
    if (!f) return false;
    f.seek(event_offset(index));
    size_t n = f.write((const uint8_t*)&ev, sizeof(ev));
    f.close();
    return (n == sizeof(ev));
}

static bool read_event_at(uint32_t index, DiagEvent& ev) {
    File f = LittleFS.open(DIAGLOG_PATH, "r");
    if (!f) return false;
    f.seek(event_offset(index));
    size_t n = f.read((uint8_t*)&ev, sizeof(ev));
    f.close();
    return (n == sizeof(ev));
}

static const char* reset_reason_str() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWER_ON";
        case ESP_RST_EXT:       return "EXTERNAL";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_USB:       return "USB";
        default:                return "UNKNOWN";
    }
}

// ── Preallocate file ─────────────────────────────────────────────────────────
// Ensure the file is large enough for header + MAX_EVENTS records so that
// random-access writes via seek() work correctly on LittleFS.

static bool preallocate_file() {
    size_t required = sizeof(DiagLogHeader)
                    + (size_t)DIAGLOG_MAX_EVENTS * sizeof(DiagEvent);

    File f = LittleFS.open(DIAGLOG_PATH, "r");
    if (f) {
        size_t current = f.size();
        f.close();
        if (current >= required) return true;
    }

    // Write zeros to expand the file
    f = LittleFS.open(DIAGLOG_PATH, "w");
    if (!f) {
        DBG_ERROR(TAG, "Cannot create %s", DIAGLOG_PATH);
        return false;
    }

    // Write in 512-byte chunks to avoid large stack allocation
    uint8_t zeros[512];
    memset(zeros, 0, sizeof(zeros));
    size_t remaining = required;
    while (remaining > 0) {
        size_t chunk = (remaining < sizeof(zeros)) ? remaining : sizeof(zeros);
        size_t written = f.write(zeros, chunk);
        if (written != chunk) {
            DBG_ERROR(TAG, "Preallocate write failed at %u/%u",
                      (unsigned)(required - remaining), (unsigned)required);
            f.close();
            return false;
        }
        remaining -= chunk;
    }
    f.close();

    DBG_INFO(TAG, "Preallocated %u bytes (%u event slots)",
             (unsigned)required, (unsigned)DIAGLOG_MAX_EVENTS);
    return true;
}

// ── Public API ───────────────────────────────────────────────────────────────

bool diaglog_init() {
    if (_inited) return true;

    if (!_mutex) {
        _mutex = xSemaphoreCreateMutex();
        if (!_mutex) {
            DBG_ERROR(TAG, "Mutex creation failed");
            return false;
        }
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

    // Ensure /diag directory exists
    if (!LittleFS.exists(DIAGLOG_DIR)) {
        LittleFS.mkdir(DIAGLOG_DIR);
    }

    // Try to read existing header
    bool have_header = read_header();

    if (!have_header || _header.magic != DIAGLOG_MAGIC
        || _header.version != 1
        || _header.max_events != DIAGLOG_MAX_EVENTS) {
        // Fresh file — initialize header
        DBG_INFO(TAG, "Creating new log file");
        memset(&_header, 0, sizeof(_header));
        _header.magic = DIAGLOG_MAGIC;
        _header.version = 1;
        _header.max_events = DIAGLOG_MAX_EVENTS;
        _header.write_index = 0;
        _header.total_count = 0;
        _header.boot_count = 0;

        if (!preallocate_file()) {
            xSemaphoreGive(_mutex);
            return false;
        }
    }

    // Increment boot count
    _header.boot_count++;
    write_header();

    _inited = true;
    xSemaphoreGive(_mutex);

    DBG_INFO(TAG, "Initialized: boot #%u, %u events stored, capacity %u",
             (unsigned)_header.boot_count,
             (unsigned)(_header.total_count < DIAGLOG_MAX_EVENTS
                        ? _header.total_count : DIAGLOG_MAX_EVENTS),
             (unsigned)DIAGLOG_MAX_EVENTS);

    // Auto-log boot event with reset reason
    char boot_msg[80];
    snprintf(boot_msg, sizeof(boot_msg), "Boot #%u, reset: %s",
             (unsigned)_header.boot_count, reset_reason_str());
    diaglog_write(DiagSeverity::INFO, "system", 1, boot_msg);

    return true;
}

void diaglog_write(DiagSeverity severity, const char* subsystem,
                   uint16_t code, const char* message, float value) {
    if (!_inited || !_mutex) return;

    DiagEvent ev = {};

    // Timestamp
    time_t now;
    time(&now);
    ev.epoch = (now > 1700000000) ? (uint32_t)now : 0;
    ev.uptime_ms = millis();
    ev.severity = severity;
    ev.code = code;
    ev.value = value;

    if (subsystem) {
        strncpy(ev.subsystem, subsystem, sizeof(ev.subsystem) - 1);
    }
    if (message) {
        strncpy(ev.message, message, sizeof(ev.message) - 1);
    }

    xSemaphoreTake(_mutex, portMAX_DELAY);

    uint32_t slot = _header.write_index;
    if (write_event_at(slot, ev)) {
        _header.write_index = (slot + 1) % DIAGLOG_MAX_EVENTS;
        _header.total_count++;
        write_header();
    } else {
        DBG_ERROR(TAG, "Failed to write event at slot %u", (unsigned)slot);
    }

    xSemaphoreGive(_mutex);

    // Echo to serial at WARN+ level
    if (severity >= DiagSeverity::WARN) {
        DBG_WARN(TAG, "[%s] %s/%u: %s (val=%.2f)",
                 severity_str(severity), subsystem, (unsigned)code, message, value);
    }
}

int diaglog_read(DiagEvent* events, int max_out, int offset) {
    if (!_inited || !_mutex || !events || max_out <= 0) return 0;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    int stored = (_header.total_count < DIAGLOG_MAX_EVENTS)
                 ? (int)_header.total_count
                 : (int)DIAGLOG_MAX_EVENTS;

    if (offset >= stored) {
        xSemaphoreGive(_mutex);
        return 0;
    }

    int avail = stored - offset;
    int n = (avail < max_out) ? avail : max_out;

    // Calculate the file index of the oldest event
    uint32_t oldest_slot;
    if (_header.total_count <= DIAGLOG_MAX_EVENTS) {
        oldest_slot = 0;  // Haven't wrapped yet
    } else {
        oldest_slot = _header.write_index;  // Oldest was just overwritten
    }

    for (int i = 0; i < n; i++) {
        uint32_t slot = (oldest_slot + offset + i) % DIAGLOG_MAX_EVENTS;
        if (!read_event_at(slot, events[i])) {
            xSemaphoreGive(_mutex);
            return i;
        }
    }

    xSemaphoreGive(_mutex);
    return n;
}

int diaglog_count() {
    if (!_inited) return 0;
    return (_header.total_count < DIAGLOG_MAX_EVENTS)
           ? (int)_header.total_count
           : (int)DIAGLOG_MAX_EVENTS;
}

uint32_t diaglog_boot_count() {
    return _header.boot_count;
}

void diaglog_clear() {
    if (!_inited || !_mutex) return;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    _header.write_index = 0;
    _header.total_count = 0;
    // Keep boot_count intact
    write_header();

    xSemaphoreGive(_mutex);
    DBG_INFO(TAG, "Log cleared (boot count preserved: %u)",
             (unsigned)_header.boot_count);
}

// ── JSON helper ──────────────────────────────────────────────────────────────

static int json_escape_str(char* dst, size_t dst_size, const char* src) {
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 2; si++) {
        char c = src[si];
        if (c == '"' || c == '\\') {
            if (di + 2 >= dst_size) break;
            dst[di++] = '\\';
            dst[di++] = c;
        } else if (c == '\n') {
            if (di + 2 >= dst_size) break;
            dst[di++] = '\\';
            dst[di++] = 'n';
        } else if (c >= 0x20) {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
    return (int)di;
}

int diaglog_get_json(char* buf, size_t buf_size, int offset, int count) {
    if (!buf || buf_size < 32) return -1;

    int stored = diaglog_count();
    int max_read = (count < stored) ? count : stored;
    if (max_read <= 0) {
        return snprintf(buf, buf_size,
            "{\"boot_count\":%u,\"total\":%u,\"returned\":0,\"events\":[]}",
            (unsigned)_header.boot_count, (unsigned)_header.total_count);
    }

    // Allocate temp buffer for batch read (stack-friendly: max ~50 at a time)
    int batch = (max_read < 50) ? max_read : 50;
    DiagEvent* tmp = (DiagEvent*)malloc(batch * sizeof(DiagEvent));
    if (!tmp) return -1;

    int pos = snprintf(buf, buf_size,
        "{\"boot_count\":%u,\"total\":%u,\"returned\":%d,\"events\":[",
        (unsigned)_header.boot_count, (unsigned)_header.total_count, max_read);

    char esc_msg[128];
    char esc_sub[24];
    int written = 0;
    int read_offset = offset;

    while (written < max_read && pos < (int)buf_size - 256) {
        int to_read = max_read - written;
        if (to_read > batch) to_read = batch;

        int got = diaglog_read(tmp, to_read, read_offset);
        if (got <= 0) break;

        for (int i = 0; i < got && pos < (int)buf_size - 256; i++) {
            const DiagEvent& ev = tmp[i];
            if (written > 0) buf[pos++] = ',';

            json_escape_str(esc_sub, sizeof(esc_sub), ev.subsystem);
            json_escape_str(esc_msg, sizeof(esc_msg), ev.message);

            pos += snprintf(buf + pos, buf_size - pos,
                "{\"epoch\":%u,\"uptime_ms\":%u,\"severity\":\"%s\","
                "\"subsystem\":\"%s\",\"code\":%u,"
                "\"message\":\"%s\",\"value\":%.3f}",
                (unsigned)ev.epoch, (unsigned)ev.uptime_ms,
                severity_str(ev.severity),
                esc_sub, (unsigned)ev.code, esc_msg, ev.value);
            written++;
        }
        read_offset += got;
    }

    pos += snprintf(buf + pos, buf_size - pos, "]}");
    free(tmp);
    return pos;
}

#endif  // SIMULATOR
