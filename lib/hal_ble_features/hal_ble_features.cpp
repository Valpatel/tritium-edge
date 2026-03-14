// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_ble_features.h"
#include <cstring>
#include <cstdio>
#include <cmath>

namespace hal_ble_features {

// Feature extraction version — increment when feature set changes
static constexpr int FEATURE_VERSION = 1;

// Classification feedback cache
static CachedClassification _feedback_cache[FEEDBACK_CACHE_SIZE] = {};
static int _feedback_count = 0;

// FNV-1a hash for strings (32-bit)
static uint32_t fnv1a(const char* data, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 16777619u;
    }
    return hash;
}

bool extract(const BleDevice& dev, BleFeatureVector& out) {
    memset(&out, 0, sizeof(out));

    // Feature 0: OUI hash — hash first 3 octets, normalize to 0.0-1.0
    char oui_str[9];
    snprintf(oui_str, sizeof(oui_str), "%02X%02X%02X",
             dev.addr[0], dev.addr[1], dev.addr[2]);
    uint32_t oui_h = fnv1a(oui_str, 6);
    out.features[0] = (float)(oui_h & 0x7FFFFFFF) / (float)0x7FFFFFFF;

    // Feature 1: Name length
    size_t name_len = strlen(dev.name);
    out.features[1] = (float)name_len;

    // Feature 2-3: Name character ratios
    if (name_len > 0) {
        int alpha = 0, digit = 0;
        for (size_t i = 0; i < name_len; i++) {
            char c = dev.name[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) alpha++;
            if (c >= '0' && c <= '9') digit++;
        }
        out.features[2] = (float)alpha / (float)name_len;
        out.features[3] = (float)digit / (float)name_len;
    }

    // Features 4-6: RSSI histogram from history
    if (dev.rssi_history_count > 0) {
        int near = 0, mid = 0, far = 0;
        int count = dev.rssi_history_count;
        for (int i = 0; i < count; i++) {
            int8_t r = dev.rssi_history[i].rssi;
            if (r > -50) near++;
            else if (r >= -70) mid++;
            else far++;
        }
        out.features[4] = (float)near / (float)count;
        out.features[5] = (float)mid / (float)count;
        out.features[6] = (float)far / (float)count;
    } else {
        // Use current RSSI as single-sample estimate
        if (dev.rssi > -50) out.features[4] = 1.0f;
        else if (dev.rssi >= -70) out.features[5] = 1.0f;
        else out.features[6] = 1.0f;
    }

    // Features 7-10: Service UUID presence bits
    // These are detected from raw advertisement data during scanning.
    // The BLE scanner sets device_class based on parsed services.
    // We derive service hints from the device class and advertisement hash.

    // Check for common service indicators from device_class
    switch (dev.device_class) {
        case BleDeviceClass::WATCH:
        case BleDeviceClass::FITNESS:
            out.features[7] = 1.0f;  // Likely has heart rate
            out.features[8] = 1.0f;  // Likely has battery
            break;
        case BleDeviceClass::HEADPHONES:
        case BleDeviceClass::SPEAKER:
            out.features[10] = 1.0f; // Audio service
            out.features[8] = 1.0f;  // Battery
            break;
        case BleDeviceClass::PERIPHERAL:
            out.features[9] = 1.0f;  // HID
            out.features[8] = 1.0f;  // Battery
            break;
        case BleDeviceClass::MEDICAL:
            out.features[7] = 1.0f;  // Heart rate / health
            break;
        default:
            break;
    }

    // Feature 11: Device class as float
    out.features[11] = (float)(uint8_t)dev.device_class;

    // Feature 12: Random MAC flag
    out.features[12] = dev.is_random_mac ? 1.0f : 0.0f;

    // Feature 13: Advertisement payload length
    out.features[13] = (float)dev.raw_adv_len;

    return true;
}

