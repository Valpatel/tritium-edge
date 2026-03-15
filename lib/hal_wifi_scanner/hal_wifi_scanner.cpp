// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.

#include "hal_wifi_scanner.h"

#ifdef SIMULATOR

namespace hal_wifi_scanner {
bool init(const ScanConfig&) { return false; }
void shutdown() {}
bool is_active() { return false; }
int get_networks(WifiNetwork*, int) { return 0; }
int get_visible_count() { return 0; }
int get_networks_json(char* buf, size_t size) { return snprintf(buf, size, "[]"); }
int get_summary_json(char* buf, size_t size) { return snprintf(buf, size, "{}"); }
}

#else

#include "tritium_compat.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <esp_wifi.h>
#include <cstring>
#include <cstdio>

#if __has_include("hal_diag.h")
#include "hal_diag.h"
#define WIFI_SCAN_LOG(sev, fmt, ...) hal_diag::log(sev, "wifi_scan", fmt, ##__VA_ARGS__)
#else
#define WIFI_SCAN_LOG(sev, fmt, ...) ((void)0)
#endif

namespace hal_wifi_scanner {

// --- Internal state ---
static WifiNetwork _networks[WIFI_SCANNER_MAX_NETWORKS];
static int _network_count = 0;
static SemaphoreHandle_t _mutex = nullptr;

static bool _running = false;
static ScanConfig _config;
static TaskHandle_t _scan_task = nullptr;

// --- Helpers ---

static uint8_t mapAuth(wifi_auth_mode_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN:         return 0;
        case WIFI_AUTH_WEP:          return 1;
        case WIFI_AUTH_WPA_PSK:      return 2;
        case WIFI_AUTH_WPA2_PSK:     return 3;
        case WIFI_AUTH_WPA_WPA2_PSK: return 4;
        case WIFI_AUTH_WPA3_PSK:     return 5;
        default:                     return 6;  // unknown
    }
}

static const char* authStr(uint8_t auth_type) {
    switch (auth_type) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA";
        case 3: return "WPA2";
        case 4: return "WPA/2";
        case 5: return "WPA3";
        default: return "?";
    }
}

static int find_by_bssid(const uint8_t bssid[6]) {
    for (int i = 0; i < _network_count; i++) {
        if (memcmp(_networks[i].bssid, bssid, 6) == 0) return i;
    }
    return -1;
}

static void prune_stale() {
    uint32_t now = millis();
    int write = 0;
    for (int i = 0; i < _network_count; i++) {
        if ((now - _networks[i].last_seen) < WIFI_NETWORK_TIMEOUT_MS) {
            if (write != i) _networks[write] = _networks[i];
            write++;
        }
    }
    _network_count = write;
}

// Estimate SNR from RSSI. ESP32 noise floor is typically around -95 to -100 dBm.
// We use -95 as the noise floor estimate for a clean 2.4 GHz environment.
static constexpr int8_t NOISE_FLOOR_DBM = -95;

static uint8_t estimate_snr(int8_t rssi) {
    int snr = (int)rssi - NOISE_FLOOR_DBM;
    if (snr < 0) snr = 0;
    if (snr > 90) snr = 90;
    return (uint8_t)snr;
}

// Count how many APs share the same primary channel in current scan results
static uint8_t count_channel_peers(wifi_ap_record_t* ap_records, uint16_t ap_count, uint8_t channel) {
    uint8_t count = 0;
    for (uint16_t i = 0; i < ap_count; i++) {
        if (ap_records[i].primary == channel) count++;
    }
    return count;
}

