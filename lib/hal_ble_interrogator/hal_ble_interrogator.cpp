// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_ble_interrogator.h"

#ifdef SIMULATOR

namespace hal_ble_interrogator {
bool init(const InterrogatorConfig&) { return false; }
void shutdown() {}
bool queue_interrogation(const uint8_t[6], uint8_t) { return false; }
bool queue_interrogation_by_mac(const char*, uint8_t) { return false; }
bool get_result(const uint8_t[6], BleDeviceProfile*) { return false; }
bool get_result_by_mac(const char*, BleDeviceProfile*) { return false; }
int get_completed_count() { return 0; }
int get_pending_count() { return 0; }
bool is_on_cooldown(const uint8_t[6]) { return false; }
int profile_to_json(const BleDeviceProfile*, char* buf, size_t buf_size) { return snprintf(buf, buf_size, "{}"); }
int get_status_json(char* buf, size_t buf_size) { return snprintf(buf, buf_size, "{\"active\":false}"); }
int get_all_profiles_json(char* buf, size_t buf_size) { return snprintf(buf, buf_size, "[]"); }
void clear_cache() {}
bool is_active() { return false; }
const char* service_uuid_name(uint16_t) { return "Unknown"; }
}

#else

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <cstring>
#include <cstdio>

#if __has_include("hal_diag.h")
#include "hal_diag.h"
#define INTERROG_LOG(sev, fmt, ...) hal_diag::log(sev, "ble_interrog", fmt, ##__VA_ARGS__)
#else
#define INTERROG_LOG(sev, fmt, ...) ((void)0)
#endif

