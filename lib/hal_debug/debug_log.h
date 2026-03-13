#pragma once
// Multi-backend debug logging library.
// Outputs to USB serial, TCP, and/or BLE simultaneously.
//
// Usage:
//   DBG_INFO("sensor", "Temperature: %.1f", temp);
//   DBG_WARN("wifi", "Signal weak: %d dBm", rssi);
//   DBG_ERROR("display", "Init failed!");
//
// Log levels can be set at compile time (-DDBG_LEVEL=DBG_LVL_WARN)
// or at runtime via DebugLog::setLevel().

#include <cstdint>
#include <cstddef>

// Log levels
#define DBG_LVL_NONE    0
#define DBG_LVL_ERROR   1
#define DBG_LVL_WARN    2
#define DBG_LVL_INFO    3
#define DBG_LVL_DEBUG   4
#define DBG_LVL_VERBOSE 5

// Compile-time default level
#ifndef DBG_LEVEL
#define DBG_LEVEL DBG_LVL_INFO
#endif

// Output backends (bitfield)
#define DBG_BACKEND_SERIAL  (1 << 0)
#define DBG_BACKEND_TCP     (1 << 1)
#define DBG_BACKEND_BLE     (1 << 2)
#define DBG_BACKEND_SDCARD  (1 << 3)

class DebugLog {
public:
    static void init(uint32_t backends = DBG_BACKEND_SERIAL);

    // Enable/disable backends at runtime
    static void enableBackend(uint32_t backend);
    static void disableBackend(uint32_t backend);

    // Runtime log level
    static void setLevel(uint8_t level);
    static uint8_t getLevel();

    // TCP server for wireless debugging
    static void startTcpServer(uint16_t port = 9999);
    static void stopTcpServer();

    // Core logging (use macros below instead)
    static void log(uint8_t level, const char* tag, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)));

    // Process pending TCP connections (call from loop or FreeRTOS task)
    static void poll();

    // Flush SD card log buffer (call periodically or before reboot)
    static void flushSDLog();

private:
    static void output(const char* buf, size_t len);

    static uint32_t _backends;
    static uint8_t _level;
    static bool _initialized;
};

// Convenience macros — compile-time filtered, zero cost when level is below threshold
#if DBG_LEVEL >= DBG_LVL_ERROR
#define DBG_ERROR(tag, fmt, ...) DebugLog::log(DBG_LVL_ERROR, tag, fmt, ##__VA_ARGS__)
#else
#define DBG_ERROR(tag, fmt, ...) ((void)0)
#endif

#if DBG_LEVEL >= DBG_LVL_WARN
#define DBG_WARN(tag, fmt, ...)  DebugLog::log(DBG_LVL_WARN, tag, fmt, ##__VA_ARGS__)
#else
#define DBG_WARN(tag, fmt, ...)  ((void)0)
#endif

#if DBG_LEVEL >= DBG_LVL_INFO
#define DBG_INFO(tag, fmt, ...)  DebugLog::log(DBG_LVL_INFO, tag, fmt, ##__VA_ARGS__)
#else
#define DBG_INFO(tag, fmt, ...)  ((void)0)
#endif

#if DBG_LEVEL >= DBG_LVL_DEBUG
#define DBG_DEBUG(tag, fmt, ...) DebugLog::log(DBG_LVL_DEBUG, tag, fmt, ##__VA_ARGS__)
#else
#define DBG_DEBUG(tag, fmt, ...) ((void)0)
#endif

#if DBG_LEVEL >= DBG_LVL_VERBOSE
#define DBG_VERBOSE(tag, fmt, ...) DebugLog::log(DBG_LVL_VERBOSE, tag, fmt, ##__VA_ARGS__)
#else
#define DBG_VERBOSE(tag, fmt, ...) ((void)0)
#endif