// Deduplicate scan results by BSSID — same AP seen on multiple channels
// is reported as one entry with the stronger RSSI.
static uint16_t dedup_scan_results(wifi_ap_record_t* ap_records, uint16_t ap_count) {
    if (ap_count <= 1) return ap_count;

    uint16_t write = 0;
    for (uint16_t i = 0; i < ap_count; i++) {
        // Check if this BSSID already exists in the deduplicated portion
        bool found = false;
        for (uint16_t j = 0; j < write; j++) {
            if (memcmp(ap_records[j].bssid, ap_records[i].bssid, 6) == 0) {
                // Same BSSID — keep the stronger signal
                if (ap_records[i].rssi > ap_records[j].rssi) {
                    ap_records[j].rssi = ap_records[i].rssi;
                    ap_records[j].primary = ap_records[i].primary;
                    // Prefer non-hidden SSID
                    if (ap_records[i].ssid[0] && !ap_records[j].ssid[0]) {
                        memcpy(ap_records[j].ssid, ap_records[i].ssid, sizeof(ap_records[j].ssid));
                    }
                }
                found = true;
                break;
            }
        }
        if (!found) {
            if (write != i) {
                ap_records[write] = ap_records[i];
            }
            write++;
        }
    }
    return write;
}

static void process_scan_results(wifi_ap_record_t* ap_records, uint16_t ap_count) {
    uint32_t now = millis();

    // Deduplicate: same BSSID on multiple channels = one AP, keep strongest RSSI
    uint16_t deduped_count = dedup_scan_results(ap_records, ap_count);
    if (deduped_count < ap_count) {
        WIFI_SCAN_LOG("INFO", "Deduplicated %d -> %d APs (removed %d multi-channel dupes)",
            (int)ap_count, (int)deduped_count, (int)(ap_count - deduped_count));
    }

    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (uint16_t i = 0; i < deduped_count; i++) {
            wifi_ap_record_t& ap = ap_records[i];
            uint8_t snr = estimate_snr(ap.rssi);
            uint8_t ch_load = count_channel_peers(ap_records, deduped_count, ap.primary);

            int idx = find_by_bssid(ap.bssid);
            if (idx >= 0) {
                // Update existing entry — keep strongest RSSI
                if (ap.rssi > _networks[idx].rssi) {
                    _networks[idx].rssi = ap.rssi;
                    _networks[idx].channel = ap.primary;
                }
                _networks[idx].last_seen = now;
                _networks[idx].seen_count++;
                _networks[idx].snr = snr;
                _networks[idx].channel_load = ch_load;
                // Update SSID in case it changed (hidden -> visible)
                if (ap.ssid[0]) {
                    strncpy(_networks[idx].ssid, (const char*)ap.ssid, sizeof(_networks[idx].ssid) - 1);
                    _networks[idx].ssid[sizeof(_networks[idx].ssid) - 1] = '\0';
                }
            } else if (_network_count < WIFI_SCANNER_MAX_NETWORKS) {
                // New network
                WifiNetwork& net = _networks[_network_count];
                memset(&net, 0, sizeof(net));
                memcpy(net.bssid, ap.bssid, 6);
                net.rssi = ap.rssi;
                net.channel = ap.primary;
                net.auth_type = mapAuth(ap.authmode);
                net.first_seen = now;
                net.last_seen = now;
                net.seen_count = 1;
                net.snr = snr;
                net.channel_load = ch_load;

                strncpy(net.ssid, (const char*)ap.ssid, sizeof(net.ssid) - 1);
                net.ssid[sizeof(net.ssid) - 1] = '\0';

                _network_count++;
            }
        }

        prune_stale();
        xSemaphoreGive(_mutex);
    }
}

// --- Background scan task ---

static void scan_task(void* param) {
    while (_running) {
        // Only scan in STA mode — AP mode does not support scanning
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
            Serial.printf("[wifi_scan] Scanning...\n");
            wifi_scan_config_t scan_cfg = {};
            scan_cfg.show_hidden = _config.show_hidden;
            scan_cfg.scan_type = _config.passive ? WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE;
            esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // blocking scan
            if (err == ESP_OK) {
                uint16_t ap_count = 0;
                esp_wifi_scan_get_ap_num(&ap_count);
                if (ap_count > 0) {
                    uint16_t max_records = (ap_count > WIFI_SCANNER_MAX_NETWORKS) ?
                                           WIFI_SCANNER_MAX_NETWORKS : ap_count;
                    wifi_ap_record_t* ap_records = (wifi_ap_record_t*)malloc(
                        max_records * sizeof(wifi_ap_record_t));
                    if (ap_records) {
                        esp_wifi_scan_get_ap_records(&max_records, ap_records);
                        process_scan_results(ap_records, max_records);
                        free(ap_records);
                    }
                }
                Serial.printf("[wifi_scan] %d networks found (%d tracked)\n",
                    (int)ap_count, _network_count);
            } else {
                Serial.printf("[wifi_scan] Scan failed (0x%x)\n", err);
            }
        } else {
            Serial.printf("[wifi_scan] Skipping — not in STA mode\n");
        }

        vTaskDelay(pdMS_TO_TICKS(_config.scan_interval_ms));
    }

    vTaskDelete(nullptr);
}

