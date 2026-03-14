// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#include "hal_identity.h"
#include "tritium_compat.h"

#ifndef SIMULATOR
#include <Preferences.h>
#include <esp_random.h>
#endif

namespace hal_identity {

static char _uuid[37] = {0};       // "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
static char _short_id[10] = {0};   // "xxxx-xxxx"
static char _device_name[64] = {0};
static bool _initialized = false;

// NVS namespace and keys
static constexpr const char* NVS_NS = "tritium_id";
static constexpr const char* NVS_KEY_UUID = "uuid";
static constexpr const char* NVS_KEY_NAME = "dev_name";

static const char HEX[] = "0123456789abcdef";

#ifndef SIMULATOR

// Generate a UUIDv4 string using hardware RNG
static void generate_uuid(char* out) {
    uint8_t bytes[16];
    // Use hardware RNG for true randomness
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        bytes[i]     = (r >> 0) & 0xFF;
        bytes[i + 1] = (r >> 8) & 0xFF;
        bytes[i + 2] = (r >> 16) & 0xFF;
        bytes[i + 3] = (r >> 24) & 0xFF;
    }

    // Set version 4 (random) bits: byte 6 = 0100xxxx
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    // Set variant 1 bits: byte 8 = 10xxxxxx
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    int pos = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[pos++] = '-';
        }
        out[pos++] = HEX[(bytes[i] >> 4) & 0x0F];
        out[pos++] = HEX[bytes[i] & 0x0F];
    }
    out[pos] = '\0';
}

bool init() {
    if (_initialized) return true;

    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) {
        Serial.printf("[identity] ERROR: failed to open NVS namespace '%s'\n", NVS_NS);
        return false;
    }

    // Try to load existing UUID
    size_t len = prefs.getString(NVS_KEY_UUID, _uuid, sizeof(_uuid));
    if (len == 0 || _uuid[0] == '\0') {
        // First boot: generate new UUID
        generate_uuid(_uuid);
        prefs.putString(NVS_KEY_UUID, _uuid);
        Serial.printf("[identity] Generated new device UUID: %s\n", _uuid);
    } else {
        Serial.printf("[identity] Loaded device UUID: %s\n", _uuid);
    }

    // Derive short ID from first and last 4 hex chars of UUID
    _short_id[0] = _uuid[0];
    _short_id[1] = _uuid[1];
    _short_id[2] = _uuid[2];
    _short_id[3] = _uuid[3];
    _short_id[4] = '-';
    _short_id[5] = _uuid[32];
    _short_id[6] = _uuid[33];
    _short_id[7] = _uuid[34];
    _short_id[8] = _uuid[35];
    _short_id[9] = '\0';

    // Load device name
    len = prefs.getString(NVS_KEY_NAME, _device_name, sizeof(_device_name));
    if (len == 0 || _device_name[0] == '\0') {
        snprintf(_device_name, sizeof(_device_name), "tritium-%s", _short_id);
    }

    prefs.end();
    _initialized = true;
    Serial.printf("[identity] Device name: %s | Short ID: %s\n", _device_name, _short_id);
    return true;
}

bool set_device_name(const char* name) {
    if (!name || name[0] == '\0') return false;
    snprintf(_device_name, sizeof(_device_name), "%s", name);

    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return false;
    prefs.putString(NVS_KEY_NAME, _device_name);
    prefs.end();
    Serial.printf("[identity] Device name set to: %s\n", _device_name);
    return true;
}

bool regenerate() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) return false;

    generate_uuid(_uuid);
    prefs.putString(NVS_KEY_UUID, _uuid);

    // Update short ID
    _short_id[0] = _uuid[0];
    _short_id[1] = _uuid[1];
    _short_id[2] = _uuid[2];
    _short_id[3] = _uuid[3];
    _short_id[4] = '-';
    _short_id[5] = _uuid[32];
    _short_id[6] = _uuid[33];
    _short_id[7] = _uuid[34];
    _short_id[8] = _uuid[35];
    _short_id[9] = '\0';

    // Update device name if it was auto-generated
    if (strncmp(_device_name, "tritium-", 8) == 0) {
        snprintf(_device_name, sizeof(_device_name), "tritium-%s", _short_id);
        prefs.putString(NVS_KEY_NAME, _device_name);
    }

    prefs.end();
    Serial.printf("[identity] Regenerated UUID: %s\n", _uuid);
    return true;
}

#else  // SIMULATOR

bool init() {
    if (_initialized) return true;
    // Simulator: generate a deterministic UUID
    snprintf(_uuid, sizeof(_uuid), "00000000-0000-4000-8000-000000000000");
    snprintf(_short_id, sizeof(_short_id), "0000-0000");
    snprintf(_device_name, sizeof(_device_name), "tritium-sim");
    _initialized = true;
    return true;
}

bool set_device_name(const char* name) {
    if (!name || name[0] == '\0') return false;
    snprintf(_device_name, sizeof(_device_name), "%s", name);
    return true;
}

bool regenerate() {
    snprintf(_uuid, sizeof(_uuid), "11111111-1111-4111-8111-111111111111");
    snprintf(_short_id, sizeof(_short_id), "1111-1111");
    return true;
}

#endif  // SIMULATOR

const char* get_uuid() {
    return _uuid;
}

const char* get_short_id() {
    return _short_id;
}

const char* get_device_name() {
    return _device_name;
}

bool is_initialized() {
    return _initialized;
}

int to_json_field(char* buf, size_t size) {
    if (!_initialized || size < 52) return 0;
    return snprintf(buf, size, "\"device_uuid\":\"%s\"", _uuid);
}

}  // namespace hal_identity