namespace hal_ble_interrogator {

// --- Service UUID name lookup table ---
struct ServiceUuidEntry {
    uint16_t uuid;
    const char* name;
};

static const ServiceUuidEntry _svc_names[] = {
    {0x1800, "Generic Access"},
    {0x1801, "Generic Attribute"},
    {0x1802, "Immediate Alert"},
    {0x1803, "Link Loss"},
    {0x1804, "TX Power"},
    {0x1805, "Current Time"},
    {0x1808, "Glucose"},
    {0x1809, "Health Thermometer"},
    {0x180A, "Device Information"},
    {0x180D, "Heart Rate"},
    {0x180F, "Battery Service"},
    {0x1810, "Blood Pressure"},
    {0x1812, "HID"},
    {0x1813, "Scan Parameters"},
    {0x1814, "Running Speed"},
    {0x1816, "Cycling Speed"},
    {0x1818, "Cycling Power"},
    {0x1819, "Location and Navigation"},
    {0x181A, "Environmental Sensing"},
    {0x181C, "User Data"},
    {0x181D, "Weight Scale"},
    {0x181E, "Bond Management"},
    {0x1826, "Fitness Machine"},
    {0x1827, "Mesh Provisioning"},
    {0x1828, "Mesh Proxy"},
    {0, nullptr}  // sentinel
};

const char* service_uuid_name(uint16_t uuid16) {
    for (int i = 0; _svc_names[i].name != nullptr; i++) {
        if (_svc_names[i].uuid == uuid16) return _svc_names[i].name;
    }
    return "Unknown";
}

// --- Internal state ---

struct InterrogationRequest {
    uint8_t addr[6];
    uint8_t addr_type;
    bool valid;
};

// Cooldown tracking: MAC -> last interrogation time
struct CooldownEntry {
    uint8_t addr[6];
    uint32_t timestamp;
    bool valid;
};

static InterrogatorConfig _config;
static SemaphoreHandle_t _mutex = nullptr;
static TaskHandle_t _task_handle = nullptr;
static bool _running = false;

// Queue (circular buffer)
static InterrogationRequest _queue[BLE_INTERROGATOR_QUEUE_SIZE];
static int _queue_head = 0;
static int _queue_tail = 0;
static int _queue_count = 0;

// Results cache
static constexpr int MAX_RESULTS = 32;
static BleDeviceProfile _results[MAX_RESULTS];
static int _result_count = 0;
static int _completed_count = 0;

// Cooldown cache
static constexpr int MAX_COOLDOWNS = 64;
static CooldownEntry _cooldowns[MAX_COOLDOWNS];

// --- Helper: MAC comparison ---
static bool mac_equal(const uint8_t a[6], const uint8_t b[6]) {
    return memcmp(a, b, 6) == 0;
}

// --- Helper: parse MAC string ---
static bool parse_mac(const char* str, uint8_t out[6]) {
    unsigned int a[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
        for (int i = 0; i < 6; i++) out[i] = (uint8_t)a[i];
        return true;
    }
    return false;
}

// --- Helper: format MAC ---
static void format_mac(const uint8_t addr[6], char* buf, size_t size) {
    snprintf(buf, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

// --- Cooldown management ---
static bool check_cooldown(const uint8_t addr[6]) {
    uint32_t now = millis();
    for (int i = 0; i < MAX_COOLDOWNS; i++) {
        if (_cooldowns[i].valid && mac_equal(_cooldowns[i].addr, addr)) {
            if ((now - _cooldowns[i].timestamp) < _config.cooldown_ms) {
                return true;  // still on cooldown
            }
            _cooldowns[i].valid = false;  // cooldown expired
            return false;
        }
    }
    return false;
}

static void set_cooldown(const uint8_t addr[6]) {
    uint32_t now = millis();
    // Try to find existing entry
    for (int i = 0; i < MAX_COOLDOWNS; i++) {
        if (_cooldowns[i].valid && mac_equal(_cooldowns[i].addr, addr)) {
            _cooldowns[i].timestamp = now;
            return;
        }
    }
    // Find empty slot
    for (int i = 0; i < MAX_COOLDOWNS; i++) {
        if (!_cooldowns[i].valid) {
            memcpy(_cooldowns[i].addr, addr, 6);
            _cooldowns[i].timestamp = now;
            _cooldowns[i].valid = true;
            return;
        }
    }
    // Evict oldest
    int oldest = 0;
    uint32_t oldest_time = _cooldowns[0].timestamp;
    for (int i = 1; i < MAX_COOLDOWNS; i++) {
        if (_cooldowns[i].timestamp < oldest_time) {
            oldest = i;
            oldest_time = _cooldowns[i].timestamp;
        }
    }
    memcpy(_cooldowns[oldest].addr, addr, 6);
    _cooldowns[oldest].timestamp = now;
    _cooldowns[oldest].valid = true;
}

// --- Store result ---
static void store_result(const BleDeviceProfile& profile) {
    // Check if we already have a result for this MAC
    for (int i = 0; i < _result_count; i++) {
        if (mac_equal(_results[i].addr, profile.addr)) {
            _results[i] = profile;
            return;
        }
    }
    // Add new
    if (_result_count < MAX_RESULTS) {
        _results[_result_count++] = profile;
    } else {
        // Evict oldest (slot 0), shift down
        memmove(&_results[0], &_results[1], sizeof(BleDeviceProfile) * (MAX_RESULTS - 1));
        _results[MAX_RESULTS - 1] = profile;
    }
}

// --- Read a string characteristic ---
static bool read_char_string(NimBLERemoteService* svc, uint16_t char_uuid,
                             char* out, size_t out_size) {
    NimBLERemoteCharacteristic* ch = svc->getCharacteristic(NimBLEUUID(char_uuid));
    if (!ch) return false;
    if (!ch->canRead()) return false;

    std::string val = ch->readValue();
    if (val.empty()) return false;

    size_t copy_len = val.size() < out_size - 1 ? val.size() : out_size - 1;
    memcpy(out, val.c_str(), copy_len);
    out[copy_len] = '\0';
    return true;
}

// --- Perform single interrogation ---
static void do_interrogation(const InterrogationRequest& req) {
    BleDeviceProfile profile;
    memset(&profile, 0, sizeof(profile));
    memcpy(profile.addr, req.addr, 6);
    profile.addr_type = req.addr_type;
    profile.battery_level = -1;
    profile.success = false;

    uint32_t start_ms = millis();
    char mac_str[18];
    format_mac(req.addr, mac_str, sizeof(mac_str));

    Serial.printf("[ble_interrog] Interrogating %s...\n", mac_str);
    INTERROG_LOG(2, "Interrogating %s", mac_str);

    // Create NimBLE address
    NimBLEAddress addr(req.addr, req.addr_type);

    // Create client
    NimBLEClient* client = NimBLEDevice::createClient();
    if (!client) {
        snprintf(profile.error, sizeof(profile.error), "Failed to create BLE client");
        profile.connection_duration_ms = (uint16_t)(millis() - start_ms);
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100))) {
            store_result(profile);
            _completed_count++;
            xSemaphoreGive(_mutex);
        }
        return;
    }

