// Tritium-OS Serial Capture — ring buffer and Print wrapper for capturing
// Serial output and feeding it to WebSocket terminal clients.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "serial_capture.h"
#include "debug_log.h"

#include "tritium_compat.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>

static const char* TAG = "serial_cap";

// ── Ring buffer configuration ────────────────────────────────────────────────

static constexpr int MAX_LINES    = 48;
static constexpr int MAX_LINE_LEN = 96;

static char _ring[MAX_LINES][MAX_LINE_LEN];
static int  _ring_head  = 0;   // next write position
static int  _ring_count = 0;   // total lines stored

// Current line accumulator
static char _line_buf[MAX_LINE_LEN];
static int  _line_pos = 0;

// Thread safety
static SemaphoreHandle_t _mutex = nullptr;

// Line callback
static serial_capture::SerialLineCallback _callback = nullptr;
static void* _callback_data = nullptr;

// Command injection buffer (ring of pending commands)
static constexpr int MAX_INJECT_CMDS = 4;
static constexpr int MAX_INJECT_LEN  = 128;
static char _inject_ring[MAX_INJECT_CMDS][MAX_INJECT_LEN];
static volatile int _inject_head  = 0;
static volatile int _inject_tail  = 0;

// ── Capture helper (no Arduino Print class — uses direct line accumulation) ──

static void _capture_flush_line() {
    _line_buf[_line_pos] = '\0';

    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        strncpy(_ring[_ring_head], _line_buf, MAX_LINE_LEN - 1);
        _ring[_ring_head][MAX_LINE_LEN - 1] = '\0';
        _ring_head = (_ring_head + 1) % MAX_LINES;
        if (_ring_count < MAX_LINES) _ring_count++;
        xSemaphoreGive(_mutex);
    }

    // Fire callback outside mutex to avoid deadlock
    if (_callback) {
        _callback(_line_buf, _callback_data);
    }

    _line_pos = 0;
}

static void _capture_write_byte(uint8_t c) {
    if (c == '\n' || c == '\r') {
        if (_line_pos > 0) {
            _capture_flush_line();
        }
    } else if (_line_pos < MAX_LINE_LEN - 1) {
        _line_buf[_line_pos++] = (char)c;
    }
}

static void _capture_write_bytes(const uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        _capture_write_byte(buf[i]);
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

void serial_capture::init() {
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) {
        DBG_INFO(TAG, "Failed to create mutex");
        return;
    }

    // Serial capture initialized — lines are pushed via captureLog() or
    // the debug_log output hook calling serial_capture callbacks.
    // No Arduino Serial redirection needed with ESP-IDF native.

    DBG_INFO(TAG, "Serial capture initialized (%d line ring buffer)", MAX_LINES);
}

int serial_capture::getLines(char* buf, size_t buf_size, int max_lines) {
    if (!_mutex || !buf || buf_size == 0) return 0;

    int count = 0;
    size_t pos = 0;

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        int available = _ring_count;
        int to_read = (max_lines < available) ? max_lines : available;

        // Start from oldest available line
        int start = (_ring_head - available + MAX_LINES) % MAX_LINES;
        // Skip to get only the last `to_read` lines
        start = (start + available - to_read + MAX_LINES) % MAX_LINES;

        for (int i = 0; i < to_read; i++) {
            int idx = (start + i) % MAX_LINES;
            size_t len = strlen(_ring[idx]);

            if (pos + len + 1 >= buf_size) break;

            memcpy(buf + pos, _ring[idx], len);
            buf[pos + len] = '\0';
            pos += len + 1;  // include null terminator
            count++;
        }

        xSemaphoreGive(_mutex);
    }

    return count;
}

int serial_capture::getLinesJson(char* buf, size_t buf_size, int max_lines) {
    if (!_mutex || !buf || buf_size < 32) return 0;

    int pos = 0;
    int count = 0;

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        int available = _ring_count;
        int to_read = (max_lines < available) ? max_lines : available;

        int start = (_ring_head - available + MAX_LINES) % MAX_LINES;
        start = (start + available - to_read + MAX_LINES) % MAX_LINES;

        pos = snprintf(buf, buf_size, "{\"count\":%d,\"lines\":[", to_read);

        for (int i = 0; i < to_read && pos < (int)buf_size - 20; i++) {
            int idx = (start + i) % MAX_LINES;
            if (i > 0) buf[pos++] = ',';
            buf[pos++] = '"';
            for (int c = 0; _ring[idx][c] && pos < (int)buf_size - 4; c++) {
                char ch = _ring[idx][c];
                if (ch == '"')       { buf[pos++] = '\\'; buf[pos++] = '"'; }
                else if (ch == '\\') { buf[pos++] = '\\'; buf[pos++] = '\\'; }
                else if (ch == '\n') { buf[pos++] = '\\'; buf[pos++] = 'n'; }
                else if (ch == '\r') { /* skip */ }
                else if ((unsigned char)ch < 0x20) { buf[pos++] = '.'; }
                else { buf[pos++] = ch; }
            }
            buf[pos++] = '"';
            count++;
        }

        xSemaphoreGive(_mutex);
    } else {
        pos = snprintf(buf, buf_size, "{\"count\":0,\"lines\":[");
    }

    pos += snprintf(buf + pos, buf_size - pos, "]}");
    return pos;
}

void serial_capture::onLine(SerialLineCallback cb, void* user_data) {
    _callback = cb;
    _callback_data = user_data;
}

void serial_capture::injectCommand(const char* cmd) {
    if (!cmd) return;

    int next = (_inject_head + 1) % MAX_INJECT_CMDS;
    if (next == _inject_tail) return;  // buffer full, drop

    strncpy(_inject_ring[_inject_head], cmd, MAX_INJECT_LEN - 1);
    _inject_ring[_inject_head][MAX_INJECT_LEN - 1] = '\0';
    _inject_head = next;

    // Store in inject ring and let ws_bridge tick pull from it and
    // dispatch to ServiceRegistry directly. Cannot push to Serial RX
    // on USB CDC — the inject ring is the canonical path.
}

// Called by ws_bridge to drain injected commands (since we can't push to
// Serial RX on USB CDC). Returns null when empty.
const char* _serial_capture_drain_inject() {
    if (_inject_head == _inject_tail) return nullptr;
    const char* cmd = _inject_ring[_inject_tail];
    _inject_tail = (_inject_tail + 1) % MAX_INJECT_CMDS;
    return cmd;
}

// Write a line through the capture pipeline (for echoing command responses
// that bypass Serial.printf, e.g. from ServiceRegistry::dispatchCommand)
void _serial_capture_write_line(const char* line) {
    _capture_write_bytes((const uint8_t*)line, strlen(line));
    _capture_write_byte((uint8_t)'\n');
}
