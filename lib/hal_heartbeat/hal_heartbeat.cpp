// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_heartbeat.h"
#include "debug_log.h"

#include <cstring>
#include <cstdio>
#include <new>

static constexpr const char* TAG = "heartbeat";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace hal_heartbeat {

static bool _active = false;
static uint32_t _interval_ms = 60000;

bool init(const HeartbeatConfig& config) {
    (void)config;
    DBG_INFO(TAG, "Heartbeat init (simulator stub)");
    return false;
}

bool tick() { return false; }
bool send_now() { return false; }
bool is_active() { return false; }
uint32_t get_interval_ms() { return _interval_ms; }

}  // namespace hal_heartbeat

// ============================================================================
// ESP32 — real HTTP heartbeat
// ============================================================================
#else

#include "tritium_compat.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include "hal_provision.h"

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

namespace hal_heartbeat {

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

// Provisioning — heap-allocated on demand during init(), then freed

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

    // If not provided, try provisioning (heap-allocated, freed after use)
    if (_server_url[0] == '\0' || _device_id[0] == '\0') {
        ProvisionHAL* prov = new (std::nothrow) ProvisionHAL();
        if (prov) {
            if (prov->init() && prov->isProvisioned()) {
                const DeviceIdentity& id = prov->getIdentity();
                if (_server_url[0] == '\0' && id.server_url[0] != '\0') {
                    strncpy(_server_url, id.server_url, sizeof(_server_url) - 1);
                }
                if (_device_id[0] == '\0' && id.device_id[0] != '\0') {
                    strncpy(_device_id, id.device_id, sizeof(_device_id) - 1);
                }
            }
            delete prov;
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

    _active = true;
    _last_send_ms = 0;  // Send first heartbeat on next tick()
    DBG_INFO(TAG, "Initialized: server=%s device=%s interval=%lums",
             _server_url, _device_id, (unsigned long)_interval_ms);
    return true;
}

bool tick() {
    if (!_active) return false;
    { wifi_ap_record_t _ap; if (esp_wifi_sta_get_ap_info(&_ap) != ESP_OK) return false; }

    uint32_t now = millis();
    if (_last_send_ms != 0 && (now - _last_send_ms) < _interval_ms) {
        return false;
    }
    _last_send_ms = now;

    return send_now();
}

// ---------------------------------------------------------------------------
// HTTP helper — performs a POST and returns HTTP status code.
// Optionally reads the response body into resp_buf (if non-null).
// Returns -1 on transport error.
// ---------------------------------------------------------------------------
static int http_post(const char* url, const char* post_data, int post_len,
                     char* resp_buf, size_t resp_buf_size, int timeout_ms) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.method = HTTP_METHOD_POST;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, post_len);

