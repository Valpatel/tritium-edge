#include "debug_log.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

uint32_t DebugLog::_backends = DBG_BACKEND_SERIAL;
uint8_t DebugLog::_level = DBG_LEVEL;
bool DebugLog::_initialized = false;

static const char* level_names[] = {
    "",      // NONE
    "ERROR", // 1
    "WARN",  // 2
    "INFO",  // 3
    "DEBUG", // 4
    "VERB",  // 5
};

static const char* level_colors[] = {
    "",         // NONE
    "\033[31m", // ERROR - red
    "\033[33m", // WARN  - yellow
    "\033[32m", // INFO  - green
    "\033[36m", // DEBUG - cyan
    "\033[90m", // VERB  - gray
};

#ifdef SIMULATOR
// ---- Desktop simulator implementation ----
#include <ctime>

void DebugLog::init(uint32_t backends) {
    _backends = backends;
    _initialized = true;
}

void DebugLog::enableBackend(uint32_t backend)  { _backends |= backend; }
void DebugLog::disableBackend(uint32_t backend) { _backends &= ~backend; }
void DebugLog::setLevel(uint8_t level) { _level = level; }
uint8_t DebugLog::getLevel() { return _level; }
void DebugLog::startTcpServer(uint16_t) {}
void DebugLog::stopTcpServer() {}
void DebugLog::poll() {}

void DebugLog::output(const char* buf, size_t len) {
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
}

void DebugLog::log(uint8_t level, const char* tag, const char* fmt, ...) {
    if (level > _level || !_initialized) return;

    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    // Timestamp
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    char buf[384];
    int len = snprintf(buf, sizeof(buf), "%s[%lu][%s][%s] %s\033[0m\n",
                       level_colors[level], ms, level_names[level], tag, msg);
    if (len > 0) output(buf, len);
}

#else
// ---- ESP32 implementation ----
#include <Arduino.h>
#include <WiFi.h>

static WiFiServer* _tcp_server = nullptr;
static WiFiClient _tcp_clients[3];  // max 3 simultaneous TCP debug clients
static const int MAX_TCP_CLIENTS = 3;

void DebugLog::init(uint32_t backends) {
    _backends = backends;
    _initialized = true;
}

void DebugLog::enableBackend(uint32_t backend)  { _backends |= backend; }
void DebugLog::disableBackend(uint32_t backend) { _backends &= ~backend; }
void DebugLog::setLevel(uint8_t level) { _level = level; }
uint8_t DebugLog::getLevel() { return _level; }

void DebugLog::startTcpServer(uint16_t port) {
    if (_tcp_server) return;
    _tcp_server = new WiFiServer(port);
    _tcp_server->begin();
    _tcp_server->setNoDelay(true);
    _backends |= DBG_BACKEND_TCP;
}

void DebugLog::stopTcpServer() {
    if (!_tcp_server) return;
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (_tcp_clients[i]) _tcp_clients[i].stop();
    }
    _tcp_server->stop();
    delete _tcp_server;
    _tcp_server = nullptr;
    _backends &= ~DBG_BACKEND_TCP;
}

void DebugLog::poll() {
    if (!_tcp_server) return;

    // Accept new clients
    WiFiClient newClient = _tcp_server->available();
    if (newClient) {
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (!_tcp_clients[i] || !_tcp_clients[i].connected()) {
                _tcp_clients[i] = newClient;
                break;
            }
        }
    }

    // Clean up disconnected
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (_tcp_clients[i] && !_tcp_clients[i].connected()) {
            _tcp_clients[i].stop();
        }
    }
}

void DebugLog::output(const char* buf, size_t len) {
    // USB Serial
    if (_backends & DBG_BACKEND_SERIAL) {
        Serial.write(buf, len);
    }

    // TCP clients
    if ((_backends & DBG_BACKEND_TCP) && _tcp_server) {
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (_tcp_clients[i] && _tcp_clients[i].connected()) {
                _tcp_clients[i].write(buf, len);
            }
        }
    }

    // BLE (via Nordic UART Service)
    if (_backends & DBG_BACKEND_BLE) {
        // Use BleManager singleton if available
        // Kept as a separate include to avoid hard dependency
        extern void debug_ble_send(const char* data, size_t len) __attribute__((weak));
        if (debug_ble_send) {
            debug_ble_send(buf, len);
        }
    }
}

void DebugLog::log(uint8_t level, const char* tag, const char* fmt, ...) {
    if (level > _level || !_initialized) return;

    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    unsigned long ms = millis();

    char buf[384];
    int len = snprintf(buf, sizeof(buf), "%s[%lu][%s][%s] %s\033[0m\n",
                       level_colors[level], ms, level_names[level], tag, msg);
    if (len > 0) output(buf, (size_t)len);
}

// Weak BLE bridge — override in app or hal_ble if BLE debug is desired
__attribute__((weak)) void debug_ble_send(const char* data, size_t len) {
    (void)data; (void)len;
}

#endif
