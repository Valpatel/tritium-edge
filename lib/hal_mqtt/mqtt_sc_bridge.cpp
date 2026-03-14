// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "mqtt_sc_bridge.h"
#include "debug_log.h"

static constexpr const char* TAG = "mqtt_sc";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace mqtt_sc_bridge {

bool init(const BridgeConfig&) { return false; }
void tick() {}
bool publish_heartbeat() { return false; }
bool publish_sightings() { return false; }
bool is_connected() { return false; }
bool is_active() { return false; }
void on_command(CommandCallback) {}

}  // namespace mqtt_sc_bridge

// ============================================================================
// ESP32 — real MQTT SC bridge
// ============================================================================
#else

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include "hal_mqtt.h"
#include "hal_provision.h"

// Optional BLE scanner integration
#if __has_include("hal_ble_scanner.h")
#include "hal_ble_scanner.h"
#define HAS_BLE_SCANNER 1
#else
#define HAS_BLE_SCANNER 0
#endif

// Optional WiFi scanner integration
#if __has_include("hal_wifi_scanner.h")
#include "hal_wifi_scanner.h"
#include "wifi_classifier.h"
#define HAS_WIFI_SCANNER 1
#else
#define HAS_WIFI_SCANNER 0
#endif

// ESP-NOW mesh status
#if defined(ENABLE_ESPNOW)
#define HAS_ESPNOW 1
#else
#define HAS_ESPNOW 0
#endif

// WiFi probe request capture
#if __has_include("hal_wifi_probe.h")
#include "hal_wifi_probe.h"
#define HAS_WIFI_PROBE 1
#else
#define HAS_WIFI_PROBE 0
#endif

// RF motion monitor
#if defined(ENABLE_ESPNOW) && __has_include("hal_rf_monitor.h")
#include "hal_rf_monitor.h"
#define HAS_RF_MONITOR 1
#else
#define HAS_RF_MONITOR 0
#endif

// Remote config sync — report config version in heartbeat
#if __has_include("hal_config_sync.h")
#include "hal_config_sync.h"
#define HAS_CONFIG_SYNC 1
#else
#define HAS_CONFIG_SYNC 0
#endif

