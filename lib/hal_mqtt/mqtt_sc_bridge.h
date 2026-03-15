// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// MQTT SC Bridge — publishes device telemetry to tritium MQTT topics
// for direct consumption by tritium-sc's MQTTBridge/EdgeTrackerPlugin.
//
// Topics published:
//   tritium/{device_id}/status      — online/offline (retained, last will)
//   tritium/{device_id}/heartbeat   — periodic JSON telemetry
//   tritium/{device_id}/sighting    — BLE/WiFi sighting data
//
// Topics subscribed:
//   tritium/{device_id}/cmd         — commands from SC

#include <cstdint>
#include <cstddef>

namespace mqtt_sc_bridge {

struct BridgeConfig {
    const char* broker = nullptr;       // MQTT broker host (from provisioning)
    uint16_t port = 1883;               // MQTT broker port
    const char* device_id = nullptr;    // Device ID for topic prefix
    uint32_t heartbeat_interval_ms = 30000;  // How often to publish heartbeat
    uint32_t sighting_interval_ms = 15000;   // How often to publish sightings
    uint32_t full_heartbeat_interval_ms = 300000; // Full JSON heartbeat every 5 min
    bool compact_heartbeat = true;      // Use compact binary between full heartbeats
};

// Initialize the MQTT SC bridge. Call after WiFi is connected.
// If broker is null, tries to load from provisioning.
bool init(const BridgeConfig& config = {});

// Call from loop(). Handles MQTT connection, heartbeat/sighting publishing.
void tick();

// Force publish a heartbeat now (full JSON).
bool publish_heartbeat();

// Publish a compact binary heartbeat (battery/RSSI/sighting_count only).
// ~20 bytes vs ~500+ for full JSON. Used between full heartbeats to
// reduce MQTT bandwidth for large fleets.
bool publish_compact_heartbeat();

// Force publish current sighting data now.
bool publish_sightings();

// Check if bridge is connected to MQTT broker.
bool is_connected();

// Check if bridge is initialized (may not be connected yet).
bool is_active();

// Publish a capabilities advertisement listing all compiled-in HALs.
// Called automatically on first connect, but can be called manually
// e.g. after radio scheduler changes modes.
bool publish_capabilities();

// Command callback — called when SC sends a command
typedef void (*CommandCallback)(const char* command, const char* payload, size_t len);
void on_command(CommandCallback cb);

// Publish a command acknowledgement to tritium/{device_id}/cmd/ack.
// Call this after executing a received command to report success/failure.
// result should be "success", "failure", or "unsupported".
bool publish_cmd_ack(const char* command_id, const char* command, const char* result, const char* error_msg = nullptr);

}  // namespace mqtt_sc_bridge
