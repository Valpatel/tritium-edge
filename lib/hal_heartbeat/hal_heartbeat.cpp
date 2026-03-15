// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_heartbeat.h"
#include "debug_log.h"

#include <cstring>
#include <cstdio>

static constexpr const char* TAG = "heartbeat";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace hal_heartbeat {

static bool _active = false;
static uint32_t _interval_ms = 60000;

uint8_t get_capabilities() { return 0; }

bool init(const HeartbeatConfig& config) {
    (void)config;
    DBG_INFO(TAG, "Heartbeat init (simulator stub)");
    return false;
}

bool tick() { return false; }
bool send_now() { return false; }
bool is_active() { return false; }
uint32_t get_interval_ms() { return _interval_ms; }
void set_group(const char*) {}
const char* get_group() { return ""; }
void set_lifecycle_state(const char*) {}
const char* get_lifecycle_state() { return "active"; }
void set_compact_mode(bool) {}
bool is_compact_mode() { return false; }

}  // namespace hal_heartbeat

// ============================================================================
// ESP32 — real HTTP heartbeat
// ============================================================================
#else

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include "hal_provision.h"

#if __has_include("ota_manager.h")
#include "ota_manager.h"
#define HAS_OTA_MANAGER 1
#else
#define HAS_OTA_MANAGER 0
#endif

// Optional BLE scanner integration
#if __has_include("hal_ble_scanner.h")
#include "hal_ble_scanner.h"
#define HAS_BLE_SCANNER 1
#else
#define HAS_BLE_SCANNER 0
#endif

// Optional CoT integration — piggyback position updates on heartbeat ticks
#if __has_include("hal_cot.h")
#include "hal_cot.h"
#define HAS_COT 1
#else
#define HAS_COT 0
#endif

// Optional diagnostics — piggyback diag report upload on heartbeat ticks
#if __has_include("hal_diag.h")
#include "hal_diag.h"
#define HAS_DIAG 1
#else
#define HAS_DIAG 0
#endif

// Optional persistent diagnostic log — include event count in heartbeat
#if __has_include("hal_diaglog.h")
#include "hal_diaglog.h"
#define HAS_DIAGLOG 1
#else
#define HAS_DIAGLOG 0
#endif

// ESP-NOW mesh status — use ENABLE_ESPNOW build flag
#if defined(ENABLE_ESPNOW)
#include "mesh_manager.h"
#define HAS_ESPNOW 1
#else
#define HAS_ESPNOW 0
#endif

// Optional LoRa — include status in heartbeat
#if __has_include("hal_lora.h")
#include "hal_lora.h"
#define HAS_LORA 1
#else
#define HAS_LORA 0
#endif

// Optional WiFi scanner — include network count + classification summary
#if __has_include("hal_wifi_scanner.h")
#include "hal_wifi_scanner.h"
#include "wifi_classifier.h"
#define HAS_WIFI_SCANNER 1
#else
#define HAS_WIFI_SCANNER 0
#endif

// Optional RF motion monitor — inter-node RSSI variance
#if defined(ENABLE_ESPNOW) && __has_include("hal_rf_monitor.h")
#include "hal_rf_monitor.h"
#define HAS_RF_MONITOR 1
#else
#define HAS_RF_MONITOR 0
#endif

// Optional acoustic sensor — include audio analysis status
#if __has_include("hal_acoustic.h")
#include "hal_acoustic.h"
#define HAS_ACOUSTIC 1
#else
#define HAS_ACOUSTIC 0
#endif

// Optional config sync — include config version and sync state
#if __has_include("hal_config_sync.h")
#include "hal_config_sync.h"
#define HAS_CONFIG_SYNC 1
#else
#define HAS_CONFIG_SYNC 0
#endif

// Optional power consumption tracker — include draw estimates in heartbeat
#if __has_include("power_tracker.h")
#include "power_tracker.h"
#define HAS_POWER_TRACKER 1
#else
#define HAS_POWER_TRACKER 0
#endif

// Sensor self-test — periodic WiFi/BLE/heap/NTP diagnostics
#include "sensor_self_test.h"
#define HAS_SELF_TEST 1

