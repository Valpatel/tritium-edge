// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

#include <cstdint>

namespace hal_heartbeat {

// Capabilities bitfield — 1 byte encoding which HALs are compiled in.
// SC decodes this to build the device capability matrix without parsing JSON keys.
// Bit assignments:
static constexpr uint8_t CAP_WIFI          = 0x01;  // bit 0: WiFi networking
static constexpr uint8_t CAP_BLE           = 0x02;  // bit 1: BLE scanner
static constexpr uint8_t CAP_ESPNOW        = 0x04;  // bit 2: ESP-NOW mesh
static constexpr uint8_t CAP_LORA          = 0x08;  // bit 3: LoRa radio
static constexpr uint8_t CAP_MQTT          = 0x10;  // bit 4: MQTT bridge
static constexpr uint8_t CAP_CAMERA        = 0x20;  // bit 5: Camera
static constexpr uint8_t CAP_ACOUSTIC      = 0x40;  // bit 6: Acoustic sensor
static constexpr uint8_t CAP_COT           = 0x80;  // bit 7: CoT/TAK

// Get the capabilities bitfield for this build.
uint8_t get_capabilities();

struct HeartbeatConfig {
    const char* server_url = nullptr;  // e.g., "http://192.168.1.100:8080"
    const char* device_id = nullptr;   // e.g., "esp32-aabbccddeeff"
    uint32_t interval_ms = 60000;      // Default 60s
    bool cot_enabled = false;          // Also send CoT position on each heartbeat tick
    const char* device_group = nullptr;  // Group assignment: "perimeter", "interior", "mobile", "reserve"
};

// Initialize heartbeat system. Call once after WiFi is connected.
// If server_url is null, tries to load from NVS (hal_provision).
// Returns true if configured and ready to send.
bool init(const HeartbeatConfig& config = {});

// Call from loop(). Sends heartbeat if interval has elapsed.
// Non-blocking — returns immediately if not time yet.
// Returns true if a heartbeat was successfully sent.
bool tick();

// Force send a heartbeat now (ignores interval timer).
bool send_now();

// Check if heartbeat is configured and active.
bool is_active();

// Get the server-recommended interval (may differ from config).
uint32_t get_interval_ms();

// Set device group assignment. Valid groups: perimeter, interior, mobile, reserve.
// Persists to NVS so it survives reboot. Included in heartbeat JSON.
void set_group(const char* group);

// Get current device group (empty string if unassigned).
const char* get_group();

// Device lifecycle state management.
// Valid states: "provisioning", "active", "maintenance", "retired", "error".
// Persists to NVS so it survives reboot. Included in heartbeat JSON.
void set_lifecycle_state(const char* state);
const char* get_lifecycle_state();

}  // namespace hal_heartbeat
