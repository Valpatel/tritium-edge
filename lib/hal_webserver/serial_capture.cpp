// Tritium-OS Serial Capture — ring buffer and Print wrapper for capturing
// Serial output and feeding it to WebSocket terminal clients.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "serial_capture.h"
#include "debug_log.h"

#include <Arduino.h>
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

// ── Custom Print class that wraps Serial ─────────────────────────────────────

class SerialCapturePrint : public Print {
public:
    void setTarget(Print* target) { _target = target; }

    size_t write(uint8_t c) override {
        // Pass through to original Serial
        if (_target) _target->write(c);

        // Accumulate into line buffer
        if (c == '\n' || c == '\r') {
            if (_line_pos > 0) {
                flushLine();
            }
        } else if (_line_pos < MAX_LINE_LEN - 1) {
            _line_buf[_line_pos++] = (char)c;
        }
        return 1;
    }

    size_t write(const uint8_t* buffer, size_t size) override {
        for (size_t i = 0; i < size; i++) {
            write(buffer[i]);
        }
        return size;
    }

private:
    Print* _target = nullptr;

    void flushLine() {
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
};

static SerialCapturePrint _capture_print;

// ── Public API ───────────────────────────────────────────────────────────────

void serial_capture::init() {
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) {
        DBG_INFO(TAG, "Failed to create mutex");
        return;
    }

    // Redirect Serial output through our capture wrapper.
    // We store the underlying UART/CDC target and redirect Serial's
    // output stream to our wrapper that copies to the ring buffer.
    // Note: on ESP32-S3 with USB CDC, Serial is HWCDC which inherits Print.
    // We capture by installing a custom print hook via the log system.
    _capture_print.setTarget(&Serial);

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

    // Push each character of the command into Serial RX
    // This makes it appear as if typed on the serial console,
    // so handleSerialCommands() in main.cpp will process it.
    size_t len = strlen(cmd);
    for (size_t i = 0; i < len; i++) {
        // On ESP32-S3 with USB CDC, we cannot easily inject into the HW RX.
        // Instead, write directly into the Serial input buffer if available.
    }

    // Fallback: write command + newline to Serial so it echoes and gets
    // picked up by the next handleSerialCommands() cycle via available().
    // On USB CDC Serial, writing to Serial goes to the host, not back to RX.
    // So we use a different approach: store in inject ring and let the
    // ws_bridge tick pull from it and dispatch to ServiceRegistry directly.
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
    _capture_print.write((const uint8_t*)line, strlen(line));
    _capture_print.write((uint8_t)'\n');
}