// Network quality monitoring — latency, packet loss, stability
#include "network_quality.h"
#define HAS_NET_QUALITY 1

// Optional tamper detection — sighting dropout monitoring
#if __has_include("hal_tamper_detect.h")
#include "hal_tamper_detect.h"
#define HAS_TAMPER_DETECT 1
#else
#define HAS_TAMPER_DETECT 0
#endif

namespace hal_heartbeat {

// --- Capabilities bitfield — computed at compile time via HAS_* flags ---
uint8_t get_capabilities() {
    uint8_t caps = 0;
    // WiFi is always available on ESP32 builds
    caps |= CAP_WIFI;
#if HAS_BLE_SCANNER
    caps |= CAP_BLE;
#endif
#if HAS_ESPNOW
    caps |= CAP_ESPNOW;
#endif
#if HAS_LORA
    caps |= CAP_LORA;
#endif
    // MQTT — check if PubSubClient is available (always for builds with ENABLE_MQTT_SC_BRIDGE)
#if __has_include("mqtt_sc_bridge.h")
    caps |= CAP_MQTT;
#endif
    // Camera
#if __has_include("hal_camera.h") && defined(HAS_CAMERA) && HAS_CAMERA
    caps |= CAP_CAMERA;
#endif
#if HAS_ACOUSTIC
    caps |= CAP_ACOUSTIC;
#endif
#if HAS_COT
    caps |= CAP_COT;
#endif
    return caps;
}

// Internal state
static bool _initialized = false;
static bool _active = false;
static bool _cot_enabled = false;
static uint32_t _interval_ms = 60000;
static uint32_t _last_send_ms = 0;
static char _server_url[256] = {};
static char _device_id[64] = {};

// Cached firmware info (computed once at init)
static char _fw_version[32] = {};
static char _fw_hash[65] = {};
static const char* _board_name = nullptr;

// Device group assignment (perimeter, interior, mobile, reserve)
static char _device_group[32] = {};

// Device lifecycle state (provisioning, active, maintenance, retired, error)
static char _lifecycle_state[16] = "active";

// Compact heartbeat mode — alternates full/compact to reduce bandwidth
static bool _compact_mode = false;
static uint32_t _heartbeat_seq = 0;  // sequence counter for alternating

// Provisioning instance (shared, lightweight)
static ProvisionHAL _provision;
static bool _provision_inited = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void computeFirmwareHash() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        _fw_hash[0] = '\0';
        return;
    }

    // Read the image header to determine actual firmware size.
    // ESP32 image format: header(24) + segments. The header contains segment count.
    // Total binary size = all segments + header + hash at end.
    // Use esp_ota_get_app_description offset to compute: app_desc is at offset 0x20
    // in the first segment, meaning first segment starts at sizeof(esp_image_header_t) +
    // sizeof(esp_image_segment_header_t). We'll read through segments.
    esp_image_header_t hdr;
    if (esp_partition_read(running, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        _fw_hash[0] = '\0';
        return;
    }
    // Walk through all segments to find total image size
    size_t imgOffset = sizeof(esp_image_header_t);
    for (int seg = 0; seg < hdr.segment_count; seg++) {
        esp_image_segment_header_t seg_hdr;
        if (esp_partition_read(running, imgOffset, &seg_hdr, sizeof(seg_hdr)) != ESP_OK) {
            _fw_hash[0] = '\0';
            return;
        }
        imgOffset += sizeof(seg_hdr) + seg_hdr.data_len;
    }
    // After segments: 1 byte checksum, then padded to 16-byte boundary, then optional hash (32 bytes)
    size_t fwSize = imgOffset + 1;  // +1 for checksum byte
    fwSize = (fwSize + 15) & ~15;  // Align to 16 bytes
    if (hdr.hash_appended) {
        fwSize += 32;  // SHA-256 hash appended by esptool
    }
    DBG_DEBUG(TAG, "Firmware image size: %u bytes (partition: %u)", (unsigned)fwSize, (unsigned)running->size);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    uint8_t buf[4096];
    size_t offset = 0;
    while (offset < fwSize) {
        size_t toRead = (fwSize - offset < sizeof(buf)) ? fwSize - offset : sizeof(buf);
        if (esp_partition_read(running, offset, buf, toRead) != ESP_OK) {
            mbedtls_sha256_free(&ctx);
            _fw_hash[0] = '\0';
            return;
        }
        mbedtls_sha256_update(&ctx, buf, toRead);
        offset += toRead;
    }

    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    for (int i = 0; i < 32; i++) {
        snprintf(&_fw_hash[i * 2], 3, "%02x", hash[i]);
    }
}

