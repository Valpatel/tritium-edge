// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "network_quality.h"
#include "debug_log.h"

#include <cstdio>
#include <cstring>

static constexpr const char* TAG = "net-quality";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace network_quality {

void init() {}
void tick() {}

NetworkStats get_stats() {
    return {0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0, true};
}

int to_json(char* buf, int buf_size) {
    return snprintf(buf, buf_size,
        "{\"avg_latency_ms\":0,\"packet_loss_pct\":0,\"stable\":true}");
}

}  // namespace network_quality

// ============================================================================
// ESP32 — real network quality monitoring
// ============================================================================
#else

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

namespace network_quality {

// Rolling window for latency measurements
static constexpr int WINDOW_SIZE = 10;
static float _latencies[WINDOW_SIZE] = {};
static int _latency_idx = 0;
static int _latency_count = 0;

// Counters
static uint32_t _ping_attempts = 0;
static uint32_t _ping_failures = 0;
static uint32_t _reconnect_count = 0;
static uint32_t _dns_failures = 0;
static bool _was_connected = false;
static uint32_t _connection_start_ms = 0;
static bool _initialized = false;

// Target host for latency test (MQTT broker or provisioned server)
static char _test_host[128] = {};
static uint16_t _test_port = 1883;  // Default MQTT port

void init() {
    if (_initialized) return;
    _initialized = true;

    memset(_latencies, 0, sizeof(_latencies));
    _latency_idx = 0;
    _latency_count = 0;
    _ping_attempts = 0;
    _ping_failures = 0;
    _reconnect_count = 0;
    _dns_failures = 0;
    _was_connected = WiFi.isConnected();
    _connection_start_ms = _was_connected ? millis() : 0;

    DBG_INFO(TAG, "Network quality monitor initialized");
}

void set_target(const char* host, uint16_t port) {
    if (host && host[0] != '\0') {
        strncpy(_test_host, host, sizeof(_test_host) - 1);
        _test_host[sizeof(_test_host) - 1] = '\0';
    }
    _test_port = port;
}

void tick() {
    if (!_initialized) return;

    // Track reconnections
    bool connected = WiFi.isConnected();
    if (connected && !_was_connected) {
        _reconnect_count++;
        _connection_start_ms = millis();
        DBG_DEBUG(TAG, "WiFi reconnected (count=%u)", (unsigned)_reconnect_count);
    }
    _was_connected = connected;

    if (!connected) return;

    // If no test host configured, try gateway
    const char* host = _test_host;
    if (host[0] == '\0') {
        // Use gateway IP as fallback
        IPAddress gw = WiFi.gatewayIP();
        if (gw == IPAddress(0, 0, 0, 0)) return;
        static char gw_str[20];
        snprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
        host = gw_str;
    }

    // Measure TCP connect latency to target host
    _ping_attempts++;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(_test_port);

    // Resolve hostname
    struct hostent* he = gethostbyname(host);
    if (!he) {
        _dns_failures++;
        _ping_failures++;
        return;
    }
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        _ping_failures++;
        return;
    }

    // Set connect timeout (2 seconds)
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    uint32_t start = millis();
    int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    uint32_t elapsed = millis() - start;

    close(sock);

    if (result == 0) {
        float latency = (float)elapsed;
        _latencies[_latency_idx] = latency;
        _latency_idx = (_latency_idx + 1) % WINDOW_SIZE;
        if (_latency_count < WINDOW_SIZE) _latency_count++;
    } else {
        _ping_failures++;
    }
}

NetworkStats get_stats() {
    NetworkStats stats = {};

    // Compute latency stats from rolling window
    if (_latency_count > 0) {
        float sum = 0;
        stats.min_latency_ms = 999999.0f;
        stats.max_latency_ms = 0.0f;

        for (int i = 0; i < _latency_count; i++) {
            float v = _latencies[i];
            sum += v;
            if (v < stats.min_latency_ms) stats.min_latency_ms = v;
            if (v > stats.max_latency_ms) stats.max_latency_ms = v;
        }
        stats.avg_latency_ms = sum / (float)_latency_count;
    }

    // Packet loss
    if (_ping_attempts > 0) {
        stats.packet_loss_pct = ((float)_ping_failures / (float)_ping_attempts) * 100.0f;
    }

    stats.reconnect_count = _reconnect_count;
    stats.dns_failures = _dns_failures;
    stats.rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;

    if (_connection_start_ms > 0 && WiFi.isConnected()) {
        stats.uptime_ms = millis() - _connection_start_ms;
    }

    // Stability check: loss < 5% and avg latency < 500ms
    stats.stable = (stats.packet_loss_pct < 5.0f) &&
                   (stats.avg_latency_ms < 500.0f || _latency_count == 0);

    return stats;
}

int to_json(char* buf, int buf_size) {
    NetworkStats s = get_stats();
    return snprintf(buf, buf_size,
        "{\"avg_latency_ms\":%.1f,\"max_latency_ms\":%.1f,"
        "\"min_latency_ms\":%.1f,\"packet_loss_pct\":%.1f,"
        "\"reconnects\":%u,\"dns_failures\":%u,"
        "\"rssi\":%d,\"wifi_uptime_s\":%lu,\"stable\":%s}",
        s.avg_latency_ms, s.max_latency_ms,
        s.min_latency_ms, s.packet_loss_pct,
        (unsigned)s.reconnect_count, (unsigned)s.dns_failures,
        (int)s.rssi, (unsigned long)(s.uptime_ms / 1000),
        s.stable ? "true" : "false");
}

}  // namespace network_quality

#endif  // SIMULATOR
