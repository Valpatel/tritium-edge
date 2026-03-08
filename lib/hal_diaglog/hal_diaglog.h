// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#pragma once

// Persistent diagnostic event log — survives reboots via LittleFS ring buffer.
//
// Stores up to MAX_EVENTS fixed-size DiagEvent records in /diag/events.bin.
// Each record is 128 bytes for O(1) seeking. A 128-byte header tracks the
// write index, total count, and boot count.
//
// Usage:
//   #include "hal_diaglog.h"
//
//   diaglog_init();                              // call after LittleFS is mounted
//   DIAG_INFO("wifi", 100, "Connected to AP");
//   DIAG_WARN("power", 201, "Battery low");
//   DIAG_ERROR("i2c", 300, "NACK from 0x34");
//   DIAG_CRITICAL("system", 500, "Watchdog triggered");
//
//   // Upload to server:
//   char buf[4096];
//   int len = diaglog_get_json(buf, sizeof(buf), 0, 50);
//   // ... POST buf to fleet server ...
//   diaglog_clear();

#include <cstdint>
#include <cstddef>

// ── Configuration ────────────────────────────────────────────────────────────

static constexpr uint16_t DIAGLOG_MAX_EVENTS = 500;
static constexpr const char* DIAGLOG_DIR      = "/diag";
static constexpr const char* DIAGLOG_PATH     = "/diag/events.bin";

// ── Severity levels ──────────────────────────────────────────────────────────

enum class DiagSeverity : uint8_t {
    INFO     = 0,
    WARN     = 1,
    ERR      = 2,   // ERROR collides with Windows macro
    CRITICAL = 3
};

// ── Persistent event record (128 bytes, fixed-size for file seeking) ─────────

struct DiagEvent {
    uint32_t epoch;              // UTC seconds (0 if NTP unavailable)
    uint32_t uptime_ms;          // millis() at time of event
    DiagSeverity severity;       // INFO / WARN / ERR / CRITICAL
    char     subsystem[16];      // "wifi", "power", "i2c", etc.
    uint16_t code;               // Subsystem-specific event code
    char     message[80];        // Human-readable description
    float    value;              // Optional numeric context
    uint8_t  _pad[128 - 4 - 4 - 1 - 16 - 2 - 80 - 4]; // pad to 128 bytes
};

static_assert(sizeof(DiagEvent) == 128, "DiagEvent must be exactly 128 bytes");

// ── File header (128 bytes, stored at offset 0) ─────────────────────────────

struct DiagLogHeader {
    uint32_t magic;              // 0xD1A6L06 marker
    uint16_t version;            // Format version (1)
    uint16_t max_events;         // Capacity
    uint32_t write_index;        // Next slot to write (modulo max_events)
    uint32_t total_count;        // Lifetime event count (may exceed max_events)
    uint32_t boot_count;         // Incremented each init()
    uint8_t  _pad[128 - 4 - 2 - 2 - 4 - 4 - 4]; // pad to 128 bytes
};

static_assert(sizeof(DiagLogHeader) == 128, "DiagLogHeader must be exactly 128 bytes");

static constexpr uint32_t DIAGLOG_MAGIC = 0xD1A6106E;

// ── Public API ───────────────────────────────────────────────────────────────

/// Initialize the persistent log. Creates file if missing, increments boot
/// count, and auto-logs a BOOT event with the reset reason.
/// LittleFS must already be mounted (via FsHAL::init() or LittleFS.begin()).
bool diaglog_init();

/// Write an event to the ring buffer. Thread-safe (mutex-protected).
/// @param severity  Event severity level.
/// @param subsystem Short subsystem name (max 15 chars).
/// @param code      Subsystem-specific event code.
/// @param message   Human-readable message (max 79 chars).
/// @param value     Optional numeric value (default 0).
void diaglog_write(DiagSeverity severity, const char* subsystem,
                   uint16_t code, const char* message, float value = 0.0f);

/// Read events from the log.
/// @param events    Output array to fill.
/// @param max_out   Max events to read.
/// @param offset    Starting event index (0 = oldest available).
/// @return Number of events actually read.
int diaglog_read(DiagEvent* events, int max_out, int offset = 0);

/// Total number of events currently stored (up to MAX_EVENTS).
int diaglog_count();

/// Lifetime boot count (persisted in header).
uint32_t diaglog_boot_count();

/// Clear all events (resets write_index and total_count, keeps boot_count).
void diaglog_clear();

/// Serialize events as a JSON array string.
/// @param buf       Output buffer.
/// @param buf_size  Buffer capacity.
/// @param offset    Starting event index (0 = oldest).
/// @param count     Max events to serialize.
/// @return Bytes written (excluding null terminator), or -1 on error.
int diaglog_get_json(char* buf, size_t buf_size, int offset = 0, int count = 50);

// ── Convenience macros ───────────────────────────────────────────────────────

#define DIAG_INFO(sub, code, msg) \
    diaglog_write(DiagSeverity::INFO, (sub), (code), (msg))

#define DIAG_WARN(sub, code, msg) \
    diaglog_write(DiagSeverity::WARN, (sub), (code), (msg))

#define DIAG_ERROR(sub, code, msg) \
    diaglog_write(DiagSeverity::ERR, (sub), (code), (msg))

#define DIAG_CRITICAL(sub, code, msg) \
    diaglog_write(DiagSeverity::CRITICAL, (sub), (code), (msg))

#define DIAG_INFO_V(sub, code, msg, val) \
    diaglog_write(DiagSeverity::INFO, (sub), (code), (msg), (val))

#define DIAG_WARN_V(sub, code, msg, val) \
    diaglog_write(DiagSeverity::WARN, (sub), (code), (msg), (val))

#define DIAG_ERROR_V(sub, code, msg, val) \
    diaglog_write(DiagSeverity::ERR, (sub), (code), (msg), (val))

#define DIAG_CRITICAL_V(sub, code, msg, val) \
    diaglog_write(DiagSeverity::CRITICAL, (sub), (code), (msg), (val))