// --- Public API ---

bool init(const ScanConfig& config) {
    if (_running) return true;

    _config = config;
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) return false;

    _network_count = 0;

    _running = true;
    xTaskCreatePinnedToCore(scan_task, "wifi_scan", 4096, nullptr, 1, &_scan_task, 1);

    Serial.printf("[wifi_scan] Started (interval=%lums, hidden=%s, passive=%s)\n",
        (unsigned long)config.scan_interval_ms,
        config.show_hidden ? "yes" : "no",
        config.passive ? "yes" : "no");
    return true;
}

void shutdown() {
    _running = false;
    if (_scan_task) {
        vTaskDelay(pdMS_TO_TICKS(500));
        _scan_task = nullptr;
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

bool is_active() {
    return _running;
}

int get_networks(WifiNetwork* out, int max_count) {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int count = (_network_count < max_count) ? _network_count : max_count;
    memcpy(out, _networks, count * sizeof(WifiNetwork));
    xSemaphoreGive(_mutex);
    return count;
}

int get_visible_count() {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t now = millis();
    int count = 0;
    for (int i = 0; i < _network_count; i++) {
        if ((now - _networks[i].last_seen) < WIFI_NETWORK_TIMEOUT_MS) count++;
    }
    xSemaphoreGive(_mutex);
    return count;
}

int get_networks_json(char* buf, size_t size) {
    if (!_mutex) return snprintf(buf, size, "[]");
    xSemaphoreTake(_mutex, portMAX_DELAY);

    uint32_t now = millis();
    int pos = 0;
    pos += snprintf(buf + pos, size - pos, "[");

    bool first = true;
    for (int i = 0; i < _network_count && pos < (int)size - 120; i++) {
        if ((now - _networks[i].last_seen) >= WIFI_NETWORK_TIMEOUT_MS) continue;
        if (!first) pos += snprintf(buf + pos, size - pos, ",");
        first = false;
        pos += snprintf(buf + pos, size - pos,
            "{\"ssid\":\"%s\",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"rssi\":%d,\"snr\":%u,\"ch\":%u,\"ch_load\":%u,"
            "\"auth\":\"%s\",\"seen\":%u}",
            _networks[i].ssid,
            _networks[i].bssid[0], _networks[i].bssid[1], _networks[i].bssid[2],
            _networks[i].bssid[3], _networks[i].bssid[4], _networks[i].bssid[5],
            _networks[i].rssi, (unsigned)_networks[i].snr,
            (unsigned)_networks[i].channel, (unsigned)_networks[i].channel_load,
            authStr(_networks[i].auth_type),
            (unsigned)_networks[i].seen_count);
    }

    pos += snprintf(buf + pos, size - pos, "]");
    xSemaphoreGive(_mutex);
    return pos;
}

int get_summary_json(char* buf, size_t size) {
    if (!_mutex) return snprintf(buf, size, "{}");
    xSemaphoreTake(_mutex, portMAX_DELAY);

    uint32_t now = millis();
    int visible = 0, open_count = 0, secured = 0;
    for (int i = 0; i < _network_count; i++) {
        if ((now - _networks[i].last_seen) >= WIFI_NETWORK_TIMEOUT_MS) continue;
        visible++;
        if (_networks[i].auth_type == 0) {
            open_count++;
        } else {
            secured++;
        }
    }

    int written = snprintf(buf, size,
        "{\"wifi_networks\":%d,\"wifi_open\":%d,\"wifi_secured\":%d}",
        visible, open_count, secured);

    xSemaphoreGive(_mutex);
    return written;
}

}  // namespace hal_wifi_scanner

#endif  // !SIMULATOR
