// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#pragma once

#include <cstdint>
#include <cstddef>

namespace hal_sighting_buffer {

struct BufferConfig {
    const char* base_dir = "/sdcard/sightings";  // SD card directory
    uint32_t flush_interval_ms = 10000;           // Flush to SD every 10s
    size_t max_file_size = 1048576;               // 1MB max per file, then rotate
    uint16_t max_memory_entries = 100;            // Buffer in RAM before flushing
};

bool init(const BufferConfig& config = BufferConfig());
void shutdown();

// Add sightings to the buffer (called from BLE/WiFi scanner callbacks)
void add_ble_sighting(const char* mac, const char* name, int8_t rssi,
                       bool is_known, uint16_t seen_count);
void add_wifi_sighting(const char* ssid, const uint8_t bssid[6], int8_t rssi,
                        uint8_t channel, uint8_t auth_type);

// Flush in-memory buffer to SD card (called automatically by tick or manually)
int flush_to_sd();

// Get count of pending (un-synced) sightings on SD
int get_pending_count();

// Upload pending sightings to server. Returns count uploaded, -1 on error.
// Called when connectivity is available.
// server_url: base URL like "http://192.168.1.100:8080"
// device_id: this device's ID for the upload endpoint
int sync_to_server(const char* server_url, const char* device_id);

// Tick function — call from main loop. Handles periodic flushing.
void tick();

// Stats
struct Stats {
    uint32_t ble_sightings_buffered;
    uint32_t wifi_sightings_buffered;
    uint32_t total_flushed_to_sd;
    uint32_t total_synced_to_server;
    uint32_t pending_on_sd;
    bool sd_available;
};
Stats get_stats();

}  // namespace hal_sighting_buffer