    client->setConnectTimeout(_config.timeout_ms / 1000 + 1);

    // Attempt connection
    if (!client->connect(addr)) {
        snprintf(profile.error, sizeof(profile.error), "Connection failed");
        NimBLEDevice::deleteClient(client);
        profile.connection_duration_ms = (uint16_t)(millis() - start_ms);
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100))) {
            store_result(profile);
            _completed_count++;
            xSemaphoreGive(_mutex);
        }
        Serial.printf("[ble_interrog] Connection to %s failed\n", mac_str);
        return;
    }

    Serial.printf("[ble_interrog] Connected to %s, discovering services...\n", mac_str);

    // Discover services
    auto* svcs = client->getServices(true);
    if (svcs) {
        profile.service_count = 0;
        for (auto* svc : *svcs) {
            if (profile.service_count >= BLE_INTERROGATOR_MAX_SERVICES) break;

            BleGattService& gs = profile.services[profile.service_count];
            memset(&gs, 0, sizeof(gs));

            NimBLEUUID uuid = svc->getUUID();
            if (uuid.bitSize() == 16) {
                gs.uuid16 = uuid.getNative()->u16.value;
                gs.is_standard = true;
                const char* name = service_uuid_name(gs.uuid16);
                strncpy(gs.name, name, sizeof(gs.name) - 1);
            } else {
                gs.uuid16 = 0;
                gs.is_standard = false;
                strncpy(gs.name, uuid.toString().c_str(), sizeof(gs.name) - 1);
                // Copy 128-bit UUID
                const uint8_t* uuid_bytes = uuid.getNative()->u128.value;
                if (uuid_bytes) {
                    memcpy(gs.uuid128, uuid_bytes, 16);
                }
            }

            profile.service_count++;

            // Read Device Information Service
            if (gs.uuid16 == BLE_SVC_DEVICE_INFO) {
                read_char_string(svc, BLE_CHAR_MANUFACTURER_NAME,
                                 profile.manufacturer, sizeof(profile.manufacturer));
                read_char_string(svc, BLE_CHAR_MODEL_NUMBER,
                                 profile.model, sizeof(profile.model));
                read_char_string(svc, BLE_CHAR_FIRMWARE_REV,
                                 profile.firmware_rev, sizeof(profile.firmware_rev));
                read_char_string(svc, BLE_CHAR_HARDWARE_REV,
                                 profile.hardware_rev, sizeof(profile.hardware_rev));
                read_char_string(svc, BLE_CHAR_SOFTWARE_REV,
                                 profile.software_rev, sizeof(profile.software_rev));
                read_char_string(svc, BLE_CHAR_SERIAL_NUMBER,
                                 profile.serial_number, sizeof(profile.serial_number));
            }

            // Read GAP service
            if (gs.uuid16 == BLE_SVC_GAP) {
                read_char_string(svc, BLE_CHAR_DEVICE_NAME,
                                 profile.device_name, sizeof(profile.device_name));
                // Read appearance (uint16_t)
                NimBLERemoteCharacteristic* app_ch =
                    svc->getCharacteristic(NimBLEUUID(BLE_CHAR_APPEARANCE));
                if (app_ch && app_ch->canRead()) {
                    std::string val = app_ch->readValue();
                    if (val.size() >= 2) {
                        profile.appearance = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
                    }
                }
            }

            // Read Battery Service
            if (gs.uuid16 == BLE_SVC_BATTERY) {
                NimBLERemoteCharacteristic* bat_ch =
                    svc->getCharacteristic(NimBLEUUID(BLE_CHAR_BATTERY_LEVEL));
                if (bat_ch && bat_ch->canRead()) {
                    std::string val = bat_ch->readValue();
                    if (!val.empty()) {
                        profile.battery_level = (int8_t)(uint8_t)val[0];
                    }
                }
            }
        }
    }

    // Disconnect
    client->disconnect();
    NimBLEDevice::deleteClient(client);

    profile.success = true;
    profile.interrogated_at = millis();
    profile.connection_duration_ms = (uint16_t)(millis() - start_ms);

    Serial.printf("[ble_interrog] %s: %d services, mfr=%s model=%s bat=%d (%dms)\n",
                  mac_str, profile.service_count,
                  profile.manufacturer[0] ? profile.manufacturer : "n/a",
                  profile.model[0] ? profile.model : "n/a",
                  profile.battery_level,
                  profile.connection_duration_ms);

    // Store result and set cooldown
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100))) {
        store_result(profile);
        set_cooldown(req.addr);
        _completed_count++;
        xSemaphoreGive(_mutex);
    }
}

