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

#include <Arduino.h>
#include <WiFi.h>
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

static void process_scan_results() {
    int n = WiFi.scanComplete();
    if (n < 0) return;  // scan not done or error

    uint32_t now = millis();

    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < n; i++) {
            uint8_t* bssid = WiFi.BSSID(i);
            if (!bssid) continue;

            int idx = find_by_bssid(bssid);
            if (idx >= 0) {
                // Update existing entry
                _networks[idx].rssi = WiFi.RSSI(i);
                _networks[idx].channel = WiFi.channel(i);
                _networks[idx].last_seen = now;
                _networks[idx].seen_count++;
                // Update SSID in case it changed (hidden -> visible)
                const char* ssid = WiFi.SSID(i).c_str();
                if (ssid && ssid[0]) {
                    strncpy(_networks[idx].ssid, ssid, sizeof(_networks[idx].ssid) - 1);
                    _networks[idx].ssid[sizeof(_networks[idx].ssid) - 1] = '\0';
                }
            } else if (_network_count < WIFI_SCANNER_MAX_NETWORKS) {
                // New network
                WifiNetwork& net = _networks[_network_count];
                memset(&net, 0, sizeof(net));
                memcpy(net.bssid, bssid, 6);
                net.rssi = WiFi.RSSI(i);
                net.channel = WiFi.channel(i);
                net.auth_type = mapAuth(WiFi.encryptionType(i));
                net.first_seen = now;
                net.last_seen = now;
                net.seen_count = 1;

                const char* ssid = WiFi.SSID(i).c_str();
                if (ssid) {
                    strncpy(net.ssid, ssid, sizeof(net.ssid) - 1);
                    net.ssid[sizeof(net.ssid) - 1] = '\0';
                }

                _network_count++;
            }
        }

        prune_stale();
        xSemaphoreGive(_mutex);
    }

    WiFi.scanDelete();
}

// --- Background scan task ---

static void scan_task(void* param) {
    while (_running) {
        // Only scan in STA mode — AP mode does not support scanning
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
            Serial.printf("[wifi_scan] Scanning...\n");
            int result = WiFi.scanNetworks(false, _config.show_hidden, _config.passive);
            if (result >= 0) {
                process_scan_results();
                Serial.printf("[wifi_scan] %d networks found (%d tracked)\n",
                    result, _network_count);
            } else {
                Serial.printf("[wifi_scan] Scan failed (%d)\n", result);
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
            "\"rssi\":%d,\"ch\":%u,\"auth\":\"%s\",\"seen\":%u}",
            _networks[i].ssid,
            _networks[i].bssid[0], _networks[i].bssid[1], _networks[i].bssid[2],
            _networks[i].bssid[3], _networks[i].bssid[4], _networks[i].bssid[5],
            _networks[i].rssi, (unsigned)_networks[i].channel,
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