static void cacheFirmwareInfo() {
    // Version from app description
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc) {
        strncpy(_fw_version, desc->version, sizeof(_fw_version) - 1);
        _fw_version[sizeof(_fw_version) - 1] = '\0';
    } else {
        strncpy(_fw_version, "unknown", sizeof(_fw_version) - 1);
    }

    // Board name
#ifdef DISPLAY_DRIVER
    _board_name = DISPLAY_DRIVER;
#else
    _board_name = "unknown";
#endif

    // Firmware hash (expensive, do once)
    computeFirmwareHash();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init(const HeartbeatConfig& config) {
    if (_initialized) return _active;

    _initialized = true;
    _interval_ms = config.interval_ms;

    // Resolve server_url and device_id
    if (config.server_url && config.server_url[0] != '\0') {
        strncpy(_server_url, config.server_url, sizeof(_server_url) - 1);
    }
    if (config.device_id && config.device_id[0] != '\0') {
        strncpy(_device_id, config.device_id, sizeof(_device_id) - 1);
    }

    // If not provided, try provisioning
    if (_server_url[0] == '\0' || _device_id[0] == '\0') {
        if (!_provision_inited) {
            _provision_inited = _provision.init();
        }
        if (_provision_inited && _provision.isProvisioned()) {
            const DeviceIdentity& id = _provision.getIdentity();
            if (_server_url[0] == '\0' && id.server_url[0] != '\0') {
                strncpy(_server_url, id.server_url, sizeof(_server_url) - 1);
            }
            if (_device_id[0] == '\0' && id.device_id[0] != '\0') {
                strncpy(_device_id, id.device_id, sizeof(_device_id) - 1);
            }
        }
    }

    // Check if we have enough config to send heartbeats
    if (_server_url[0] == '\0' || _device_id[0] == '\0') {
        DBG_INFO(TAG, "Not configured, skipping");
        _active = false;
        return false;
    }

    // Cache firmware info (version, hash, board)
    cacheFirmwareInfo();

    // Device group — from config or NVS
    if (config.device_group && config.device_group[0] != '\0') {
        strncpy(_device_group, config.device_group, sizeof(_device_group) - 1);
    } else if (_provision_inited) {
        // Try to load from NVS (persisted via set_group())
        Preferences prefs;
        if (prefs.begin("tritium", true)) {
            String grp = prefs.getString("device_group", "");
            if (grp.length() > 0) {
                strncpy(_device_group, grp.c_str(), sizeof(_device_group) - 1);
            }
            prefs.end();
        }
    }

    // Device lifecycle state — load from NVS
    if (_provision_inited) {
        Preferences prefs;
        if (prefs.begin("tritium", true)) {
            String lcs = prefs.getString("lifecycle_state", "active");
            if (lcs.length() > 0) {
                strncpy(_lifecycle_state, lcs.c_str(), sizeof(_lifecycle_state) - 1);
            }
            prefs.end();
        }
    }

    // Initialize CoT if requested
    _cot_enabled = config.cot_enabled;
#if HAS_COT
    if (_cot_enabled) {
        hal_cot::CotConfig cot_cfg;
        cot_cfg.device_id = _device_id;
        cot_cfg.interval_ms = _interval_ms;
        if (hal_cot::init(cot_cfg)) {
            DBG_INFO(TAG, "CoT enabled — will send position on heartbeat ticks");
        } else {
            DBG_WARN(TAG, "CoT init failed, continuing without CoT");
            _cot_enabled = false;
        }
    }
#else
    if (_cot_enabled) {
        DBG_WARN(TAG, "CoT requested but hal_cot not available");
        _cot_enabled = false;
    }
#endif

    // Initialize sensor self-test (runs every hour)
#if HAS_SELF_TEST
    sensor_self_test::init(3600000);  // 1 hour
#endif

    // Initialize tamper detection (5-minute silence threshold)
#if HAS_TAMPER_DETECT
    hal_tamper_detect::init();
#endif

    // Initialize network quality monitoring
#if HAS_NET_QUALITY
    network_quality::init();
#endif

    _active = true;
    _last_send_ms = 0;  // Send first heartbeat on next tick()
    DBG_INFO(TAG, "Initialized: server=%s device=%s interval=%lums",
             _server_url, _device_id, (unsigned long)_interval_ms);
    return true;
}

