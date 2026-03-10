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
#include "tritium_compat.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>

static int _tcp_server_fd = -1;
static int _tcp_client_fds[3] = {-1, -1, -1};
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
    if (_tcp_server_fd >= 0) return;
    _tcp_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (_tcp_server_fd < 0) return;

    int opt = 1;
    setsockopt(_tcp_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // Set non-blocking
    int flags = fcntl(_tcp_server_fd, F_GETFL, 0);
    fcntl(_tcp_server_fd, F_SETFL, flags | O_NONBLOCK);
    // Enable TCP_NODELAY
    setsockopt(_tcp_server_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(_tcp_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(_tcp_server_fd, 3) < 0) {
        close(_tcp_server_fd);
        _tcp_server_fd = -1;
        return;
    }
    _backends |= DBG_BACKEND_TCP;
}

void DebugLog::stopTcpServer() {
    if (_tcp_server_fd < 0) return;
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (_tcp_client_fds[i] >= 0) {
            close(_tcp_client_fds[i]);
            _tcp_client_fds[i] = -1;
        }
    }
    close(_tcp_server_fd);
    _tcp_server_fd = -1;
    _backends &= ~DBG_BACKEND_TCP;
}

void DebugLog::poll() {
    if (_tcp_server_fd < 0) return;

    // Accept new clients (non-blocking)
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int new_fd = accept(_tcp_server_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (new_fd >= 0) {
        // Set new client non-blocking
        int flags = fcntl(new_fd, F_GETFL, 0);
        fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);
        bool placed = false;
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (_tcp_client_fds[i] < 0) {
                _tcp_client_fds[i] = new_fd;
                placed = true;
                break;
            }
        }
        if (!placed) close(new_fd);
    }

    // Clean up disconnected clients
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (_tcp_client_fds[i] >= 0) {
            char tmp;
            int r = recv(_tcp_client_fds[i], &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r == 0) {  // peer closed
                close(_tcp_client_fds[i]);
                _tcp_client_fds[i] = -1;
            }
        }
    }
}

void DebugLog::output(const char* buf, size_t len) {
    // USB Serial
    if (_backends & DBG_BACKEND_SERIAL) {
        Serial.write((const uint8_t*)buf, len);
    }

    // TCP clients
    if ((_backends & DBG_BACKEND_TCP) && _tcp_server_fd >= 0) {
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (_tcp_client_fds[i] >= 0) {
                if (send(_tcp_client_fds[i], buf, len, MSG_DONTWAIT) < 0) {
                    close(_tcp_client_fds[i]);
                    _tcp_client_fds[i] = -1;
                }
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
