// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.

#include "hal_wifi_probe.h"
#include <cstring>
#include <cstdio>

#ifdef SIMULATOR

// Simulator stubs
namespace hal_wifi_probe {
bool init(const ProbeConfig&) { return false; }
void shutdown() {}
bool is_active() { return false; }
int get_devices(ProbeDevice*, int) { return 0; }
int get_active_count() { return 0; }
int get_devices_json(char* buf, size_t size) { return snprintf(buf, size, "[]"); }
int get_summary_json(char* buf, size_t size) { return snprintf(buf, size, "{\"probe_devices\":0}"); }
int get_sighting_json(const ProbeDevice&, char* buf, size_t size) { return snprintf(buf, size, "{}"); }
}

#else  // ESP32

#include "tritium_compat.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#if __has_include("hal_diag.h")
#include "hal_diag.h"
#define PROBE_LOG(sev, fmt, ...) hal_diag::log(sev, "wifi_probe", fmt, ##__VA_ARGS__)
#else
#define PROBE_LOG(sev, fmt, ...) ((void)0)
#endif

namespace hal_wifi_probe {

// --- IEEE 802.11 frame structures ---
// Probe request is a management frame, subtype 0x04.
// Frame control: 2 bytes, first nibble of subtype is in bits 7-4 of byte 0.

struct __attribute__((packed)) ieee80211_hdr {
    uint16_t frame_control;
    uint16_t duration;
    uint8_t addr1[6];  // Destination (usually broadcast FF:FF:FF:FF:FF:FF)
    uint8_t addr2[6];  // Source (transmitter MAC)
    uint8_t addr3[6];  // BSSID
    uint16_t seq_ctrl;
};

// --- Internal state ---
static ProbeDevice _devices[PROBE_MAX_DEVICES];
static int _device_count = 0;
static SemaphoreHandle_t _mutex = nullptr;
static bool _running = false;
static ProbeConfig _config;
static TaskHandle_t _hop_task = nullptr;

// --- Helpers ---

static bool is_locally_administered(const uint8_t mac[6]) {
    // Locally administered bit: bit 1 of first octet
    return (mac[0] & 0x02) != 0;
}

static int find_device(const uint8_t mac[6]) {
    for (int i = 0; i < _device_count; i++) {
        if (memcmp(_devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static void prune_stale() {
    uint32_t now = millis();
    int write = 0;
    for (int i = 0; i < _device_count; i++) {
        if ((now - _devices[i].last_seen) < PROBE_DEVICE_TIMEOUT_MS) {
            if (write != i) _devices[write] = _devices[i];
            write++;
        }
    }
    _device_count = write;
}

static void add_ssid(ProbeDevice& dev, const char* ssid) {
    if (!ssid || ssid[0] == '\0') return;

    // Check if already tracked
    for (int i = 0; i < dev.ssid_count; i++) {
        if (strncmp(dev.ssids[i].ssid, ssid, 32) == 0) {
            dev.ssids[i].last_seen = millis();
            return;
        }
    }

    // Add new SSID if space available
    if (dev.ssid_count < PROBE_MAX_SSIDS_PER_DEVICE) {
        ProbedSSID& s = dev.ssids[dev.ssid_count];
        strncpy(s.ssid, ssid, sizeof(s.ssid) - 1);
        s.ssid[sizeof(s.ssid) - 1] = '\0';
        s.last_seen = millis();
        dev.ssid_count++;
    }
}

// --- Promiscuous mode callback ---

static void IRAM_ATTR promisc_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    if (len < (int)sizeof(ieee80211_hdr)) return;

    const ieee80211_hdr* hdr = (const ieee80211_hdr*)payload;

    // Check frame type: Management (0x00) and subtype: Probe Request (0x04)
    // frame_control bits: subtype in bits 7-4, type in bits 3-2
    uint8_t frame_type = (hdr->frame_control & 0x0C) >> 2;
    uint8_t frame_subtype = (hdr->frame_control & 0xF0) >> 4;

    if (frame_type != 0 || frame_subtype != 4) return;  // Not a probe request

    const uint8_t* src_mac = hdr->addr2;  // Source MAC (the probing device)
    int8_t rssi = pkt->rx_ctrl.rssi;
    uint8_t channel = pkt->rx_ctrl.channel;

    // Extract SSID from tagged parameters (starts after fixed parameters)
    // Probe request body: no fixed parameters, just tagged parameters starting at hdr end
    const uint8_t* tagged = payload + sizeof(ieee80211_hdr);
    int tagged_len = len - sizeof(ieee80211_hdr);
    char ssid[33] = {};

    // Parse Tag: ID=0 is SSID
    int offset = 0;
    while (offset + 2 <= tagged_len) {
        uint8_t tag_id = tagged[offset];
        uint8_t tag_len = tagged[offset + 1];
        if (offset + 2 + tag_len > tagged_len) break;

        if (tag_id == 0 && tag_len > 0 && tag_len <= 32) {
            memcpy(ssid, tagged + offset + 2, tag_len);
            ssid[tag_len] = '\0';
        }
        offset += 2 + tag_len;
    }

    // Store in device list (mutex-protected)
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        uint32_t now = millis();
        int idx = find_device(src_mac);

        if (idx >= 0) {
            // Update existing device
            _devices[idx].rssi = rssi;
            if (rssi < _devices[idx].rssi_min) _devices[idx].rssi_min = rssi;
            if (rssi > _devices[idx].rssi_max) _devices[idx].rssi_max = rssi;
            _devices[idx].channel = channel;
            _devices[idx].last_seen = now;
            _devices[idx].probe_count++;
            add_ssid(_devices[idx], ssid);
        } else if (_device_count < PROBE_MAX_DEVICES) {
            // New device
            ProbeDevice& dev = _devices[_device_count];
            memset(&dev, 0, sizeof(dev));
            memcpy(dev.mac, src_mac, 6);
            dev.rssi = rssi;
            dev.rssi_min = rssi;
            dev.rssi_max = rssi;
            dev.channel = channel;
            dev.first_seen = now;
            dev.last_seen = now;
            dev.probe_count = 1;
            dev.is_randomized = is_locally_administered(src_mac);
            add_ssid(dev, ssid);
            _device_count++;
        }

        xSemaphoreGive(_mutex);
    }
}

// --- Channel hopping task ---

static void channel_hop_task(void* param) {
    uint8_t ch = 1;
    while (_running && _config.channel_hop) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        ch = (ch % 13) + 1;  // Cycle through channels 1-13
        vTaskDelay(pdMS_TO_TICKS(_config.hop_interval_ms));
    }
    vTaskDelete(nullptr);
}

// --- Public API ---

bool init(const ProbeConfig& config) {
    if (_running) return true;

    _config = config;
    if (!_config.enabled) return false;

    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) return false;

    _device_count = 0;
    _running = true;

    // Enable promiscuous mode
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promisc_callback);