bool tick() {
    if (!_active) return false;
    if (!WiFi.isConnected()) return false;

    uint32_t now = millis();
    if (_last_send_ms != 0 && (now - _last_send_ms) < _interval_ms) {
        return false;
    }
    _last_send_ms = now;

    return send_now();
}

bool send_now() {
    if (!_active) return false;
    if (!WiFi.isConnected()) return false;

    char url[320];
    snprintf(url, sizeof(url), "%s/api/devices/%s/status", _server_url, _device_id);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    _heartbeat_seq++;
    bool is_compact = _compact_mode && (_heartbeat_seq % 2 == 1);

    // Build JSON payload — matches the format used by OtaApp::sendHeartbeat()
    const char* partition = "unknown";
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) partition = running->label;

    // Build base JSON payload — reuses PSRAM buffer
    static char* body = nullptr;
    static constexpr size_t BODY_SIZE = 3072;
    if (!body) {
        body = (char*)heap_caps_malloc(BODY_SIZE, MALLOC_CAP_SPIRAM);
        if (!body) body = (char*)malloc(BODY_SIZE);
    }

    int pos;

    if (is_compact) {
        // COMPACT HEARTBEAT — essential fields only, target <200 bytes
        // Fields: version, uptime_s, free_heap, rssi, caps, compact flag
        pos = snprintf(body, BODY_SIZE,
                 "{\"v\":\"%s\",\"up\":%lu,\"heap\":%u,"
                 "\"rssi\":%d,\"caps\":%u,\"compact\":true}",
                 _fw_version,
                 (unsigned long)(millis() / 1000),
                 (unsigned)ESP.getFreeHeap(),
                 WiFi.RSSI(),
                 (unsigned)get_capabilities());
        // Compact heartbeat is complete — skip all optional sections
        goto send_heartbeat;
    }

    // FULL HEARTBEAT — all fields
    pos = snprintf(body, BODY_SIZE,
             "{\"version\":\"%s\",\"board\":\"%s\",\"partition\":\"%s\","
             "\"ip\":\"%s\",\"mac\":\"%s\",\"uptime_s\":%lu,\"free_heap\":%u,"
             "\"rssi\":%d,\"fw_hash\":\"%s\","
             "\"reported_config\":{"
             "\"heartbeat_interval_s\":%lu,"
             "\"server_url\":\"%s\","
             "\"wifi_ssid\":\"%s\","
             "\"firmware_version\":\"%s\""
             "}",
             _fw_version, _board_name, partition,
             WiFi.localIP().toString().c_str(),
             WiFi.macAddress().c_str(),
             (unsigned long)(millis() / 1000),
             (unsigned)ESP.getFreeHeap(),
             WiFi.RSSI(),
             _fw_hash,
             (unsigned long)(_interval_ms / 1000),
             _server_url,
             WiFi.SSID().c_str(),
             _fw_version);

    // Append device group assignment if set
    if (_device_group[0] != '\0') {
        pos += snprintf(body + pos, BODY_SIZE - pos,
                        ",\"device_group\":\"%s\"", _device_group);
    }

    // Append compact capabilities bitfield — 1 byte encoding which HALs are compiled in
    pos += snprintf(body + pos, BODY_SIZE - pos,
                    ",\"caps\":%u", (unsigned)get_capabilities());

    // Append BLE scanner data if available