namespace mqtt_sc_bridge {

// Internal state
static bool _initialized = false;
static bool _active = false;
static MqttHAL _mqtt;
static char _device_id[64] = {};
static uint32_t _heartbeat_interval_ms = 30000;
static uint32_t _sighting_interval_ms = 15000;
static uint32_t _last_heartbeat_ms = 0;
static uint32_t _last_sighting_ms = 0;
static CommandCallback _cmd_cb = nullptr;

// Topic buffers
static char _topic_status[128] = {};
static char _topic_heartbeat[128] = {};
static char _topic_sighting[128] = {};
static char _topic_cmd[128] = {};

// PSRAM JSON buffer
static char* _json_buf = nullptr;
static constexpr size_t JSON_BUF_SIZE = 2048;

// Provisioning
static ProvisionHAL _provision;
static bool _provision_inited = false;

// ---------------------------------------------------------------------------
// Command handler callback
// ---------------------------------------------------------------------------
static void mqtt_cmd_callback(const char* topic, const uint8_t* payload, size_t length) {
    if (_cmd_cb && length > 0) {
        // Null-terminate the payload for safe string handling
        static char cmd_buf[512];
        size_t copy_len = (length < sizeof(cmd_buf) - 1) ? length : sizeof(cmd_buf) - 1;
        memcpy(cmd_buf, payload, copy_len);
        cmd_buf[copy_len] = '\0';

        // Extract command name from topic suffix after "cmd"
        const char* cmd_part = strrchr(topic, '/');
        if (cmd_part) cmd_part++;
        else cmd_part = "unknown";

        _cmd_cb(cmd_part, cmd_buf, copy_len);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init(const BridgeConfig& config) {
    if (_initialized) return _active;
    _initialized = true;

    _heartbeat_interval_ms = config.heartbeat_interval_ms;
    _sighting_interval_ms = config.sighting_interval_ms;

    // Resolve broker and device_id
    char broker[128] = {};
    uint16_t port = config.port;

    if (config.broker && config.broker[0] != '\0') {
        strncpy(broker, config.broker, sizeof(broker) - 1);
    }
    if (config.device_id && config.device_id[0] != '\0') {
        strncpy(_device_id, config.device_id, sizeof(_device_id) - 1);
    }

    // If not provided, try provisioning
    if (broker[0] == '\0' || _device_id[0] == '\0') {
        if (!_provision_inited) {
            _provision_inited = _provision.init();
        }
        if (_provision_inited && _provision.isProvisioned()) {
            const DeviceIdentity& id = _provision.getIdentity();
            if (broker[0] == '\0' && id.mqtt_broker[0] != '\0') {
                strncpy(broker, id.mqtt_broker, sizeof(broker) - 1);
            }
            if (port == 1883 && id.mqtt_port > 0) {
                port = id.mqtt_port;
            }
            if (_device_id[0] == '\0' && id.device_id[0] != '\0') {
                strncpy(_device_id, id.device_id, sizeof(_device_id) - 1);
            }
        }
    }

    // Need both broker and device_id
    if (broker[0] == '\0' || _device_id[0] == '\0') {
        DBG_INFO(TAG, "Not configured (no broker or device_id)");
        _active = false;
        return false;
    }

    // Build topic strings
    snprintf(_topic_status, sizeof(_topic_status), "tritium/%s/status", _device_id);
    snprintf(_topic_heartbeat, sizeof(_topic_heartbeat), "tritium/%s/heartbeat", _device_id);
    snprintf(_topic_sighting, sizeof(_topic_sighting), "tritium/%s/sighting", _device_id);
    snprintf(_topic_cmd, sizeof(_topic_cmd), "tritium/%s/cmd/#", _device_id);

    // Allocate JSON buffer in PSRAM
    _json_buf = (char*)heap_caps_malloc(JSON_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!_json_buf) {
        _json_buf = (char*)malloc(JSON_BUF_SIZE);
    }
    if (!_json_buf) {
        DBG_ERROR(TAG, "Failed to allocate JSON buffer");
        _active = false;
        return false;
    }

    // Initialize MQTT client
    if (!_mqtt.init(broker, port, _device_id)) {
        DBG_ERROR(TAG, "MQTT init failed");
        _active = false;
        return false;
    }

    // Set last will — offline status when connection drops
    _mqtt.setLastWill(_topic_status, "{\"online\":false}", 1, true);

    // Subscribe to command topic
    _mqtt.subscribe(_topic_cmd, mqtt_cmd_callback, 1);

    _active = true;
    _last_heartbeat_ms = 0;
    _last_sighting_ms = 0;

    DBG_INFO(TAG, "Initialized: broker=%s:%u device=%s", broker, port, _device_id);
    return true;
}

void tick() {
    if (!_active) return;
    if (!WiFi.isConnected()) return;

    // Process MQTT (handles connect/reconnect/message dispatch)
    _mqtt.process();

    // Publish online status on first connect
    if (_mqtt.isConnected()) {
        static bool _status_published = false;
        if (!_status_published) {
            _mqtt.publish(_topic_status, "{\"online\":true}", true, 1);
            _status_published = true;
            DBG_INFO(TAG, "Published online status");
        }
    } else {
        return;  // Not connected yet, skip publishing
    }

    uint32_t now = millis();

    // Periodic heartbeat
    if (_last_heartbeat_ms == 0 || (now - _last_heartbeat_ms) >= _heartbeat_interval_ms) {
        _last_heartbeat_ms = now;
        publish_heartbeat();
    }

    // Periodic sighting data
    if (_last_sighting_ms == 0 || (now - _last_sighting_ms) >= _sighting_interval_ms) {
        _last_sighting_ms = now;
        publish_sightings();
    }
}

bool publish_heartbeat() {
    if (!_active || !_mqtt.isConnected() || !_json_buf) return false;

    // Build heartbeat JSON — device telemetry
    const esp_app_desc_t* desc = esp_app_get_description();
    const char* version = desc ? desc->version : "unknown";

#ifdef DISPLAY_DRIVER
    const char* board = DISPLAY_DRIVER;
#else
    const char* board = "unknown";
#endif

    int pos = snprintf(_json_buf, JSON_BUF_SIZE,
        "{\"device_id\":\"%s\",\"version\":\"%s\",\"board\":\"%s\","
        "\"ip\":\"%s\",\"mac\":\"%s\",\"uptime_s\":%lu,"
        "\"free_heap\":%u,\"rssi\":%d",
        _device_id, version, board,
        WiFi.localIP().toString().c_str(),
        WiFi.macAddress().c_str(),
        (unsigned long)(millis() / 1000),
        (unsigned)ESP.getFreeHeap(),
        WiFi.RSSI());

    // Append BLE summary if available
#if HAS_BLE_SCANNER
    if (hal_ble_scanner::is_active()) {
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos, ",\"sensors\":");
        char ble_summary[128];
        hal_ble_scanner::get_summary_json(ble_summary, sizeof(ble_summary));
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos, "%s", ble_summary);
    }
#endif

    // Append WiFi probe summary if available
#if HAS_WIFI_PROBE
    if (hal_wifi_probe::is_active()) {
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos, ",\"wifi_probe\":");
        char probe_summary[128];
        hal_wifi_probe::get_summary_json(probe_summary, sizeof(probe_summary));
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos, "%s", probe_summary);
    }