    esp_err_t err = esp_http_client_perform(client);
    int code = -1;
    if (err == ESP_OK) {
        code = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    return code;
}

// HTTP helper — streaming POST that captures response body.
// Uses open/write/fetch_headers/read pattern for response capture.
static int http_post_with_response(const char* url, const char* post_data, int post_len,
                                   char* resp_buf, size_t resp_buf_size, int timeout_ms) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.method = HTTP_METHOD_POST;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, post_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }

    int written = esp_http_client_write(client, post_data, post_len);
    if (written < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int code = esp_http_client_get_status_code(client);

    if (resp_buf && resp_buf_size > 0) {
        int total_read = 0;
        int max_read = (int)resp_buf_size - 1;
        // Read whatever is available, up to buffer size
        if (content_length > 0 && content_length < max_read) {
            max_read = content_length;
        }
        while (total_read < max_read) {
            int rd = esp_http_client_read(client, resp_buf + total_read, max_read - total_read);
            if (rd <= 0) break;
            total_read += rd;
        }
        resp_buf[total_read] = '\0';
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return code;
}

bool send_now() {
    if (!_active) return false;
    { wifi_ap_record_t _ap; if (esp_wifi_sta_get_ap_info(&_ap) != ESP_OK) return false; }

    char url[320];
    snprintf(url, sizeof(url), "%s/api/devices/%s/status", _server_url, _device_id);

    // Build JSON payload — matches the format used by OtaApp::sendHeartbeat()
    const char* partition = "unknown";
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) partition = running->label;

    // Shared scratch buffer for all heartbeat JSON operations (sequential, never concurrent)
    static constexpr size_t HB_BUF_SIZE = 1536;
    static char _hb_buf[HB_BUF_SIZE];
    char* body = _hb_buf;

    // Gather WiFi info via ESP-IDF
    char _hb_ip_str[16] = "0.0.0.0";
    char _hb_mac_str[18] = "00:00:00:00:00:00";
    char _hb_ssid_str[33] = "";
    int8_t _hb_rssi = 0;
    {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                esp_ip4addr_ntoa(&ip_info.ip, _hb_ip_str, sizeof(_hb_ip_str));
            }
        }
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(_hb_mac_str, sizeof(_hb_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            _hb_rssi = ap_info.rssi;
            strncpy(_hb_ssid_str, (const char*)ap_info.ssid, sizeof(_hb_ssid_str) - 1);
            _hb_ssid_str[sizeof(_hb_ssid_str) - 1] = '\0';
        }
    }

    int pos = snprintf(body, HB_BUF_SIZE,
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
             _hb_ip_str,
             _hb_mac_str,
             (unsigned long)(millis() / 1000),
             (unsigned)esp_get_free_heap_size(),
             (int)_hb_rssi,
             _fw_hash,
             (unsigned long)(_interval_ms / 1000),
             _server_url,
             _hb_ssid_str,
             _fw_version);

    // Append BLE scanner data if available
#if HAS_BLE_SCANNER
    if (hal_ble_scanner::is_active()) {
        char ble_json[128];
        hal_ble_scanner::get_summary_json(ble_json, sizeof(ble_json));
        pos += snprintf(body + pos, HB_BUF_SIZE - pos, ",\"sensors\":%s", ble_json);
        // Append per-device list if space permits
        if (pos < (int)HB_BUF_SIZE - 200) {
            char devs_json[512];  // stack — only used briefly
            hal_ble_scanner::get_devices_json(devs_json, sizeof(devs_json));
            pos += snprintf(body + pos, HB_BUF_SIZE - pos, ",\"ble_devices\":%s", devs_json);
        }
    }
#endif

    // Append persistent diagnostic log counts if available
#if HAS_DIAGLOG
    pos += snprintf(body + pos, HB_BUF_SIZE - pos,
                    ",\"diag_event_count\":%d,\"diag_boot_count\":%u",
                    diaglog_count(), (unsigned)diaglog_boot_count());
#endif

    // Append transport status array — lists all available communication channels
    pos += snprintf(body + pos, HB_BUF_SIZE - pos,
                    ",\"transports\":[{\"type\":\"wifi\",\"state\":\"available\",\"rssi\":%d}",
                    (int)_hb_rssi);
#if HAS_ESPNOW
    pos += snprintf(body + pos, HB_BUF_SIZE - pos,
                    ",{\"type\":\"esp_now\",\"state\":\"available\"}");
#endif
#if HAS_BLE_SCANNER
    if (hal_ble_scanner::is_active()) {
        pos += snprintf(body + pos, HB_BUF_SIZE - pos,
                        ",{\"type\":\"ble\",\"state\":\"available\"}");
    }
#endif
#if HAS_LORA
    if (hal_lora::is_active()) {
        const char* mode = hal_lora::get_mode();
        int lora_rssi = hal_lora::get_last_rssi();
        pos += snprintf(body + pos, HB_BUF_SIZE - pos,
                        ",{\"type\":\"lora\",\"state\":\"available\",\"rssi\":%d}", lora_rssi);
    }