// --- Background task ---
static void interrogator_task(void* param) {
    while (_running) {
        InterrogationRequest req;
        bool have_request = false;

        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100))) {
            if (_queue_count > 0) {
                req = _queue[_queue_head];
                _queue[_queue_head].valid = false;
                _queue_head = (_queue_head + 1) % BLE_INTERROGATOR_QUEUE_SIZE;
                _queue_count--;
                have_request = true;
            }
            xSemaphoreGive(_mutex);
        }

        if (have_request && req.valid) {
            do_interrogation(req);
            // Brief pause between interrogations to let scanner breathe
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    _task_handle = nullptr;
    vTaskDelete(nullptr);
}

// --- Public API ---

bool init(const InterrogatorConfig& config) {
    if (_running) return true;

    _config = config;
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) return false;

    // Clear state
    memset(_queue, 0, sizeof(_queue));
    _queue_head = 0;
    _queue_tail = 0;
    _queue_count = 0;
    memset(_results, 0, sizeof(_results));
    _result_count = 0;
    _completed_count = 0;
    memset(_cooldowns, 0, sizeof(_cooldowns));

    _running = true;

    // Start background task — needs decent stack for NimBLE client operations
    BaseType_t ret = xTaskCreatePinnedToCore(
        interrogator_task,
        "ble_interrog",
        8192,       // Stack size
        nullptr,
        1,          // Priority (low — scanner is more important)
        &_task_handle,
        1           // Core 1 (keep scanner on core 0 if applicable)
    );

    if (ret != pdPASS) {
        _running = false;
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
        return false;
    }

    Serial.printf("[ble_interrog] Initialized (cooldown=%lus, timeout=%lums)\n",
                  _config.cooldown_ms / 1000, _config.timeout_ms);
    return true;
}

void shutdown() {
    _running = false;
    if (_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(2100));  // Wait for task to exit
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

bool queue_interrogation(const uint8_t addr[6], uint8_t addr_type) {
    if (!_running || !_mutex) return false;

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100))) {
        // Check cooldown
        if (check_cooldown(addr)) {
            xSemaphoreGive(_mutex);
            return false;
        }

        // Check if already in queue
        for (int i = 0; i < BLE_INTERROGATOR_QUEUE_SIZE; i++) {
            if (_queue[i].valid && mac_equal(_queue[i].addr, addr)) {
                xSemaphoreGive(_mutex);
                return false;  // already queued
            }
        }

        // Check queue full
        if (_queue_count >= _config.queue_size) {
            xSemaphoreGive(_mutex);
            return false;
        }

        // Enqueue
        InterrogationRequest& req = _queue[_queue_tail];
        memcpy(req.addr, addr, 6);
        req.addr_type = addr_type;
        req.valid = true;
        _queue_tail = (_queue_tail + 1) % BLE_INTERROGATOR_QUEUE_SIZE;
        _queue_count++;

        xSemaphoreGive(_mutex);

        char mac_str[18];
        format_mac(addr, mac_str, sizeof(mac_str));
        Serial.printf("[ble_interrog] Queued: %s\n", mac_str);
        return true;
    }
    return false;
}

bool queue_interrogation_by_mac(const char* mac_str, uint8_t addr_type) {
    uint8_t addr[6];
    if (!parse_mac(mac_str, addr)) return false;
    return queue_interrogation(addr, addr_type);
}

bool get_result(const uint8_t addr[6], BleDeviceProfile* out) {
    if (!_mutex || !out) return false;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100))) {
        for (int i = 0; i < _result_count; i++) {
            if (mac_equal(_results[i].addr, addr)) {
                *out = _results[i];
                xSemaphoreGive(_mutex);
                return true;
            }
        }
        xSemaphoreGive(_mutex);
    }
    return false;
}

bool get_result_by_mac(const char* mac_str, BleDeviceProfile* out) {
    uint8_t addr[6];
    if (!parse_mac(mac_str, addr)) return false;
    return get_result(addr, out);
}

int get_completed_count() {
    return _completed_count;
}

int get_pending_count() {
    return _queue_count;
}

