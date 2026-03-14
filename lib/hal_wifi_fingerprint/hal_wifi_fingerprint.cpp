// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.

// WiFi RSSI Fingerprint Collection implementation
//
// Uses WifiManager::startScan() to capture visible APs and their RSSI.
// Averages multiple scans for stability. Stores fingerprints in memory
// for MQTT publishing to the command center.

#include "hal_wifi_fingerprint.h"
#include <cstdio>
#include <cstring>
#include <cmath>

#ifdef ARDUINO
#include <Arduino.h>
#include "wifi_manager.h"
#else
// Desktop stub
static uint32_t millis() { return 0; }
static void delay(uint32_t ms) { (void)ms; }
#endif

namespace hal_wifi_fingerprint {

static Fingerprint _stored[FP_MAX_STORED];
static int _count = 0;
static uint32_t _next_id = 1;
static bool _collecting = false;
static FingerprintConfig _config;
static bool _initialized = false;

bool init(const FingerprintConfig& config) {
    _config = config;
    _count = 0;
    _next_id = 1;
    _collecting = config.enabled;
    _initialized = true;
    return true;
}

void shutdown() {
    _collecting = false;
    _initialized = false;
}

void set_collecting(bool active) {
    _collecting = active;
}

bool is_collecting() {
    return _collecting;
}

uint32_t record_fingerprint(double lat, double lon, int8_t floor_level,
                            const char* room_id, const char* plan_id) {
    if (!_initialized || !_collecting) return 0;
    if (_count >= FP_MAX_STORED) return 0;

#ifdef ARDUINO
    WifiManager* wifi = WifiManager::_instance;
    if (!wifi) return 0;

    // Accumulate RSSI over multiple scans
    struct APAccum {
        uint8_t bssid[6];
        char ssid[33];
        int32_t rssi_sum;
        uint8_t channel;
        int scan_hits;
    };
    APAccum accum[FP_MAX_APS];
    int accum_count = 0;

    for (int scan = 0; scan < _config.scan_count; ++scan) {
        if (scan > 0) delay(_config.scan_delay_ms);

        wifi->startScan();
        delay(2000);  // Wait for scan to complete

        ScanResult results[WIFI_MAX_SCAN_RESULTS];
        int found = wifi->getScanResults(results, WIFI_MAX_SCAN_RESULTS);

        for (int i = 0; i < found && accum_count < FP_MAX_APS; ++i) {
            // Check if this AP is already in accumulator
            bool existing = false;
            for (int j = 0; j < accum_count; ++j) {
                // Compare by SSID+channel as proxy (no raw BSSID in ScanResult)
                if (strcmp(accum[j].ssid, results[i].ssid) == 0 &&
                    accum[j].channel == results[i].channel) {
                    accum[j].rssi_sum += results[i].rssi;
                    accum[j].scan_hits++;
                    existing = true;
                    break;
                }
            }
            if (!existing) {
                APAccum& a = accum[accum_count++];
                memset(&a, 0, sizeof(a));
                strncpy(a.ssid, results[i].ssid, 32);
                a.ssid[32] = 0;
                a.rssi_sum = results[i].rssi;
                a.channel = results[i].channel;
                a.scan_hits = 1;
                // Generate a pseudo-BSSID from SSID hash (real BSSID not in ScanResult)
                uint32_t hash = 0;
                for (const char* p = results[i].ssid; *p; ++p)
                    hash = hash * 31 + *p;
                a.bssid[0] = 0x02;  // Locally administered
                a.bssid[1] = (hash >> 24) & 0xFF;
                a.bssid[2] = (hash >> 16) & 0xFF;
                a.bssid[3] = (hash >> 8) & 0xFF;
                a.bssid[4] = hash & 0xFF;
                a.bssid[5] = results[i].channel;
            }
        }
    }

    if (accum_count == 0) return 0;

    // Store the fingerprint
    Fingerprint& fp = _stored[_count];
    memset(&fp, 0, sizeof(fp));
    fp.id = _next_id++;
    fp.lat = lat;
    fp.lon = lon;
    fp.floor_level = floor_level;
    if (room_id) { strncpy(fp.room_id, room_id, 32); fp.room_id[32] = 0; }
    if (plan_id) { strncpy(fp.plan_id, plan_id, 32); fp.plan_id[32] = 0; }
    fp.timestamp = millis();
    fp.published = false;

    for (int i = 0; i < accum_count && i < FP_MAX_APS; ++i) {
        fp.aps[i].rssi = (int8_t)(accum[i].rssi_sum / accum[i].scan_hits);
        memcpy(fp.aps[i].bssid, accum[i].bssid, 6);
        strncpy(fp.aps[i].ssid, accum[i].ssid, 32);
        fp.aps[i].ssid[32] = 0;
        fp.aps[i].channel = accum[i].channel;
    }
    fp.ap_count = accum_count;

    _count++;
    return fp.id;
#else
    // Desktop stub — simulate a fingerprint
    Fingerprint& fp = _stored[_count];
    memset(&fp, 0, sizeof(fp));
    fp.id = _next_id++;
    fp.lat = lat;
    fp.lon = lon;
    fp.floor_level = floor_level;
    if (room_id) { strncpy(fp.room_id, room_id, 32); fp.room_id[32] = 0; }
    if (plan_id) { strncpy(fp.plan_id, plan_id, 32); fp.plan_id[32] = 0; }
    fp.timestamp = 0;
    fp.published = false;
    fp.ap_count = 0;
    _count++;
    return fp.id;
#endif
}

int get_fingerprints(Fingerprint* out, int max_count) {
    int n = (_count < max_count) ? _count : max_count;
    if (n > 0) memcpy(out, _stored, n * sizeof(Fingerprint));
    return n;
}

int get_stored_count() { return _count; }

int get_unpublished_count() {
    int n = 0;
    for (int i = 0; i < _count; ++i)
        if (!_stored[i].published) n++;
    return n;
}

void mark_published(uint32_t id) {
    for (int i = 0; i < _count; ++i)
        if (_stored[i].id == id) { _stored[i].published = true; return; }
}

void mark_all_published() {
    for (int i = 0; i < _count; ++i)
        _stored[i].published = true;
}

void clear() {
    _count = 0;
}

// -- JSON output ---------------------------------------------------------------

static void bssid_to_str(const uint8_t* bssid, char* out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

int get_fingerprint_json(const Fingerprint& fp, char* buf, size_t size) {
    int off = snprintf(buf, size,
        "{\"id\":%u,\"lat\":%.8f,\"lon\":%.8f,\"floor\":%d,"
        "\"room_id\":\"%s\",\"plan_id\":\"%s\","
        "\"ap_count\":%u,\"aps\":[",
        fp.id, fp.lat, fp.lon, fp.floor_level,
        fp.room_id, fp.plan_id,
        fp.ap_count);

    for (int i = 0; i < fp.ap_count && off < (int)size - 80; ++i) {
        char bssid_str[18];
        bssid_to_str(fp.aps[i].bssid, bssid_str);
        if (i > 0) buf[off++] = ',';
        off += snprintf(buf + off, size - off,
            "{\"bssid\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u}",
            bssid_str, fp.aps[i].ssid, fp.aps[i].rssi, fp.aps[i].channel);
    }

    off += snprintf(buf + off, size - off, "]}");
    return off;
}

int get_unpublished_json(char* buf, size_t size) {
    int off = snprintf(buf, size, "{\"fingerprints\":[");

    bool first = true;
    for (int i = 0; i < _count && off < (int)size - 200; ++i) {
        if (_stored[i].published) continue;
        if (!first) buf[off++] = ',';
        first = false;
        off += get_fingerprint_json(_stored[i], buf + off, size - off);
    }

    off += snprintf(buf + off, size - off, "],\"count\":%d}", get_unpublished_count());
    return off;
}

}  // namespace hal_wifi_fingerprint