    // Only capture management frames (probe requests)
    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);

    // Start channel hopping if enabled
    if (_config.channel_hop) {
        xTaskCreatePinnedToCore(channel_hop_task, "wifi_probe_hop", 2048,
                                nullptr, 1, &_hop_task, 1);
    }

    Serial.printf("[wifi_probe] Started (hop=%s, report=%lums)\n",
        _config.channel_hop ? "yes" : "no",
        (unsigned long)_config.report_interval_ms);
    return true;
}

void shutdown() {
    _running = false;
    esp_wifi_set_promiscuous(false);

    if (_hop_task) {
        vTaskDelay(pdMS_TO_TICKS(200));
        _hop_task = nullptr;
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

bool is_active() { return _running; }

int get_devices(ProbeDevice* out, int max_count) {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    prune_stale();
    int count = (_device_count < max_count) ? _device_count : max_count;
    memcpy(out, _devices, count * sizeof(ProbeDevice));
    xSemaphoreGive(_mutex);
    return count;
}

int get_active_count() {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t now = millis();
    int count = 0;
    for (int i = 0; i < _device_count; i++) {
        if ((now - _devices[i].last_seen) < PROBE_DEVICE_TIMEOUT_MS) count++;
    }
    xSemaphoreGive(_mutex);
    return count;
}

int get_devices_json(char* buf, size_t size) {
    if (!_mutex) return snprintf(buf, size, "[]");
    xSemaphoreTake(_mutex, portMAX_DELAY);

    uint32_t now = millis();
    int pos = 0;
    pos += snprintf(buf + pos, size - pos, "[");

    bool first = true;
    for (int i = 0; i < _device_count && pos < (int)size - 256; i++) {
        if ((now - _devices[i].last_seen) >= PROBE_DEVICE_TIMEOUT_MS) continue;
        if (!first) pos += snprintf(buf + pos, size - pos, ",");
        first = false;

        const ProbeDevice& d = _devices[i];
        pos += snprintf(buf + pos, size - pos,
            "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"rssi\":%d,\"rssi_min\":%d,\"rssi_max\":%d,"
            "\"ch\":%u,\"probes\":%u,\"random\":%s,"
            "\"ssids\":[",
            d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
            d.rssi, d.rssi_min, d.rssi_max,
            (unsigned)d.channel, (unsigned)d.probe_count,
            d.is_randomized ? "true" : "false");

        for (int j = 0; j < d.ssid_count && pos < (int)size - 64; j++) {
            if (j > 0) pos += snprintf(buf + pos, size - pos, ",");
            pos += snprintf(buf + pos, size - pos, "\"%s\"", d.ssids[j].ssid);
        }
        pos += snprintf(buf + pos, size - pos, "]}");
    }

    pos += snprintf(buf + pos, size - pos, "]");
    xSemaphoreGive(_mutex);
    return pos;
}

int get_summary_json(char* buf, size_t size) {
    if (!_mutex) return snprintf(buf, size, "{\"probe_devices\":0}");
    xSemaphoreTake(_mutex, portMAX_DELAY);

    uint32_t now = millis();
    int active = 0, randomized = 0, total_probes = 0;
    for (int i = 0; i < _device_count; i++) {
        if ((now - _devices[i].last_seen) >= PROBE_DEVICE_TIMEOUT_MS) continue;
        active++;
        if (_devices[i].is_randomized) randomized++;
        total_probes += _devices[i].probe_count;
    }

    int written = snprintf(buf, size,
        "{\"probe_devices\":%d,\"probe_randomized\":%d,\"probe_total\":%d}",
        active, randomized, total_probes);

    xSemaphoreGive(_mutex);
    return written;
}

int get_sighting_json(const ProbeDevice& dev, char* buf, size_t size) {
    int pos = snprintf(buf, size,
        "{\"type\":\"wifi_probe\","
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"rssi\":%d,\"rssi_min\":%d,\"rssi_max\":%d,"
        "\"channel\":%u,\"probe_count\":%u,"
        "\"is_randomized\":%s,"
        "\"ssids\":[",
        dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5],
        dev.rssi, dev.rssi_min, dev.rssi_max,
        (unsigned)dev.channel, (unsigned)dev.probe_count,
        dev.is_randomized ? "true" : "false");

    for (int j = 0; j < dev.ssid_count && pos < (int)size - 64; j++) {
        if (j > 0) pos += snprintf(buf + pos, size - pos, ",");
        pos += snprintf(buf + pos, size - pos, "\"%s\"", dev.ssids[j].ssid);
    }
    pos += snprintf(buf + pos, size - pos, "]}");
    return pos;
}

}  // namespace hal_wifi_probe

#endif  // !SIMULATOR