#if HAS_BLE_SCANNER
    if (hal_ble_scanner::is_active()) {
        char ble_json[128];
        hal_ble_scanner::get_summary_json(ble_json, sizeof(ble_json));
        pos += snprintf(body + pos, BODY_SIZE - pos, ",\"sensors\":%s", ble_json);
        // Append per-device list if space permits
        if (pos < (int)BODY_SIZE - 200) {
            char devs_json[2048];  // stack — only used briefly
            hal_ble_scanner::get_devices_json(devs_json, sizeof(devs_json));
            pos += snprintf(body + pos, BODY_SIZE - pos, ",\"ble_devices\":%s", devs_json);
        }
    }
#endif

    // Append WiFi scanner summary if available
#if HAS_WIFI_SCANNER
    if (hal_wifi_scanner::is_active()) {
        int wifi_count = hal_wifi_scanner::get_visible_count();
        // Classify visible networks to get type distribution
        hal_wifi_scanner::WifiNetwork nets[16];
        int n = hal_wifi_scanner::get_networks(nets, 16);
        int type_counts[12] = {};
        for (int i = 0; i < n; i++) {
            auto cl = wifi_classifier::classify(nets[i].ssid, nets[i].auth_type, nets[i].rssi);
            int tid = (int)cl.type;
            if (tid >= 0 && tid < 12) type_counts[tid]++;
        }
        pos += snprintf(body + pos, BODY_SIZE - pos,
            ",\"wifi_scan\":{\"count\":%d,\"home\":%d,\"hotspot\":%d,"
            "\"iot\":%d,\"corporate\":%d,\"public\":%d,\"hidden\":%d}",
            wifi_count,
            type_counts[(int)wifi_classifier::NetworkType::HOME_ROUTER],
            type_counts[(int)wifi_classifier::NetworkType::MOBILE_HOTSPOT],
            type_counts[(int)wifi_classifier::NetworkType::IOT_DEVICE],
            type_counts[(int)wifi_classifier::NetworkType::CORPORATE],
            type_counts[(int)wifi_classifier::NetworkType::PUBLIC_OPEN],
            type_counts[(int)wifi_classifier::NetworkType::HIDDEN]);
    }
#endif

    // Append RF motion monitor summary if available
#if HAS_RF_MONITOR
    if (hal_rf_monitor::is_active()) {
        char rf_summary[64];
        hal_rf_monitor::get_summary_json(rf_summary, sizeof(rf_summary));
        pos += snprintf(body + pos, BODY_SIZE - pos, ",\"rf_monitor\":%s", rf_summary);
    }
#endif

    // Append acoustic sensor summary if available
#if HAS_ACOUSTIC
    if (hal_acoustic::is_active()) {
        char acoustic_summary[128];
        hal_acoustic::get_summary_json(acoustic_summary, sizeof(acoustic_summary));
        pos += snprintf(body + pos, BODY_SIZE - pos, ",\"acoustic\":%s", acoustic_summary);
    }
#endif

    // Append config sync status if available
#if HAS_CONFIG_SYNC
    {
        char cfg_summary[256];
        hal_config_sync::get_config_json(cfg_summary, sizeof(cfg_summary));
        pos += snprintf(body + pos, BODY_SIZE - pos, ",\"config_sync\":%s", cfg_summary);
    }
#endif

    // Append persistent diagnostic log counts if available
#if HAS_DIAGLOG
    pos += snprintf(body + pos, BODY_SIZE - pos,
                    ",\"diag_event_count\":%d,\"diag_boot_count\":%u",
                    diaglog_count(), (unsigned)diaglog_boot_count());
#endif

    // Append transport status array — lists all available communication channels
    pos += snprintf(body + pos, BODY_SIZE - pos,
                    ",\"transports\":[{\"type\":\"wifi\",\"state\":\"available\",\"rssi\":%d}",
                    WiFi.RSSI());
