// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "camera_mqtt_publisher.h"
#include "hal_camera.h"
#include <cstdio>

#ifdef SIMULATOR

namespace camera_mqtt_publisher {
bool init(const Config&) { return false; }
void shutdown() {}
void start() {}
void stop() {}
void tick() {}
bool is_active() { return false; }
int get_stats_json(char* buf, size_t s) { return snprintf(buf, s, "{}"); }
uint32_t get_frame_count() { return 0; }
uint32_t get_avg_latency_ms() { return 0; }
}

#else

#include <Arduino.h>

#if __has_include("hal_mqtt.h")
#include "hal_mqtt.h"
#define HAS_MQTT 1
#else
#define HAS_MQTT 0
#endif

#if __has_include("mqtt_sc_bridge.h")
#include "mqtt_sc_bridge.h"
#define HAS_SC_BRIDGE 1
#else
#define HAS_SC_BRIDGE 0
#endif

#if __has_include("debug_log.h")
#include "debug_log.h"
#endif

static constexpr const char* TAG = "cam_mqtt";

namespace camera_mqtt_publisher {

// Internal state
static CameraHAL* _camera = nullptr;
static char _topic_frame[128] = {};
static char _topic_meta[128] = {};
static bool _initialized = false;
static bool _publishing = false;
static float _target_fps = 2.0f;
static uint32_t _frame_interval_ms = 500;

// Stats
static uint32_t _frames_published = 0;
static uint32_t _frames_failed = 0;
static uint32_t _total_latency_ms = 0;
static uint32_t _last_publish_ms = 0;
static uint32_t _max_frame_size = 0;

// MQTT client for binary frame publishing
static MqttHAL* _mqtt = nullptr;

bool init(const Config& config) {
    if (_initialized) return true;
    if (!config.camera || !config.device_id) {
        DBG_ERROR(TAG, "Camera or device_id not provided");
        return false;
    }

    _camera = config.camera;
    _target_fps = config.target_fps;
    if (_target_fps < 0.1f) _target_fps = 0.1f;
    if (_target_fps > 10.0f) _target_fps = 10.0f;
    _frame_interval_ms = (uint32_t)(1000.0f / _target_fps);

    // Build topic strings
    snprintf(_topic_frame, sizeof(_topic_frame),
        "tritium/%s/camera/frame", config.device_id);
    snprintf(_topic_meta, sizeof(_topic_meta),
        "tritium/%s/camera/meta", config.device_id);

    _frames_published = 0;
    _frames_failed = 0;
    _total_latency_ms = 0;
    _last_publish_ms = 0;
    _max_frame_size = 0;
    _initialized = true;
    _publishing = config.auto_start;

    DBG_INFO(TAG, "Initialized: topic=%s fps=%.1f interval=%ums",
        _topic_frame, _target_fps, _frame_interval_ms);
    return true;
}

void shutdown() {
    _publishing = false;
    _initialized = false;
    _camera = nullptr;
}

void start() { _publishing = true; }
void stop()  { _publishing = false; }

void tick() {
    if (!_initialized || !_publishing || !_camera) return;
    if (!_camera->available()) return;

#if HAS_SC_BRIDGE
    if (!mqtt_sc_bridge::is_connected()) return;
#else
    return;  // No MQTT transport available
#endif

    uint32_t now = millis();
    if (_last_publish_ms != 0 && (now - _last_publish_ms) < _frame_interval_ms) return;

    // Capture JPEG frame
    CameraFrame* frame = _camera->capture();
    if (!frame || !frame->data || frame->len == 0) {
        _frames_failed++;
        return;
    }

    if (frame->format != CamPixelFormat::JPEG) {
        // Only JPEG can be published efficiently over MQTT
        _camera->releaseFrame();
        DBG_WARN(TAG, "Camera not in JPEG mode — skipping publish");
        return;
    }

    uint32_t t0 = millis();

    // Publish binary JPEG frame to MQTT
    // Note: MqttHAL::publish_binary() would be ideal here.
    // For now, use the existing publish with binary data cast.
    // The SC camera_feeds plugin expects raw JPEG bytes on this topic.
#if HAS_SC_BRIDGE
    // Publish binary JPEG data via the SC bridge's MQTT client
    extern "C" MqttHAL* mqtt_sc_bridge_get_client();
    MqttHAL* client = mqtt_sc_bridge_get_client();
    bool ok = false;
    if (client && client->isConnected()) {
        ok = client->publish(_topic_frame, frame->data, frame->len, false, 0);
    }
#else
    bool ok = false;
#endif

    uint32_t latency = millis() - t0;
    _camera->releaseFrame();

    if (ok) {
        _frames_published++;
        _total_latency_ms += latency;
        if (frame->len > _max_frame_size) _max_frame_size = frame->len;
        _last_publish_ms = now;
    } else {
        _frames_failed++;
        DBG_DEBUG(TAG, "Frame publish failed (%u bytes)", (unsigned)frame->len);
    }
}

bool is_active() { return _initialized && _publishing; }

int get_stats_json(char* buf, size_t s) {
    uint32_t avg_lat = (_frames_published > 0) ?
        (_total_latency_ms / _frames_published) : 0;
    float actual_fps = 0.0f;
    if (_camera && _camera->available()) {
        actual_fps = _camera->getAvgFps();
    }
    return snprintf(buf, s,
        "{\"active\":%s,\"frames\":%lu,\"failed\":%lu,"
        "\"avg_latency_ms\":%lu,\"max_frame_bytes\":%lu,"
        "\"target_fps\":%.1f,\"actual_fps\":%.1f}",
        _publishing ? "true" : "false",
        (unsigned long)_frames_published,
        (unsigned long)_frames_failed,
        (unsigned long)avg_lat,
        (unsigned long)_max_frame_size,
        _target_fps, actual_fps);
}

uint32_t get_frame_count() { return _frames_published; }
uint32_t get_avg_latency_ms() {
    return (_frames_published > 0) ? (_total_latency_ms / _frames_published) : 0;
}

}  // namespace camera_mqtt_publisher

#endif  // SIMULATOR