#endif

    // Append RF motion summary if available
#if HAS_RF_MONITOR
    if (hal_rf_monitor::is_active()) {
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos, ",\"rf_monitor\":");
        char rf_summary[64];
        hal_rf_monitor::get_summary_json(rf_summary, sizeof(rf_summary));
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos, "%s", rf_summary);
    }
#endif

    // Append config sync status
#if HAS_CONFIG_SYNC
    {
        char cfg_json[256];
        hal_config_sync::get_config_json(cfg_json, sizeof(cfg_json));
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos,
            ",\"config_sync\":%s", cfg_json);
    }
#endif

    // Append transports
    pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos,
        ",\"transports\":[{\"type\":\"wifi\",\"state\":\"available\",\"rssi\":%d}"
        ",{\"type\":\"mqtt\",\"state\":\"connected\"}", WiFi.RSSI());
#if HAS_ESPNOW
    pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos,
        ",{\"type\":\"esp_now\",\"state\":\"available\"}");
#endif
    pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos, "]");

    // Close JSON
    if (pos < (int)JSON_BUF_SIZE - 1) {
        _json_buf[pos++] = '}';
        _json_buf[pos] = '\0';
    }

    bool ok = _mqtt.publish(_topic_heartbeat, _json_buf, false, 0);
    if (ok) {
        DBG_DEBUG(TAG, "Heartbeat published (%d bytes)", pos);
    } else {
        DBG_DEBUG(TAG, "Heartbeat publish failed");
    }
    return ok;
}

bool publish_sightings() {
    if (!_active || !_mqtt.isConnected() || !_json_buf) return false;

    bool published = false;

    // BLE sightings
#if HAS_BLE_SCANNER
    if (hal_ble_scanner::is_active()) {
        int pos = snprintf(_json_buf, JSON_BUF_SIZE,
            "{\"type\":\"ble\",\"device_id\":\"%s\",\"devices\":", _device_id);

        // Get device list as JSON array
        char devs_json[1536];
        hal_ble_scanner::get_devices_json(devs_json, sizeof(devs_json));
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos, "%s", devs_json);

        // Add summary stats
        char summary[128];
        hal_ble_scanner::get_summary_json(summary, sizeof(summary));
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos,
            ",\"summary\":%s}", summary);

        if (_mqtt.publish(_topic_sighting, _json_buf, false, 0)) {
            DBG_DEBUG(TAG, "BLE sighting published (%d bytes)", pos);
            published = true;
        }
    }
#endif

    // WiFi sightings
