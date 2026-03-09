// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#pragma once

#include <cstdint>
#include <cstddef>

namespace hal_wifi_scanner {

#define WIFI_SCANNER_MAX_NETWORKS 32
#define WIFI_NETWORK_TIMEOUT_MS 300000  // 5 minutes

struct WifiNetwork {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth_type;  // 0=OPEN, 1=WEP, 2=WPA_PSK, 3=WPA2_PSK, 4=WPA_WPA2_PSK, 5=WPA3_PSK
    uint32_t first_seen;  // millis()
    uint32_t last_seen;
    uint16_t seen_count;
};

struct ScanConfig {
    uint32_t scan_interval_ms = 30000;  // Scan every 30s
    bool show_hidden = false;            // Include hidden SSIDs
    bool passive = false;                // Passive scan (slower but less intrusive)
};

bool init(const ScanConfig& config = ScanConfig());
void shutdown();
bool is_active();

// Get current network list snapshot
int get_networks(WifiNetwork* out, int max_count);
int get_visible_count();

// JSON output
int get_networks_json(char* buf, size_t size);
int get_summary_json(char* buf, size_t size);

}  // namespace hal_wifi_scanner
