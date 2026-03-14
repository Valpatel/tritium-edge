// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Remote Configuration Sync HAL
//
// Pulls device config from the fleet server on boot and applies it to NVS.
// Subscribes to MQTT tritium/{device_id}/cmd/config for live config pushes.
// Reports current config version in heartbeat for SC to verify sync state.
//
// Config fields synced:
//   - heartbeat_interval_s
//   - sighting_interval_s
//   - scan_interval_s
//   - display_brightness
//   - display_timeout_s
//   - rf_monitor_threshold_dbm
//   - rf_monitor_window_s
//   - config_version (monotonic, only apply if newer)
//
// Boot flow:
//   1. HTTP GET /api/devices/{device_id}/config
//   2. Compare config_version with local NVS version
//   3. If newer, apply to NVS and update local state
//
// Live flow:
//   1. MQTT message on tritium/{device_id}/cmd/config
//   2. Parse JSON, compare version, apply if newer

#include <cstdint>
#include <cstddef>

namespace hal_config_sync {

struct SyncConfig {
    const char* server_url = nullptr;   // Fleet server URL (e.g., "http://192.168.1.100:8080")
    const char* device_id = nullptr;    // Device ID for API path
};

// Synced configuration values — read these after init
struct DeviceConfig {
    uint32_t config_version = 0;        // Monotonic version number
    uint32_t heartbeat_interval_s = 30;
    uint32_t sighting_interval_s = 15;
    uint32_t scan_interval_s = 10;
    uint8_t  display_brightness = 255;
    uint32_t display_timeout_s = 300;
    int      rf_monitor_threshold_dbm = -60;
    uint32_t rf_monitor_window_s = 10;
};

// Initialize config sync. Call after WiFi is connected, before other HALs.
// Performs HTTP GET to pull config from fleet server on boot.
// Returns true if initialized (may or may not have fetched config).
bool init(const SyncConfig& config = {});

// Called when an MQTT config push arrives. Parses JSON, applies if newer.
// Typically wired via mqtt_sc_bridge::on_command().
void on_mqtt_config(const char* payload, size_t len);

// Get the current synced config (returns defaults if no sync happened).
const DeviceConfig& get_config();

// Get config version (0 = never synced).
uint32_t get_config_version();

// Check if config has been synced at least once.
bool is_synced();

// Write current config summary as JSON into buf for heartbeat reporting.
// Returns bytes written (excluding null terminator).
int get_config_json(char* buf, size_t buf_size);

}  // namespace hal_config_sync