#endif
    pos += snprintf(body + pos, HB_BUF_SIZE - pos, "]");

    // Close JSON object
    if (pos < (int)HB_BUF_SIZE - 1) {
        body[pos++] = '}';
        body[pos] = '\0';
    }

    // Response buffer for heartbeat — reuse second half of scratch space
    static char _resp_buf[512];
    int code = http_post_with_response(url, body, pos, _resp_buf, sizeof(_resp_buf), 5000);

    if (code == 200) {
        DBG_DEBUG(TAG, "Sent to %s - %d", _server_url, code);

        // Parse response for server-directed interval or OTA
        const char* response = _resp_buf;

        // Check for heartbeat_interval_s adjustment
        const char* interval_key = "\"heartbeat_interval_s\":";
        const char* p = strstr(response, interval_key);
        if (p) {
            int newInterval = (int)strtol(p + strlen(interval_key), nullptr, 10);
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
        const char* cfg_p = strstr(response, "\"desired_config\"");
        if (cfg_p) {
            const char* cfg_interval = strstr(cfg_p, interval_key);
            if (cfg_interval) {
                int newInterval = (int)strtol(cfg_interval + strlen(interval_key), nullptr, 10);
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

        // Check for OTA directive — store URL in NVS for hal_ota to pick up
        if (strstr(response, "\"ota\"") && strstr(response, "\"url\"")) {
            const char* url_start = strstr(response, "\"url\":\"");
            if (url_start) {
                url_start += 7;  // skip past "url":"
                const char* url_end = strchr(url_start, '"');
                if (url_end && url_end > url_start) {
                    char ota_url[512];
                    int path_len = (int)(url_end - url_start);
                    snprintf(ota_url, sizeof(ota_url), "%s%.*s",
                             _server_url, path_len, url_start);
                    DBG_INFO(TAG, "Server scheduled OTA: %s", ota_url);
                    // Note: OTA execution is left to apps that include hal_ota.
                    // The heartbeat library only logs the directive.
                }
            }
        }
    } else {
        DBG_DEBUG(TAG, "Sent to %s - HTTP %d", _server_url, code);
    }

    // Piggyback CoT position update on heartbeat tick
#if HAS_COT
    if (_cot_enabled && hal_cot::is_active()) {
        hal_cot::tick();
    }
#endif

    // Piggyback diagnostic report upload on heartbeat tick
#if HAS_DIAG
    if (code == 200) {
        // Reuse _hb_buf — body POST is already done
        int diag_len = hal_diag::full_report_json(_hb_buf, HB_BUF_SIZE);
        if (diag_len > 0) {
            char diag_url[320];
            snprintf(diag_url, sizeof(diag_url), "%s/api/devices/%s/diag",
                     _server_url, _device_id);
            int diag_code = http_post(diag_url, _hb_buf, diag_len, nullptr, 0, 3000);
            if (diag_code == 200) {
                DBG_DEBUG(TAG, "Diag report sent (%d bytes)", diag_len);
            } else {
                DBG_DEBUG(TAG, "Diag report failed: HTTP %d", diag_code);
            }
        }
    }
#endif

    // Piggyback persistent diaglog event upload on heartbeat tick
#if HAS_DIAGLOG
    if (code == 200 && diaglog_count() > 0) {
        // Upload events as JSON batch: {"events":[...],"boot_count":N}
        // Reuse _hb_buf — previous POST is done
        int offset = 0;
        int batch_size = 20;
        // Write events into second half of buffer, then wrap into first half
        int ev_len = diaglog_get_json(_hb_buf + 256, HB_BUF_SIZE - 256,
                                       offset, batch_size);
        if (ev_len > 0) {
            char dl_post[512];  // stack — small wrapper
            int plen = snprintf(dl_post, sizeof(dl_post),
                "{\"events\":%s,\"boot_count\":%u}",
                _hb_buf + 256, (unsigned)diaglog_boot_count());

            char dl_url[320];
            snprintf(dl_url, sizeof(dl_url), "%s/api/devices/%s/diag/log",
                     _server_url, _device_id);
            int dl_code = http_post(dl_url, dl_post, plen, nullptr, 0, 3000);
            if (dl_code == 200) {
                DBG_DEBUG(TAG, "Diaglog events uploaded (%d bytes)", plen);
            } else {
                DBG_DEBUG(TAG, "Diaglog upload failed: HTTP %d", dl_code);
            }
        }
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

}  // namespace hal_heartbeat

#endif  // SIMULATOR
