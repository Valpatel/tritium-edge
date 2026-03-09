// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Cursor-on-Target (CoT) XML generation for TAK ecosystem.
//
// Generates MIL-STD-2045 CoT XML so edge devices appear in ATAK, WinTAK,
// and WebTAK. Follows tritium-lib/cot conventions (uid format, type codes,
// __group detail, tritium_edge metadata element).
//
// Transport: UDP multicast (239.2.3.1:6969 — standard TAK SA) and/or
// TCP streaming to a TAK server (length-prefixed XML).
//
// Usage:
//   #include "hal_cot.h"
//   hal_cot::CotConfig cfg;
//   cfg.device_id = "esp32-aabbcc";
//   cfg.callsign = "TRITIUM-AABB";
//   hal_cot::init(cfg);
//
//   // In loop:
//   hal_cot::set_position(37.7749, -122.4194, 50.0);
//   hal_cot::tick();  // sends CoT at configured interval
//
//   // Or build XML manually:
//   char xml[1024];
//   hal_cot::build_position_event(xml, sizeof(xml));
//   hal_cot::send_multicast(xml);

#include <cstdint>
#include <cstddef>

namespace hal_cot {

// ---------------------------------------------------------------------------
// CoT type codes — matches tritium-lib/cot/codec.py _EDGE_COT_TYPES
// ---------------------------------------------------------------------------
static constexpr const char* COT_TYPE_SENSOR   = "a-f-G-E-S";      // friendly ground sensor
static constexpr const char* COT_TYPE_CAMERA   = "a-f-G-E-S-C";    // friendly ground sensor camera
static constexpr const char* COT_TYPE_GATEWAY  = "a-f-G-E-S";      // friendly ground sensor
static constexpr const char* COT_TYPE_RELAY    = "a-f-G-E-S";      // friendly ground sensor
static constexpr const char* COT_TYPE_DISPLAY  = "a-f-G-U-C";      // friendly ground unit command

// Standard TAK multicast
static constexpr const char* TAK_MULTICAST_ADDR = "239.2.3.1";
static constexpr uint16_t TAK_MULTICAST_PORT = 6969;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct CotConfig {
    const char* device_id = nullptr;        // Unique ID (e.g. "esp32-aabbccddeeff")
    const char* callsign  = nullptr;        // TAK display name (defaults to TRITIUM-XXYY)
    const char* cot_type  = COT_TYPE_SENSOR;// CoT type code
    const char* team      = "Cyan";         // __group name (team color)
    const char* role       = "Sensor";      // __group role
    const char* how       = "m-g";          // how field: m-g=machine-GPS, m-r=machine-reported
    uint32_t stale_seconds = 300;           // Seconds until position goes stale
    uint32_t interval_ms   = 60000;         // Heartbeat/tick interval (ms)
    float ce = 10.0f;                       // Circular error (meters)
    float le = 10.0f;                       // Linear error (meters)
};

// ---------------------------------------------------------------------------
// CotEvent — populated by build_*() functions
// ---------------------------------------------------------------------------
struct CotEvent {
    char uid[64];
    char type[32];
    char how[8];
    char time[32];          // ISO8601 "2026-03-07T12:00:00.000Z"
    char start[32];
    char stale[32];
    double lat;
    double lon;
    float hae;              // Height above ellipsoid (meters)
    float ce;               // Circular error
    float le;               // Linear error
    char callsign[32];
    char team[16];          // __group name (team color)
    char role[16];          // __group role
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Initialize CoT subsystem. Call once after WiFi is connected.
// If device_id is null, derives from MAC address.
// If callsign is null, generates "TRITIUM-XXYY" from last 2 MAC bytes.
bool init(const CotConfig& config = {});

// Update device position (call whenever GPS/position changes).
void set_position(double lat, double lon, float hae = 0.0f);

// Periodic tick — sends CoT position if interval has elapsed and WiFi is up.
// Returns true if a CoT event was sent.
bool tick();

// Check if CoT is initialized and ready.
bool is_active();

// ---------------------------------------------------------------------------
// XML Builders — write CoT XML into buf, return bytes written (0 on error)
// ---------------------------------------------------------------------------

// Position/location SA event (primary heartbeat)
int build_position_event(char* buf, size_t buf_size);

// Sensor reading event (e.g., temperature, humidity, IMU)
int build_sensor_event(char* buf, size_t buf_size,
                       const char* sensor_type, float value,
                       const char* unit = nullptr);

// Device status event (battery, uptime, free heap, RSSI)
int build_status_event(char* buf, size_t buf_size);

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

// Send CoT XML via UDP multicast (239.2.3.1:6969).
// Returns true if sent successfully.
bool send_multicast(const char* xml, size_t len = 0);

// Send CoT XML to a TAK server via TCP (length-prefixed framing).
// Connects, sends, disconnects. Returns true on success.
bool send_tcp(const char* host, uint16_t port, const char* xml, size_t len = 0);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Format epoch seconds to ISO8601 UTC string: "2026-03-07T12:00:00.000Z"
// Returns pointer to buf on success, nullptr on failure.
const char* format_iso8601(char* buf, size_t buf_size, uint32_t epoch);

// Get current epoch from NTP/RTC (returns 0 if time not synced).
uint32_t get_epoch();

}  // namespace hal_cot
