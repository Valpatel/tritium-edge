// Tritium Sighting Logger — BLE + WiFi MAC/RSSI logging to SQLite on SD card
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>

namespace hal_sighting_logger {

struct LoggerConfig {
    const char* db_path = "/sdcard/sightings.db";  // SQLite DB on SD
    uint32_t flush_interval_ms = 5000;              // Batch insert interval
    uint16_t max_batch = 64;                        // Max rows per batch
};

bool init(const LoggerConfig& config = LoggerConfig());
void shutdown();

// Log a BLE device sighting
void log_ble(const char* mac, const char* name, int8_t rssi, bool is_known);

// Log a WiFi network sighting
void log_wifi(const char* ssid, const uint8_t bssid[6], int8_t rssi,
              uint8_t channel, uint8_t auth_type);

// Flush pending batch to DB (called by tick or manually)
int flush();

// Call from main loop
void tick();

// Check if logging is active
bool is_active();

// Stats
struct Stats {
    uint32_t ble_logged;
    uint32_t wifi_logged;
    uint32_t total_rows;
    uint32_t db_size_bytes;
    bool db_open;
};
Stats get_stats();

// Service control
void enable();
void disable();

}  // namespace hal_sighting_logger