#if HAS_ESPNOW
    {
        auto& mesh = MeshManager::instance();
        if (mesh.isReady()) {
            auto& mstats = mesh.getStats();
            pos += snprintf(body + pos, BODY_SIZE - pos,
                ",{\"type\":\"esp_now\",\"state\":\"available\","
                "\"peers\":%d,\"tx\":%lu,\"rx\":%lu,\"tx_fail\":%lu}",
                mstats.peer_count,
                (unsigned long)mstats.tx_count,
                (unsigned long)mstats.rx_count,
                (unsigned long)mstats.tx_fail);
            // Append peer quality array for comm-link visualization
            if (pos < (int)BODY_SIZE - 200 && mstats.peer_count > 0) {
                pos += snprintf(body + pos, BODY_SIZE - pos, ",\"mesh_peers\":");
                pos += mesh.peersToJson(body + pos, BODY_SIZE - pos);
            }
        } else {
            pos += snprintf(body + pos, BODY_SIZE - pos,
                            ",{\"type\":\"esp_now\",\"state\":\"available\"}");
        }
    }
#endif
#if HAS_BLE_SCANNER
    if (hal_ble_scanner::is_active()) {
        pos += snprintf(body + pos, BODY_SIZE - pos,
                        ",{\"type\":\"ble\",\"state\":\"available\"}");
    }
#endif
#if HAS_LORA
    if (hal_lora::is_active()) {
        const char* mode = hal_lora::get_mode();
        int lora_rssi = hal_lora::get_last_rssi();
        pos += snprintf(body + pos, BODY_SIZE - pos,
                        ",{\"type\":\"lora\",\"state\":\"available\",\"rssi\":%d}", lora_rssi);
    }
#endif
    pos += snprintf(body + pos, BODY_SIZE - pos, "]");

    // Report OTA result if we just rebooted after a fleet OTA
#if HAS_OTA_MANAGER
    {
        static bool _ota_result_reported = false;
        if (!_ota_result_reported) {
            // Check if we booted into a new partition (OTA just happened)
            const esp_partition_t* boot_part = esp_ota_get_boot_partition();
            if (boot_part && running && boot_part == running) {
                // We're running from the expected partition — OTA succeeded
                const auto& st = ota_manager::getStatus();
                if (st.current_version[0]) {
                    pos += snprintf(body + pos, BODY_SIZE - pos,
                        ",\"ota_result\":{\"status\":\"success\","
                        "\"version\":\"%s\"}", st.current_version);
                }
            }
            _ota_result_reported = true;
        }
    }
#endif

    // Tick sensor self-test and append results
#if HAS_SELF_TEST
    sensor_self_test::tick();  // Runs test if interval has elapsed
    if (sensor_self_test::get_result().run_count > 0 && pos < (int)BODY_SIZE - 256) {
        char st_json[256];
        sensor_self_test::to_json(st_json, sizeof(st_json));
        pos += snprintf(body + pos, BODY_SIZE - pos, ",\"self_test\":%s", st_json);
    }
#endif

    // Tick tamper detection and append status
#if HAS_TAMPER_DETECT
    hal_tamper_detect::tick();
    if (pos < (int)BODY_SIZE - 128) {
        char tamper_json[128];
        hal_tamper_detect::to_json(tamper_json, sizeof(tamper_json));
        pos += snprintf(body + pos, BODY_SIZE - pos, ",\"tamper\":%s", tamper_json);
    }
#endif

    // Append network quality metrics (latency, packet loss, stability)
#if HAS_NET_QUALITY
    network_quality::tick();  // Run measurement on each heartbeat
    if (pos < (int)BODY_SIZE - 256) {
        char nq_json[256];
        int nq_n = network_quality::to_json(nq_json, sizeof(nq_json));
        if (nq_n > 0) {
            pos += snprintf(body + pos, BODY_SIZE - pos, ",\"network_quality\":%s", nq_json);
        }
    }
#endif

    // Append power consumption tracking data
#if HAS_POWER_TRACKER
    if (pos < (int)BODY_SIZE - 300) {
        char pwr_json[256];
        int pwr_n = PowerTracker::instance().toJson(pwr_json, sizeof(pwr_json));
        if (pwr_n > 0) {
            pos += snprintf(body + pos, BODY_SIZE - pos, ",\"power_tracking\":%s", pwr_json);
        }
    }
#endif

    // Close JSON object
    if (pos < (int)BODY_SIZE - 1) {
        body[pos++] = '}';
        body[pos] = '\0';
    }

