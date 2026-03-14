// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Camera MQTT Publisher — captures JPEG frames from CameraHAL and publishes
// them to the MQTT topic `tritium/{device_id}/camera/frame` for consumption
// by tritium-sc's camera_feeds plugin.
//
// This enables ESP32 boards with OV5640 cameras (e.g., 3.5B-C) to act as
// MQTT-based camera sources for the Command Center.
//
// Usage:
//   CameraHAL cam;
//   cam.init(CamResolution::QVGA_320x240, CamPixelFormat::JPEG);
//   camera_mqtt_publisher::Config cfg;
//   cfg.camera = &cam;
//   cfg.device_id = "cam-01";
//   cfg.target_fps = 2;
//   camera_mqtt_publisher::init(cfg);

#include <cstdint>
#include <cstddef>

class CameraHAL;  // forward decl

namespace camera_mqtt_publisher {

struct Config {
    CameraHAL*  camera = nullptr;       // Camera HAL instance
    const char* device_id = nullptr;    // Device ID for topic prefix
    float       target_fps = 2.0f;      // Target frames per second (1-10)
    int         jpeg_quality = 15;      // JPEG quality (lower = better, 10-63)
    bool        auto_start = true;      // Start publishing immediately
};

// Initialize the camera MQTT publisher. Requires MQTT SC bridge to be active.
bool init(const Config& config);

// Stop publishing and free resources.
void shutdown();

// Start/stop frame capture and publishing.
void start();
void stop();

// Call from loop() or tick. Captures frame if interval elapsed, publishes.
void tick();

// Is publisher actively capturing and publishing?
bool is_active();

// Get stats for heartbeat.
int get_stats_json(char* buf, size_t buf_size);

// Get total frames published.
uint32_t get_frame_count();

// Get average publish latency in milliseconds.
uint32_t get_avg_latency_ms();

}  // namespace camera_mqtt_publisher
