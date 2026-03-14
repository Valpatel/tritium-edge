// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_meshtastic.h"
#include "debug_log.h"

#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static bool s_initialized = false;
static hal_meshtastic::MeshtasticConfig s_config;
static hal_meshtastic::MessageCallback s_msg_cb = nullptr;

static char s_last_msg[hal_meshtastic::MAX_MSG_LEN + 1] = {0};
static uint32_t s_last_msg_time = 0;
static uint32_t s_msgs_sent = 0;
static uint32_t s_msgs_received = 0;
static uint32_t s_init_time = 0;

static hal_meshtastic::MeshtasticNode s_nodes[hal_meshtastic::MAX_NODES];
static int s_node_count = 0;

// ---------------------------------------------------------------------------
// Platform-specific serial (stub when no hardware)
// ---------------------------------------------------------------------------

#ifdef SIMULATOR

// Simulator stubs — no real UART

static bool serial_open(int /*uart*/, int /*rx*/, int /*tx*/, uint32_t /*baud*/) {
    DBG_INFO("meshtastic", "Simulator: serial bridge stub initialized");
    return true;
}
static void serial_close() {}
static bool serial_available() { return false; }
static int serial_read_line(char* /*buf*/, size_t /*size*/) { return 0; }
static bool serial_write(const char* /*text*/) { return true; }

#elif defined(ARDUINO)

#include <Arduino.h>

static HardwareSerial* s_serial = nullptr;

static bool serial_open(int uart_num, int rx, int tx, uint32_t baud) {
    if (rx < 0 || tx < 0) {
        DBG_WARN("meshtastic", "RX/TX pins not configured");
        return false;
    }
    switch (uart_num) {
        case 0: s_serial = &Serial; break;
        case 1: s_serial = &Serial1; break;
#if SOC_UART_NUM > 2
        case 2: s_serial = &Serial2; break;
#endif
        default:
            DBG_WARN("meshtastic", "Invalid UART number: %d", uart_num);
            return false;
    }
    s_serial->begin(baud, SERIAL_8N1, rx, tx);
    DBG_INFO("meshtastic", "Serial bridge opened: UART%d RX=%d TX=%d @ %lu baud",
             uart_num, rx, tx, (unsigned long)baud);
    return true;
}

static void serial_close() {
    if (s_serial) {
        s_serial->end();
        s_serial = nullptr;
    }
}

static bool serial_available() {
    return s_serial && s_serial->available() > 0;
}

static int serial_read_line(char* buf, size_t size) {
    if (!s_serial || !s_serial->available()) return 0;
    size_t i = 0;
    while (i < size - 1 && s_serial->available()) {
        char c = (char)s_serial->read();
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

static bool serial_write(const char* text) {
    if (!s_serial) return false;
    s_serial->println(text);
    return true;
}

#else

// Fallback stubs for non-Arduino, non-simulator builds
static bool serial_open(int, int, int, uint32_t) { return false; }
static void serial_close() {}
static bool serial_available() { return false; }
static int serial_read_line(char*, size_t) { return 0; }
static bool serial_write(const char*) { return false; }

#endif

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace hal_meshtastic {

bool init(const MeshtasticConfig& config) {
    if (s_initialized) {
        DBG_WARN("meshtastic", "Already initialized");
        return true;
    }
    s_config = config;
    if (!serial_open(config.uart_num, config.rx_pin, config.tx_pin, config.baud)) {
        return false;
    }
    s_initialized = true;
    s_init_time = 0;
#ifdef ARDUINO
    s_init_time = millis();
#endif
    s_msgs_sent = 0;
    s_msgs_received = 0;
    s_node_count = 0;
    s_last_msg[0] = '\0';
    s_last_msg_time = 0;
    DBG_INFO("meshtastic", "HAL initialized");
    return true;
}

void shutdown() {
    if (!s_initialized) return;
    serial_close();
    s_initialized = false;
    s_msg_cb = nullptr;
    DBG_INFO("meshtastic", "HAL shut down");
}

void tick() {
    if (!s_initialized || !s_config.auto_poll) return;

    // Poll for incoming text messages
    while (serial_available()) {
        char line[MAX_MSG_LEN + 1];
        int len = serial_read_line(line, sizeof(line));
        if (len <= 0) break;

        // Store as last message
        strncpy(s_last_msg, line, MAX_MSG_LEN);
        s_last_msg[MAX_MSG_LEN] = '\0';
#ifdef ARDUINO
        s_last_msg_time = millis();
#endif
        s_msgs_received++;

        DBG_INFO("meshtastic", "RX: %s", line);

        // Invoke callback
        if (s_msg_cb) {
            s_msg_cb(line, 0xFFFFFFFF);  // Node ID unknown in TEXTMSG mode
        }
    }
}

bool is_connected() {
    return s_initialized;  // In TEXTMSG mode, we can't probe link health
}

bool is_initialized() {
    return s_initialized;
}

bool send_text(const char* text, uint32_t dest) {
    if (!s_initialized || !text) return false;
    (void)dest;  // TEXTMSG mode always broadcasts

    bool ok = serial_write(text);
    if (ok) {
        s_msgs_sent++;
        DBG_INFO("meshtastic", "TX: %s", text);
    } else {
        DBG_WARN("meshtastic", "TX failed");
    }
    return ok;
}

void on_message(MessageCallback callback) {
    s_msg_cb = callback;
}

const char* get_last_message() {
    if (s_last_msg[0] == '\0') return nullptr;
    return s_last_msg;
}

uint32_t get_last_message_time() {
    return s_last_msg_time;
}

int get_nodes(MeshtasticNode* out, int max_count) {
    if (!out || max_count <= 0) return 0;
    int count = (s_node_count < max_count) ? s_node_count : max_count;
    memcpy(out, s_nodes, count * sizeof(MeshtasticNode));
    return count;
}

int get_node_count() {
    return s_node_count;
}

int get_nodes_json(char* buf, size_t size) {
    if (!buf || size < 3) return 0;
    int pos = snprintf(buf, size, "[");
    for (int i = 0; i < s_node_count && pos < (int)size - 128; i++) {
        const auto& n = s_nodes[i];
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, size - pos,
            "{\"id\":\"%08X\",\"short\":\"%s\",\"long\":\"%s\","
            "\"rssi\":%d,\"snr\":%.1f,"
            "\"gps\":%s,\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f}",
            n.node_id, n.short_name, n.long_name,
            n.rssi, n.snr,
            n.has_gps ? "true" : "false",
            n.latitude, n.longitude, n.altitude);
    }
    pos += snprintf(buf + pos, size - pos, "]");
    return pos;
}

uint32_t get_messages_sent() {
    return s_msgs_sent;
}

uint32_t get_messages_received() {
    return s_msgs_received;
}

uint32_t get_uptime_ms() {
#ifdef ARDUINO
    return s_initialized ? (millis() - s_init_time) : 0;
#else
    return 0;
#endif
}

TestResult runTest() {
    TestResult r = {};
    uint32_t t0 = 0;
#ifdef ARDUINO
    t0 = millis();
#endif

    r.init_ok = s_initialized;
    r.send_ok = false;
    r.node_count = s_node_count;
    r.messages_sent = s_msgs_sent;
    r.messages_received = s_msgs_received;

    if (s_initialized) {
        r.send_ok = send_text("TRITIUM_TEST");
        r.status = "active";
    } else {
        r.status = "not_initialized";
    }

#ifdef ARDUINO
    r.test_duration_ms = millis() - t0;
#endif
    return r;
}

}  // namespace hal_meshtastic