bool is_on_cooldown(const uint8_t addr[6]) {
    if (!_mutex) return false;
    bool result = false;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50))) {
        result = check_cooldown(addr);
        xSemaphoreGive(_mutex);
    }
    return result;
}

int profile_to_json(const BleDeviceProfile* p, char* buf, size_t buf_size) {
    if (!p || !buf) return 0;

    char mac_str[18];
    format_mac(p->addr, mac_str, sizeof(mac_str));

    int pos = snprintf(buf, buf_size,
        "{\"mac\":\"%s\",\"success\":%s,\"duration_ms\":%d",
        mac_str,
        p->success ? "true" : "false",
        p->connection_duration_ms);

    if (!p->success) {
        pos += snprintf(buf + pos, buf_size - pos,
            ",\"error\":\"%s\"}", p->error);
        return pos;
    }

    // Services array
    pos += snprintf(buf + pos, buf_size - pos, ",\"services\":[");
    for (int i = 0; i < p->service_count && pos < (int)buf_size - 100; i++) {
        if (i > 0) buf[pos++] = ',';
        if (p->services[i].is_standard) {
            pos += snprintf(buf + pos, buf_size - pos,
                "{\"uuid\":\"0x%04X\",\"name\":\"%s\"}",
                p->services[i].uuid16, p->services[i].name);
        } else {
            pos += snprintf(buf + pos, buf_size - pos,
                "{\"uuid\":\"%s\",\"name\":\"%s\"}",
                p->services[i].name, p->services[i].name);
        }
    }
    pos += snprintf(buf + pos, buf_size - pos, "]");

    // Device info fields
    if (p->manufacturer[0])
        pos += snprintf(buf + pos, buf_size - pos, ",\"manufacturer\":\"%s\"", p->manufacturer);
    if (p->model[0])
        pos += snprintf(buf + pos, buf_size - pos, ",\"model\":\"%s\"", p->model);
    if (p->firmware_rev[0])
        pos += snprintf(buf + pos, buf_size - pos, ",\"firmware_rev\":\"%s\"", p->firmware_rev);
    if (p->hardware_rev[0])
        pos += snprintf(buf + pos, buf_size - pos, ",\"hardware_rev\":\"%s\"", p->hardware_rev);
    if (p->software_rev[0])
        pos += snprintf(buf + pos, buf_size - pos, ",\"software_rev\":\"%s\"", p->software_rev);
    if (p->serial_number[0])
        pos += snprintf(buf + pos, buf_size - pos, ",\"serial_number\":\"%s\"", p->serial_number);
    if (p->device_name[0])
        pos += snprintf(buf + pos, buf_size - pos, ",\"device_name\":\"%s\"", p->device_name);
    if (p->appearance)
        pos += snprintf(buf + pos, buf_size - pos, ",\"appearance\":%d", p->appearance);
    if (p->battery_level >= 0)
        pos += snprintf(buf + pos, buf_size - pos, ",\"battery_level\":%d", p->battery_level);

    pos += snprintf(buf + pos, buf_size - pos, ",\"service_count\":%d}", p->service_count);
    return pos;
}

int get_status_json(char* buf, size_t buf_size) {
    return snprintf(buf, buf_size,
        "{\"active\":%s,\"pending\":%d,\"completed\":%d,\"cached\":%d}",
        _running ? "true" : "false",
        _queue_count, _completed_count, _result_count);
}

int get_all_profiles_json(char* buf, size_t buf_size) {
    if (!_mutex) return snprintf(buf, buf_size, "[]");

    int pos = 0;
    buf[pos++] = '[';

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100))) {
        for (int i = 0; i < _result_count && pos < (int)buf_size - 200; i++) {
            if (i > 0) buf[pos++] = ',';
            pos += profile_to_json(&_results[i], buf + pos, buf_size - pos);
        }
        xSemaphoreGive(_mutex);
    }

    pos += snprintf(buf + pos, buf_size - pos, "]");
    return pos;
}

void clear_cache() {
    if (!_mutex) return;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100))) {
        memset(_results, 0, sizeof(_results));
        _result_count = 0;
        memset(_cooldowns, 0, sizeof(_cooldowns));
        xSemaphoreGive(_mutex);
    }
}

bool is_active() {
    return _running;
}

}  // namespace hal_ble_interrogator

#endif  // SIMULATOR
