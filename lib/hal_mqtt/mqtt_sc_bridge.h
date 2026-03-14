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
};

// Initialize the MQTT SC bridge. Call after WiFi is connected.
// If broker is null, tries to load from provisioning.
bool init(const BridgeConfig& config = {});

// Call from loop(). Handles MQTT connection, heartbeat/sighting publishing.
void tick();

// Force publish a heartbeat now.
bool publish_heartbeat();

// Force publish current sighting data now.
bool publish_sightings();

// Check if bridge is connected to MQTT broker.
bool is_connected();

// Check if bridge is initialized (may not be connected yet).
bool is_active();

// Command callback — called when SC sends a command
typedef void (*CommandCallback)(const char* command, const char* payload, size_t len);
void on_command(CommandCallback cb);

}  // namespace mqtt_sc_bridge
