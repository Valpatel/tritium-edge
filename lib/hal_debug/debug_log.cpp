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
void DebugLog::flushSDLog() {}

void DebugLog::output(const char* buf, size_t len) {
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
}

void DebugLog::log(uint8_t level, const char* tag, const char* fmt, ...) {
    if (level > _level) return;
    if (!_initialized) init(DBG_BACKEND_SERIAL);

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
#include <sys/stat.h>

static WiFiServer* _tcp_server = nullptr;
static WiFiClient _tcp_clients[3];  // max 3 simultaneous TCP debug clients
static const int MAX_TCP_CLIENTS = 3;

// SD card log buffering
static constexpr size_t SD_LOG_BUF_SIZE = 4096;
static constexpr size_t SD_LOG_MAX_FILE = 512 * 1024;  // 512KB per log file
static constexpr int    SD_LOG_MAX_FILES = 5;           // keep 5 rotated files
static constexpr unsigned long SD_LOG_FLUSH_MS = 5000;  // flush every 5s
static char* _sd_log_buf = nullptr;
static size_t _sd_log_pos = 0;
static unsigned long _sd_last_flush = 0;
static const char* SD_LOG_PATH = "/sdcard/logs/system.log";
static const char* SD_LOG_DIR = "/sdcard/logs";
static bool _sd_log_ready = false;

static void sd_log_ensure_dir() {
    struct stat st;
    if (stat(SD_LOG_DIR, &st) != 0) {
        mkdir(SD_LOG_DIR, 0755);
    }
}

static void sd_log_rotate() {
    struct stat st;
    if (stat(SD_LOG_PATH, &st) != 0) return;
    if ((size_t)st.st_size < SD_LOG_MAX_FILE) return;

    // Rotate: system.log.4 -> delete, .3->.4, .2->.3, .1->.2, .log->.1
    char old_path[80], new_path[80];
    snprintf(old_path, sizeof(old_path), "%s.%d", SD_LOG_PATH, SD_LOG_MAX_FILES);
    remove(old_path);
    for (int i = SD_LOG_MAX_FILES - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", SD_LOG_PATH, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", SD_LOG_PATH, i + 1);
        rename(old_path, new_path);
    }
    snprintf(new_path, sizeof(new_path), "%s.1", SD_LOG_PATH);
    rename(SD_LOG_PATH, new_path);
}

static void sd_log_flush() {
    if (!_sd_log_ready || !_sd_log_buf || _sd_log_pos == 0) return;

    sd_log_rotate();

    FILE* f = fopen(SD_LOG_PATH, "a");
    if (f) {
        fwrite(_sd_log_buf, 1, _sd_log_pos, f);
        fclose(f);
    }
    _sd_log_pos = 0;
    _sd_last_flush = millis();
}

static void sd_log_write(const char* data, size_t len) {
    if (!_sd_log_ready) {
        // Check if SD card is mounted
        struct stat st;
        if (stat("/sdcard", &st) == 0) {
            sd_log_ensure_dir();
            if (!_sd_log_buf) {
                _sd_log_buf = (char*)malloc(SD_LOG_BUF_SIZE);
            }
            if (_sd_log_buf) {
                _sd_log_ready = true;
                _sd_log_pos = 0;
                _sd_last_flush = millis();
            }
        }
        if (!_sd_log_ready) return;
    }

    // Strip ANSI color codes for clean log files
    size_t clean_len = 0;
    char* clean = (char*)alloca(len + 1);
    bool in_escape = false;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\033') { in_escape = true; continue; }
        if (in_escape) { if (data[i] == 'm') in_escape = false; continue; }
        clean[clean_len++] = data[i];
    }

    // Buffer the cleaned data
    if (_sd_log_pos + clean_len >= SD_LOG_BUF_SIZE) {
        sd_log_flush();
    }
    if (clean_len < SD_LOG_BUF_SIZE) {
        memcpy(_sd_log_buf + _sd_log_pos, clean, clean_len);
        _sd_log_pos += clean_len;
    }

    // Time-based flush
    if (millis() - _sd_last_flush >= SD_LOG_FLUSH_MS) {
        sd_log_flush();
    }
}

extern "C" void debug_ble_send(const char* data, size_t len) __attribute__((weak));

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
    WiFiClient newClient = _tcp_server->accept();
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
        if (debug_ble_send) {
            debug_ble_send(buf, len);
        }
    }

    // SD card log file
    if (_backends & DBG_BACKEND_SDCARD) {
        sd_log_write(buf, len);
    }
}

void DebugLog::log(uint8_t level, const char* tag, const char* fmt, ...) {
    if (level > _level) return;
    if (!_initialized) init(DBG_BACKEND_SERIAL | DBG_BACKEND_SDCARD);

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

void DebugLog::flushSDLog() {
    sd_log_flush();
}

// Weak BLE bridge — override in app or hal_ble if BLE debug is desired
extern "C" __attribute__((weak)) void debug_ble_send(const char* data, size_t len) {
    (void)data; (void)len;
}

#endif