send_heartbeat:
    DBG_DEBUG(TAG, "Heartbeat size: %d bytes (%s)", pos, is_compact ? "compact" : "full");

    int code = http.POST(body);
    if (code == 200) {
        DBG_DEBUG(TAG, "Sent to %s - %d", _server_url, code);

        // Parse response for server-directed interval or OTA
        String response = http.getString();

        // Check for heartbeat_interval_s adjustment
        int idx = response.indexOf("\"heartbeat_interval_s\":");
        if (idx >= 0) {
            int valStart = idx + 23;  // length of "heartbeat_interval_s":
            int newInterval = response.substring(valStart).toInt();
            if (newInterval > 0 && newInterval <= 3600) {
                uint32_t newMs = (uint32_t)newInterval * 1000;
                if (newMs != _interval_ms) {
                    DBG_INFO(TAG, "Server adjusted interval: %lus -> %ds",
                             (unsigned long)(_interval_ms / 1000), newInterval);
                    _interval_ms = newMs;
                }
            }
        }

        // Check for desired_config — apply server-pushed settings
        if (response.indexOf("\"desired_config\"") >= 0) {
            // Parse heartbeat_interval_s from desired_config
            int cfgIdx = response.indexOf("\"desired_config\"");
            if (cfgIdx >= 0) {
                int intervalIdx = response.indexOf("\"heartbeat_interval_s\":", cfgIdx);
                if (intervalIdx >= 0) {
                    int valStart = intervalIdx + 23;
                    int newInterval = response.substring(valStart).toInt();
                    if (newInterval > 0 && newInterval <= 3600) {
                        uint32_t newMs = (uint32_t)newInterval * 1000;
                        if (newMs != _interval_ms) {
                            DBG_INFO(TAG, "Config: heartbeat_interval=%ds", newInterval);
                            _interval_ms = newMs;
                        }
                    }
                }
                DBG_DEBUG(TAG, "Received desired_config from server");
            }
        }

        // Check for OTA directive — trigger URL-based OTA update
        if (response.indexOf("\"ota\"") >= 0 && response.indexOf("\"url\"") >= 0) {
            int urlStart = response.indexOf("\"url\":\"") + 7;
            int urlEnd = response.indexOf("\"", urlStart);
            if (urlStart > 6 && urlEnd > urlStart) {
                String otaUrl = response.substring(urlStart, urlEnd);
                // If URL is relative (starts with /), prepend server base URL
                if (otaUrl.startsWith("/")) {
                    otaUrl = String(_server_url) + otaUrl;
                }
                DBG_INFO(TAG, "Server scheduled OTA: %s", otaUrl.c_str());
#if HAS_OTA_MANAGER
                ota_manager::init();
                if (!ota_manager::updateFromUrl(otaUrl.c_str())) {
                    DBG_ERROR(TAG, "Fleet OTA failed: %s",
                              ota_manager::getStatus().error_msg);
                } else {
                    DBG_INFO(TAG, "Fleet OTA complete, rebooting...");
                    delay(500);
                    ota_manager::reboot();
                }
#endif
            }
        }
    } else {
        DBG_DEBUG(TAG, "Sent to %s - HTTP %d", _server_url, code);
    }

    http.end();

    // Piggyback CoT position update on heartbeat tick
#if HAS_COT
    if (_cot_enabled && hal_cot::is_active()) {
        hal_cot::tick();
    }
#endif

    // Piggyback diagnostic report upload on heartbeat tick
#if HAS_DIAG
    if (code == 200) {
        static char* diag_buf = nullptr;
        if (!diag_buf) diag_buf = (char*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!diag_buf) return false;
        int diag_len = hal_diag::full_report_json(diag_buf, 4096);
        if (diag_len > 0) {
            char diag_url[320];
            snprintf(diag_url, sizeof(diag_url), "%s/api/devices/%s/diag",
                     _server_url, _device_id);
            HTTPClient diag_http;
            diag_http.begin(diag_url);
            diag_http.addHeader("Content-Type", "application/json");
            diag_http.setTimeout(3000);
            int diag_code = diag_http.POST((uint8_t*)diag_buf, diag_len);
            if (diag_code == 200) {
                DBG_DEBUG(TAG, "Diag report sent (%d bytes)", diag_len);
            } else {
                DBG_DEBUG(TAG, "Diag report failed: HTTP %d", diag_code);
            }
            diag_http.end();
        }
    }
