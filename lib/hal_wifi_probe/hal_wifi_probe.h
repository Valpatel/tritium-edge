// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#pragma once

// WiFi Probe Request Capture -- passive device fingerprinting
//
// Uses ESP32 promiscuous mode to capture WiFi probe requests from nearby
// devices. Each probe request contains the device MAC address and the SSID
// being probed for, enabling passive device tracking and fingerprinting.
//
// Important: promiscuous mode works alongside normal STA connection on ESP32-S3.
// The chip can sniff packets on the current channel while connected to an AP.
// To sniff all channels, channel hopping can be enabled (interrupts connection briefly).
//
// Publishes wifi_probe sightings via MQTT for the unified target picture.

#include <cstdint>
#include <cstddef>

namespace hal_wifi_probe {

static constexpr int PROBE_MAX_DEVICES = 64;
static constexpr int PROBE_MAX_SSIDS_PER_DEVICE = 8;
static constexpr uint32_t PROBE_DEVICE_TIMEOUT_MS = 600000;  // 10 minutes

struct ProbedSSID {
    char ssid[33];
    uint32_t last_seen;
};

struct ProbeDevice {
    uint8_t mac[6];
    int8_t rssi;            // Latest RSSI
    int8_t rssi_min;        // Min RSSI seen
    int8_t rssi_max;        // Max RSSI seen
    uint8_t channel;        // Channel probe was seen on
    uint32_t first_seen;    // millis()
    uint32_t last_seen;
    uint16_t probe_count;   // Total number of probes captured
    ProbedSSID ssids[PROBE_MAX_SSIDS_PER_DEVICE];
    uint8_t ssid_count;
    bool is_randomized;     // MAC randomization detected (locally administered bit)
};

struct ProbeConfig {
    bool enabled = true;
    bool channel_hop = false;       // Hop between channels (disrupts connection briefly)
    uint32_t hop_interval_ms = 500; // Time per channel when hopping
    uint32_t report_interval_ms = 15000;  // How often to report to MQTT
};

// Initialize probe capture (call after WiFi is initialized)
bool init(const ProbeConfig& config = ProbeConfig());
void shutdown();
bool is_active();

// Get current device list snapshot
int get_devices(ProbeDevice* out, int max_count);
int get_active_count();

// JSON output for heartbeat and MQTT reporting
int get_devices_json(char* buf, size_t size);
int get_summary_json(char* buf, size_t size);

// Generate a sighting JSON for a specific device (for MQTT wifi_probe topic)
int get_sighting_json(const ProbeDevice& dev, char* buf, size_t size);

}  // namespace hal_wifi_probe