#if HAS_WIFI_SCANNER
    if (hal_wifi_scanner::is_active()) {
        hal_wifi_scanner::WifiNetwork nets[16];
        int n = hal_wifi_scanner::get_networks(nets, 16);
        if (n > 0) {
            int pos = snprintf(_json_buf, JSON_BUF_SIZE,
                "{\"type\":\"wifi\",\"device_id\":\"%s\",\"networks\":[", _device_id);

            for (int i = 0; i < n && pos < (int)JSON_BUF_SIZE - 200; i++) {
                if (i > 0) _json_buf[pos++] = ',';
                auto cl = wifi_classifier::classify(nets[i].ssid, nets[i].auth_type, nets[i].rssi);
                pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos,
                    "{\"ssid\":\"%s\",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                    "\"rssi\":%d,\"channel\":%d,\"auth\":%d,\"type\":\"%s\"}",
                    nets[i].ssid,
                    nets[i].bssid[0], nets[i].bssid[1], nets[i].bssid[2],
                    nets[i].bssid[3], nets[i].bssid[4], nets[i].bssid[5],
                    nets[i].rssi, nets[i].channel, nets[i].auth_type,
                    cl.type_name);
            }

            pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos,
                "],\"count\":%d}", n);

            if (_mqtt.publish(_topic_sighting, _json_buf, false, 0)) {
                DBG_DEBUG(TAG, "WiFi sighting published (%d bytes)", pos);
                published = true;
            }
        }
    }
#endif

    // WiFi probe sightings — passive device fingerprinting
#if HAS_WIFI_PROBE
    if (hal_wifi_probe::is_active() && hal_wifi_probe::get_active_count() > 0) {
        int pos = snprintf(_json_buf, JSON_BUF_SIZE,
            "{\"type\":\"wifi_probe\",\"device_id\":\"%s\",\"devices\":", _device_id);

        pos += hal_wifi_probe::get_devices_json(_json_buf + pos, JSON_BUF_SIZE - pos);

        // Append summary
        char summary[128];
        hal_wifi_probe::get_summary_json(summary, sizeof(summary));
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos,
            ",\"summary\":%s}", summary);

        if (_mqtt.publish(_topic_sighting, _json_buf, false, 0)) {
            DBG_DEBUG(TAG, "WiFi probe sighting published (%d bytes)", pos);
            published = true;
        }
    }
#endif

    // RF motion sighting — inter-node RSSI variance data
#if HAS_RF_MONITOR
    if (hal_rf_monitor::is_active() && hal_rf_monitor::get_peer_count() > 0) {
        int pos = snprintf(_json_buf, JSON_BUF_SIZE,
            "{\"type\":\"rf_motion\",\"device_id\":\"%s\",\"rf_peers\":", _device_id);

        pos += hal_rf_monitor::get_peer_rssi_json(_json_buf + pos, JSON_BUF_SIZE - pos);

        // Append summary
        char summary[64];
        hal_rf_monitor::get_summary_json(summary, sizeof(summary));
        pos += snprintf(_json_buf + pos, JSON_BUF_SIZE - pos,
            ",\"summary\":%s}", summary);

        if (_mqtt.publish(_topic_sighting, _json_buf, false, 0)) {
            DBG_DEBUG(TAG, "RF motion sighting published (%d bytes)", pos);
            published = true;
        }
    }
#endif

    return published;
}

bool is_connected() {
    return _active && _mqtt.isConnected();
}

bool is_active() {
    return _active;
}

void on_command(CommandCallback cb) {
    _cmd_cb = cb;
}

}  // namespace mqtt_sc_bridge

// Accessor for camera_mqtt_publisher and other subsystems that need
// direct MQTT publish access (e.g., binary frame data).
// Note: _mqtt is file-static inside the namespace, so this accessor
// is defined in the same translation unit where it has visibility.
extern "C" MqttHAL* mqtt_sc_bridge_get_client() {
    if (!mqtt_sc_bridge::_active) return nullptr;
    return &mqtt_sc_bridge::_mqtt;
}

#endif  // SIMULATOR