#endif

    // Piggyback persistent diaglog event upload on heartbeat tick
#if HAS_DIAGLOG
    if (code == 200 && diaglog_count() > 0) {
        // Upload events as JSON batch: {"events":[...],"boot_count":N}
        static char* dl_buf = nullptr;
        static char* dl_post = nullptr;
        if (!dl_buf) dl_buf = (char*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!dl_post) dl_post = (char*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (dl_buf && dl_post) {
        int offset = 0;
        int batch_size = 50;  // Upload up to 50 events per heartbeat
        int ev_len = diaglog_get_json(dl_buf + 128, 4096 - 128,
                                       offset, batch_size);
        if (ev_len > 0) {
            int plen = snprintf(dl_post, 4096,
                "{\"events\":%s,\"boot_count\":%u}",
                dl_buf + 128, (unsigned)diaglog_boot_count());

            char dl_url[320];
            snprintf(dl_url, sizeof(dl_url), "%s/api/devices/%s/diag/log",
                     _server_url, _device_id);
            HTTPClient dl_http;
            dl_http.begin(dl_url);
            dl_http.addHeader("Content-Type", "application/json");
            dl_http.setTimeout(3000);
            int dl_code = dl_http.POST((uint8_t*)dl_post, plen);
            if (dl_code == 200) {
                DBG_DEBUG(TAG, "Diaglog events uploaded (%d bytes)", plen);
            } else {
                DBG_DEBUG(TAG, "Diaglog upload failed: HTTP %d", dl_code);
            }
            dl_http.end();
        }
        } // dl_buf && dl_post
    }
#endif

    return (code == 200);
}

bool is_active() {
    return _active;
}

uint32_t get_interval_ms() {
    return _interval_ms;
}

void set_group(const char* group) {
    if (group && group[0] != '\0') {
        strncpy(_device_group, group, sizeof(_device_group) - 1);
        _device_group[sizeof(_device_group) - 1] = '\0';
    } else {
        _device_group[0] = '\0';
    }
    // Persist to NVS
    Preferences prefs;
    if (prefs.begin("tritium", false)) {
        prefs.putString("device_group", _device_group);
        prefs.end();
        DBG_INFO(TAG, "Device group set: '%s' (persisted)", _device_group);
    }
}

const char* get_group() {
    return _device_group;
}

void set_lifecycle_state(const char* state) {
    if (state && state[0] != '\0') {
        // Validate allowed states
        const char* valid[] = {"provisioning", "active", "maintenance", "retired", "error"};
        bool is_valid = false;
        for (int i = 0; i < 5; i++) {
            if (strcmp(state, valid[i]) == 0) {
                is_valid = true;
                break;
            }
        }
        if (!is_valid) {
            DBG_WARN(TAG, "Invalid lifecycle state: '%s'", state);
            return;
        }
        strncpy(_lifecycle_state, state, sizeof(_lifecycle_state) - 1);
        _lifecycle_state[sizeof(_lifecycle_state) - 1] = '\0';
    } else {
        strncpy(_lifecycle_state, "active", sizeof(_lifecycle_state) - 1);
    }

    // Persist to NVS
    Preferences prefs;
    if (prefs.begin("tritium", false)) {
        prefs.putString("lifecycle_state", _lifecycle_state);
        prefs.end();
        DBG_INFO(TAG, "Lifecycle state set: '%s' (persisted)", _lifecycle_state);
    }
}

const char* get_lifecycle_state() {
    return _lifecycle_state;
}

void set_compact_mode(bool enabled) {
    _compact_mode = enabled;
    DBG_INFO(TAG, "Compact heartbeat mode: %s", enabled ? "ON" : "OFF");
}

bool is_compact_mode() {
    return _compact_mode;
}

}  // namespace hal_heartbeat

#endif  // SIMULATOR