int to_json(const BleFeatureVector& fv, char* buf, size_t buf_size) {
    if (buf_size < 64) return 0;

    int pos = 0;
    buf[pos++] = '{';

    for (int i = 0; i < FEATURE_COUNT; i++) {
        if (i > 0) {
            if (pos >= (int)buf_size - 40) break;
            buf[pos++] = ',';
        }
        int written;
        // Use integer format for whole numbers, float for fractional
        if (fv.features[i] == (float)(int)fv.features[i] && fv.features[i] < 1000.0f) {
            written = snprintf(buf + pos, buf_size - pos,
                "\"%s\":%d", FEATURE_NAMES[i], (int)fv.features[i]);
        } else {
            written = snprintf(buf + pos, buf_size - pos,
                "\"%s\":%.4f", FEATURE_NAMES[i], (double)fv.features[i]);
        }
        if (written <= 0) break;
        pos += written;
    }

    if (pos < (int)buf_size - 1) {
        buf[pos++] = '}';
    }
    buf[pos] = '\0';
    return pos;
}

int to_json_array(const BleDevice* devs, int count, char* buf, size_t buf_size) {
    if (buf_size < 32 || count <= 0) return 0;

    int pos = 0;
    buf[pos++] = '[';

    for (int i = 0; i < count && pos < (int)buf_size - 200; i++) {
        if (i > 0) buf[pos++] = ',';

        // MAC string
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 devs[i].addr[0], devs[i].addr[1], devs[i].addr[2],
                 devs[i].addr[3], devs[i].addr[4], devs[i].addr[5]);

        BleFeatureVector fv;
        extract(devs[i], fv);

        // Write entry: {"mac":"...","features":{...}}
        int written = snprintf(buf + pos, buf_size - pos,
            "{\"mac\":\"%s\",\"features\":", mac_str);
        if (written <= 0) break;
        pos += written;

        int fv_written = to_json(fv, buf + pos, buf_size - pos);
        if (fv_written <= 0) break;
        pos += fv_written;

        // Add cached classification if available
        const CachedClassification* cached = get_cached_classification(devs[i].addr);
        if (cached) {
            written = snprintf(buf + pos, buf_size - pos,
                ",\"sc_type\":\"%s\",\"sc_conf\":%.2f",
                cached->predicted_type, (double)cached->confidence);
            if (written > 0) pos += written;
        }

        if (pos < (int)buf_size - 1) buf[pos++] = '}';
    }

    if (pos < (int)buf_size - 1) buf[pos++] = ']';
    buf[pos] = '\0';
    return pos;
}

int get_version() {
    return FEATURE_VERSION;
}

void cache_feedback(const uint8_t addr[6], const char* predicted_type, float confidence) {
    // Look for existing entry
    for (int i = 0; i < FEEDBACK_CACHE_SIZE; i++) {
        if (_feedback_cache[i].valid &&
            memcmp(_feedback_cache[i].addr, addr, 6) == 0) {
            // Update existing
            strncpy(_feedback_cache[i].predicted_type, predicted_type, 15);
            _feedback_cache[i].predicted_type[15] = '\0';
            _feedback_cache[i].confidence = confidence;
#ifndef SIMULATOR
            _feedback_cache[i].received_ms = millis();
#else
            _feedback_cache[i].received_ms = 0;
#endif
            return;
        }
    }

    // Find empty slot or oldest entry
    int slot = -1;
    uint32_t oldest_ms = UINT32_MAX;
    for (int i = 0; i < FEEDBACK_CACHE_SIZE; i++) {
        if (!_feedback_cache[i].valid) {
            slot = i;
            break;
        }
        if (_feedback_cache[i].received_ms < oldest_ms) {
            oldest_ms = _feedback_cache[i].received_ms;
            slot = i;
        }
    }

    if (slot >= 0) {
        memcpy(_feedback_cache[slot].addr, addr, 6);
        strncpy(_feedback_cache[slot].predicted_type, predicted_type, 15);
        _feedback_cache[slot].predicted_type[15] = '\0';
        _feedback_cache[slot].confidence = confidence;
#ifndef SIMULATOR
        _feedback_cache[slot].received_ms = millis();
#else
        _feedback_cache[slot].received_ms = 0;
#endif
        _feedback_cache[slot].valid = true;
        if (_feedback_count < FEEDBACK_CACHE_SIZE) _feedback_count++;
    }
}

const CachedClassification* get_cached_classification(const uint8_t addr[6]) {
    for (int i = 0; i < FEEDBACK_CACHE_SIZE; i++) {
        if (_feedback_cache[i].valid &&
            memcmp(_feedback_cache[i].addr, addr, 6) == 0) {
            return &_feedback_cache[i];
        }
    }
    return nullptr;
}

int get_feedback_count() {
    return _feedback_count;
}

}  // namespace hal_ble_features
