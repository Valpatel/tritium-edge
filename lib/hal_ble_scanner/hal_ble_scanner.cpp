// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_ble_scanner.h"

#ifdef SIMULATOR

namespace hal_ble_scanner {
bool init(const ScanConfig&) { return false; }
void shutdown() {}
void add_known_device(const uint8_t[6], const char*) {}
int get_devices(BleDevice*, int) { return 0; }
int get_visible_count() { return 0; }
int get_known_visible_count() { return 0; }
bool is_device_visible(const uint8_t[6]) { return false; }
int get_summary_json(char* buf, size_t buf_size) { return snprintf(buf, buf_size, "{}"); }
int get_devices_json(char* buf, size_t buf_size) { return snprintf(buf, buf_size, "[]"); }
int get_devices_json_batch(char* buf, size_t buf_size, int, int) { return snprintf(buf, buf_size, "[]"); }
int get_batch_count(int) { return 0; }
bool is_cache_valid() { return false; }
uint32_t cache_age_ms() { return 0; }
int get_rssi_history_json(const uint8_t[6], char* buf, size_t buf_size) { return 0; }
int get_rssi_history_json_by_mac(const char*, char* buf, size_t buf_size) { return 0; }
int get_rotation_group_count() { return 0; }
bool is_active() { return false; }
}

#else

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <cstring>
#include <cstdio>

#if __has_include("hal_diag.h")
#include "hal_diag.h"
#define BLE_DIAG_LOG(sev, fmt, ...) hal_diag::log(sev, "ble", fmt, ##__VA_ARGS__)
#else
#define BLE_DIAG_LOG(sev, fmt, ...) ((void)0)
#endif

namespace hal_ble_scanner {

// --- Internal state ---
static BleDevice _devices[BLE_SCANNER_MAX_DEVICES];
static int _device_count = 0;
static SemaphoreHandle_t _mutex = nullptr;

static BleDeviceMatch _known[16];
static int _known_count = 0;

static bool _running = false;
static ScanConfig _config;
static TaskHandle_t _scan_task = nullptr;
static uint32_t _last_scan_complete_ms = 0;  // millis() when last scan cycle finished
static int _next_rotation_group = 0;          // Next group ID to assign

// Track recently departed random-MAC devices for rotation correlation.
// When a random-MAC device disappears (pruned) and a new random-MAC device
// appears within MAC_ROTATION_WINDOW_MS at similar RSSI, we link them.
struct DepartedRandomMac {
    uint8_t addr[6];
    int8_t last_rssi;
    uint32_t departed_at;       // millis() when pruned
    int8_t rotation_group;      // group ID (or -1 if ungrouped)
    BleDeviceClass device_class;
    AppleDeviceType apple_type;
    bool valid;
};
static DepartedRandomMac _departed[MAC_ROTATION_MAX_GROUPS];
static int _departed_count = 0;

// --- Helpers ---

static int find_device(const uint8_t addr[6]) {
    for (int i = 0; i < _device_count; i++) {
        if (memcmp(_devices[i].addr, addr, 6) == 0) return i;
    }
    return -1;
}

static bool is_known_device(const uint8_t addr[6], const char** label) {
    for (int i = 0; i < _known_count; i++) {
        if (memcmp(_known[i].addr, addr, 6) == 0) {
            *label = _known[i].label;
            return true;
        }
    }
    return false;
}

static void record_rssi(BleDevice& d, int8_t rssi) {
    uint32_t now = millis();
    d.rssi_history[d.rssi_history_head] = {rssi, now};
    d.rssi_history_head = (d.rssi_history_head + 1) % BLE_RSSI_HISTORY_SIZE;
    if (d.rssi_history_count < BLE_RSSI_HISTORY_SIZE) {
        d.rssi_history_count++;
    }
    if (rssi < d.rssi_min) d.rssi_min = rssi;
    if (rssi > d.rssi_max) d.rssi_max = rssi;
}

// Compute RSSI trend: compare average of first half vs second half of history.
// Returns "approaching" if signal is strengthening, "departing" if weakening,
// or "stable" if change is within noise threshold.
static const char* compute_rssi_trend(const BleDevice& d) {
    if (d.rssi_history_count < 4) return "stable";

    int half = d.rssi_history_count / 2;
    int oldest_start = (d.rssi_history_head - d.rssi_history_count + BLE_RSSI_HISTORY_SIZE) % BLE_RSSI_HISTORY_SIZE;
    long sum_old = 0, sum_new = 0;

    for (int i = 0; i < half; i++) {
        int idx = (oldest_start + i) % BLE_RSSI_HISTORY_SIZE;
        sum_old += d.rssi_history[idx].rssi;
    }
    for (int i = half; i < d.rssi_history_count; i++) {
        int idx = (oldest_start + i) % BLE_RSSI_HISTORY_SIZE;
        sum_new += d.rssi_history[idx].rssi;
    }

    int new_count = d.rssi_history_count - half;
    float avg_old = (float)sum_old / half;
    float avg_new = (float)sum_new / new_count;
    float delta = avg_new - avg_old;

    // 3 dBm threshold to avoid noise
    if (delta > 3.0f) return "approaching";
    if (delta < -3.0f) return "departing";
    return "stable";
}

static void prune_stale() {
    uint32_t now = millis();

    // Expire old departed entries
    for (int i = 0; i < _departed_count; ) {
        if ((now - _departed[i].departed_at) > MAC_ROTATION_WINDOW_MS * 3) {
            _departed[i] = _departed[--_departed_count];
        } else {
            i++;
        }
    }

    int write = 0;
    for (int i = 0; i < _device_count; i++) {
        if ((now - _devices[i].last_seen) < BLE_DEVICE_TIMEOUT_MS) {
            if (write != i) _devices[write] = _devices[i];
            write++;
        } else {
            // Track departed random-MAC devices for rotation correlation
            if (_devices[i].is_random_mac && _departed_count < MAC_ROTATION_MAX_GROUPS) {
                DepartedRandomMac& dep = _departed[_departed_count++];
                memcpy(dep.addr, _devices[i].addr, 6);
                dep.last_rssi = _devices[i].rssi;
                dep.departed_at = now;
                dep.rotation_group = _devices[i].rotation_group;
                dep.device_class = _devices[i].device_class;
                dep.apple_type = _devices[i].apple_type;
                dep.valid = true;
            }
            if (_devices[i].is_known) {
                BLE_DIAG_LOG(hal_diag::Severity::INFO,
                    "Known BLE device departed: %s (seen %u times)",
                    _devices[i].name, _devices[i].seen_count);
            }
        }
    }
    _device_count = write;
}

// Try to correlate a new random-MAC device with a recently departed one.
// Returns the rotation group ID if correlated, or -1 if no match.
static int8_t try_correlate_rotation(const BleDevice& new_dev, uint32_t now) {
    if (!new_dev.is_random_mac) return -1;

    for (int i = 0; i < _departed_count; i++) {
        if (!_departed[i].valid) continue;
        uint32_t elapsed = now - _departed[i].departed_at;
        if (elapsed > MAC_ROTATION_WINDOW_MS) continue;

        // RSSI similarity check
        int8_t rssi_diff = new_dev.rssi - _departed[i].last_rssi;
        if (rssi_diff < 0) rssi_diff = -rssi_diff;
        if (rssi_diff > MAC_ROTATION_RSSI_TOLERANCE) continue;

        // Device class match (if both classified)
        if (new_dev.device_class != BleDeviceClass::UNKNOWN &&
            _departed[i].device_class != BleDeviceClass::UNKNOWN &&
            new_dev.device_class != _departed[i].device_class) continue;

        // Apple type match (if both typed)
        if (new_dev.apple_type != AppleDeviceType::NONE &&
            _departed[i].apple_type != AppleDeviceType::NONE &&
            new_dev.apple_type != _departed[i].apple_type) continue;

        // Correlated: assign or reuse group
        int8_t group = _departed[i].rotation_group;
        if (group < 0) {
            group = _next_rotation_group++;
            if (_next_rotation_group > 127) _next_rotation_group = 0;
        }
        _departed[i].valid = false;  // consumed

        Serial.printf("[ble_scan] MAC rotation detected: group %d, RSSI diff=%d, elapsed=%lums\n",
            group, rssi_diff, (unsigned long)elapsed);
        BLE_DIAG_LOG(hal_diag::Severity::INFO,
            "MAC rotation group %d: new device correlated with departed (dt=%lums)",
            group, (unsigned long)elapsed);
        return group;
    }
    return -1;
}

// --- Apple Continuity protocol parsing ---
// Apple uses company ID 0x004C in manufacturer-specific BLE advertisement data.
// The Continuity protocol encodes device type in the data payload.

static const char* apple_type_string(AppleDeviceType t) {
    switch (t) {
        case AppleDeviceType::IPHONE:       return "iPhone";
        case AppleDeviceType::IPAD:         return "iPad";
        case AppleDeviceType::WATCH:        return "Watch";
        case AppleDeviceType::MACBOOK:      return "MacBook";
        case AppleDeviceType::AIRPODS:      return "AirPods";
        case AppleDeviceType::HOMEPOD:      return "HomePod";
        case AppleDeviceType::APPLE_TV:     return "AppleTV";
        case AppleDeviceType::PENCIL:       return "Pencil";
        case AppleDeviceType::AIRTAG:       return "AirTag";
        case AppleDeviceType::UNKNOWN_APPLE: return "Apple";
        default:                            return "";
    }
}

static BleDeviceClass apple_type_to_class(AppleDeviceType t) {
    switch (t) {
        case AppleDeviceType::IPHONE:       return BleDeviceClass::PHONE;
        case AppleDeviceType::IPAD:         return BleDeviceClass::TABLET;
        case AppleDeviceType::WATCH:        return BleDeviceClass::WATCH;
        case AppleDeviceType::MACBOOK:      return BleDeviceClass::LAPTOP;
        case AppleDeviceType::AIRPODS:      return BleDeviceClass::HEADPHONES;
        case AppleDeviceType::HOMEPOD:      return BleDeviceClass::SPEAKER;
        case AppleDeviceType::APPLE_TV:     return BleDeviceClass::TV_DONGLE;
        case AppleDeviceType::PENCIL:       return BleDeviceClass::PERIPHERAL;
        case AppleDeviceType::AIRTAG:       return BleDeviceClass::TRACKER;
        default:                            return BleDeviceClass::UNKNOWN;
    }
}

static const char* device_class_string(BleDeviceClass c) {
    switch (c) {
        case BleDeviceClass::PHONE:         return "phone";
        case BleDeviceClass::WATCH:         return "watch";
        case BleDeviceClass::TABLET:        return "tablet";
        case BleDeviceClass::LAPTOP:        return "laptop";
        case BleDeviceClass::HEADPHONES:    return "headphones";
        case BleDeviceClass::SPEAKER:       return "speaker";
        case BleDeviceClass::TV_DONGLE:     return "tv_dongle";
        case BleDeviceClass::TRACKER:       return "tracker";
        case BleDeviceClass::IOT_DEVICE:    return "iot";
        case BleDeviceClass::BEACON:        return "beacon";
        case BleDeviceClass::MEDICAL:       return "medical";
        case BleDeviceClass::FITNESS:       return "fitness";
        case BleDeviceClass::PERIPHERAL:    return "peripheral";
        default:                            return "unknown";
    }
}

// Parse Apple manufacturer-specific data from BLE advertisement.
// Apple company ID = 0x004C (little-endian in BLE: 0x4C, 0x00).
// Continuity message format: [type_byte] [length] [payload...]
// The type byte indicates the device/message type.
static void parse_apple_continuity(const NimBLEAdvertisedDevice* dev, BleDevice& d) {
    // getManufacturerData() returns the raw manufacturer data after the company ID
    // In NimBLE v2, company ID 0x004C is Apple
    std::string mfr = dev->getManufacturerData();
    if (mfr.size() < 4) return;  // Need at least company ID (2) + type (1) + len (1)

    // First two bytes are company ID (little-endian)
    uint16_t company_id = (uint8_t)mfr[0] | ((uint8_t)mfr[1] << 8);
    if (company_id != 0x004C) return;  // Not Apple

    // Parse Continuity message types starting at byte 2
    // Multiple TLV records can follow: [type][length][data...]
    size_t offset = 2;
    while (offset + 1 < mfr.size()) {
        uint8_t msg_type = (uint8_t)mfr[offset];
        uint8_t msg_len = (uint8_t)mfr[offset + 1];

        // Map known Continuity message types to Apple device types
        // 0x01 = Nearby Action (various devices)
        // 0x02 = iBeacon
        // 0x05 = AirDrop
        // 0x07 = AirPods (Proximity Pairing)
        // 0x09 = AirPlay target
        // 0x0C = Handoff
        // 0x0F = Nearby Info (contains device type in status byte)
        // 0x10 = Nearby Action
        // 0x12 = Find My (AirTag / Find My network)

        switch (msg_type) {
            case 0x07:  // Proximity Pairing — AirPods/Beats
                d.apple_type = AppleDeviceType::AIRPODS;
                break;
            case 0x09:  // AirPlay target — Apple TV / HomePod
                d.apple_type = AppleDeviceType::APPLE_TV;
                break;
            case 0x0F:  // Nearby Info — contains device type in upper nibble of status
                if (msg_len >= 1 && offset + 2 < mfr.size()) {
                    uint8_t status_byte = (uint8_t)mfr[offset + 2];
                    uint8_t dev_type = (status_byte >> 4) & 0x0F;
                    // Device type nibble mapping:
                    // 1=iPhone, 2=iPad, 3=MacBook, 4=Watch, 5=AirPods, 6=AppleTV, 7=HomePod
                    switch (dev_type) {
                        case 1: d.apple_type = AppleDeviceType::IPHONE; break;
                        case 2: d.apple_type = AppleDeviceType::IPAD; break;
                        case 3: d.apple_type = AppleDeviceType::MACBOOK; break;
                        case 4: d.apple_type = AppleDeviceType::WATCH; break;
                        case 5: d.apple_type = AppleDeviceType::AIRPODS; break;
                        case 6: d.apple_type = AppleDeviceType::APPLE_TV; break;
                        case 7: d.apple_type = AppleDeviceType::HOMEPOD; break;
                        default: d.apple_type = AppleDeviceType::UNKNOWN_APPLE; break;
                    }
                }
                break;
            case 0x12:  // Find My network — AirTag
                d.apple_type = AppleDeviceType::AIRTAG;
                break;
            case 0x0C:  // Handoff — Mac/iPhone/iPad
                if (d.apple_type == AppleDeviceType::NONE) {
                    d.apple_type = AppleDeviceType::UNKNOWN_APPLE;
                }
                break;
            default:
                if (d.apple_type == AppleDeviceType::NONE) {
                    d.apple_type = AppleDeviceType::UNKNOWN_APPLE;
                }
                break;
        }

        offset += 2 + msg_len;
    }

    // Set device class and type string from Apple type
    if (d.apple_type != AppleDeviceType::NONE) {
        d.device_class = apple_type_to_class(d.apple_type);
        const char* type_str = apple_type_string(d.apple_type);
        strncpy(d.device_type, type_str, sizeof(d.device_type) - 1);
        d.device_type[sizeof(d.device_type) - 1] = '\0';
    }
}

// --- NimBLE scan callback ---

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!_mutex) return;
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

        NimBLEAddress addr = dev->getAddress();
        const uint8_t* raw = addr.getVal();
        uint32_t now = millis();

        int idx = find_device(raw);
        if (idx >= 0) {
            int8_t new_rssi = dev->getRSSI();
            _devices[idx].rssi = new_rssi;
            _devices[idx].last_seen = now;
            _devices[idx].seen_count++;
            record_rssi(_devices[idx], new_rssi);
            // Update Apple classification if not yet classified
            if (_devices[idx].apple_type == AppleDeviceType::NONE &&
                dev->haveManufacturerData()) {
                parse_apple_continuity(dev, _devices[idx]);
            }
        } else if (_device_count < BLE_SCANNER_MAX_DEVICES) {
            BleDevice& d = _devices[_device_count];
            memcpy(d.addr, raw, 6);
            d.rssi = dev->getRSSI();
            d.addr_type = addr.getType();
            d.first_seen = now;
            d.last_seen = now;
            d.seen_count = 1;
            d.name[0] = '\0';
            d.apple_type = AppleDeviceType::NONE;
            d.device_class = BleDeviceClass::UNKNOWN;
            d.device_type[0] = '\0';
            d.is_random_mac = is_locally_administered(raw);
            d.rotation_group = -1;
            d.rssi_history_head = 0;
            d.rssi_history_count = 0;
            d.rssi_min = d.rssi;
            d.rssi_max = d.rssi;
            record_rssi(d, d.rssi);

            if (dev->haveName()) {
                strncpy(d.name, dev->getName().c_str(), sizeof(d.name) - 1);
                d.name[sizeof(d.name) - 1] = '\0';
            }

            // Parse Apple Continuity protocol
            if (dev->haveManufacturerData()) {
                parse_apple_continuity(dev, d);
            }

            const char* label = nullptr;
            d.is_known = is_known_device(raw, &label);
            if (d.is_known && label) {
                strncpy(d.name, label, sizeof(d.name) - 1);
            }

            // MAC randomization: try to correlate with recently departed device
            if (d.is_random_mac) {
                d.rotation_group = try_correlate_rotation(d, now);
            }

            _device_count++;

            if (d.is_known) {
                Serial.printf("[ble_scan] Known device: %s (%02X:%02X:%02X:%02X:%02X:%02X) RSSI=%d\n",
                    d.name, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], d.rssi);
                BLE_DIAG_LOG(hal_diag::Severity::INFO,
                    "Known BLE device arrived: %s RSSI=%d", d.name, d.rssi);
            } else if (d.apple_type != AppleDeviceType::NONE) {
                Serial.printf("[ble_scan] Apple %s (%02X:%02X:%02X:%02X:%02X:%02X) RSSI=%d\n",
                    d.device_type, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], d.rssi);
            }
        }

        xSemaphoreGive(_mutex);
    }
};

static ScanCallbacks _scan_callbacks;

// --- Background scan task ---

static void scan_task(void* param) {
    while (_running) {
        NimBLEScan* scan = NimBLEDevice::getScan();
        scan->setActiveScan(_config.active_scan);
        scan->setInterval(_config.scan_interval_ms);
        scan->setWindow(_config.scan_window_ms);

        Serial.printf("[ble_scan] Scanning for %lus...\n", (unsigned long)_config.scan_duration_s);
        scan->start(_config.scan_duration_s, false);

        // Prune stale devices and mark cache as fresh
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            prune_stale();
            _last_scan_complete_ms = millis();
            xSemaphoreGive(_mutex);
        }

        Serial.printf("[ble_scan] %d devices visible (%d known)\n",
            get_visible_count(), get_known_visible_count());

        vTaskDelay(pdMS_TO_TICKS(_config.pause_between_ms));
    }

    vTaskDelete(nullptr);
}

// --- Public API ---

bool init(const ScanConfig& config) {
    if (_running) return true;

    _config = config;
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) return false;

    _device_count = 0;

    NimBLEDevice::init("Tritium-Node");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&_scan_callbacks, false);

    _running = true;
    xTaskCreatePinnedToCore(scan_task, "ble_scan", 4096, nullptr, 1, &_scan_task, 1);

    Serial.printf("[ble_scan] Started (interval=%dms, window=%dms, passive=%s)\n",
        config.scan_interval_ms, config.scan_window_ms,
        config.active_scan ? "no" : "yes");
    return true;
}

void shutdown() {
    _running = false;
    if (_scan_task) {
        vTaskDelay(pdMS_TO_TICKS(500));
        _scan_task = nullptr;
    }
    NimBLEDevice::getScan()->stop();
    NimBLEDevice::deinit(true);
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

void add_known_device(const uint8_t addr[6], const char* label) {
    if (_known_count >= 16) return;
    memcpy(_known[_known_count].addr, addr, 6);
    strncpy(_known[_known_count].label, label, sizeof(_known[0].label) - 1);
    _known[_known_count].label[sizeof(_known[0].label) - 1] = '\0';
    _known_count++;
}

int get_devices(BleDevice* out, int max_count) {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int count = (_device_count < max_count) ? _device_count : max_count;
    memcpy(out, _devices, count * sizeof(BleDevice));
    xSemaphoreGive(_mutex);
    return count;
}

int get_visible_count() {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t now = millis();
    int count = 0;
    for (int i = 0; i < _device_count; i++) {
        if ((now - _devices[i].last_seen) < BLE_DEVICE_TIMEOUT_MS) count++;
    }
    xSemaphoreGive(_mutex);
    return count;
}

int get_known_visible_count() {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t now = millis();
    int count = 0;
    for (int i = 0; i < _device_count; i++) {
        if (_devices[i].is_known && (now - _devices[i].last_seen) < BLE_DEVICE_TIMEOUT_MS) count++;
    }
    xSemaphoreGive(_mutex);
    return count;
}

bool is_device_visible(const uint8_t addr[6]) {
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int idx = find_device(addr);
    bool visible = (idx >= 0) && ((millis() - _devices[idx].last_seen) < BLE_DEVICE_TIMEOUT_MS);
    xSemaphoreGive(_mutex);
    return visible;
}

int get_summary_json(char* buf, size_t buf_size) {
    if (!_mutex) return snprintf(buf, buf_size, "{}");
    xSemaphoreTake(_mutex, portMAX_DELAY);

    uint32_t now = millis();
    int visible = 0, known_visible = 0;
    for (int i = 0; i < _device_count; i++) {
        if ((now - _devices[i].last_seen) < BLE_DEVICE_TIMEOUT_MS) {
            visible++;
            if (_devices[i].is_known) known_visible++;
        }
    }

    int written = snprintf(buf, buf_size,
        "{\"ble_devices\":%d,\"ble_known\":%d,\"ble_total_seen\":%d}",
        visible, known_visible, _device_count);

    xSemaphoreGive(_mutex);
    return written;
}

int get_devices_json(char* buf, size_t buf_size) {
    if (!_mutex) return snprintf(buf, buf_size, "[]");
    xSemaphoreTake(_mutex, portMAX_DELAY);

    uint32_t now = millis();
    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "[");

    bool first = true;
    for (int i = 0; i < _device_count && pos < (int)buf_size - 120; i++) {
        if ((now - _devices[i].last_seen) >= BLE_DEVICE_TIMEOUT_MS) continue;
        if (!first) pos += snprintf(buf + pos, buf_size - pos, ",");
        first = false;
        pos += snprintf(buf + pos, buf_size - pos,
            "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d,\"seen\":%u",
            _devices[i].addr[0], _devices[i].addr[1], _devices[i].addr[2],
            _devices[i].addr[3], _devices[i].addr[4], _devices[i].addr[5],
            _devices[i].rssi, (unsigned)_devices[i].seen_count);
        if (_devices[i].name[0] && pos < (int)buf_size - 60) {
            pos += snprintf(buf + pos, buf_size - pos,
                ",\"name\":\"%s\"", _devices[i].name);
        }
        if (_devices[i].device_type[0] && pos < (int)buf_size - 40) {
            pos += snprintf(buf + pos, buf_size - pos,
                ",\"device_type\":\"%s\"", _devices[i].device_type);
        }
        if (_devices[i].device_class != BleDeviceClass::UNKNOWN && pos < (int)buf_size - 40) {
            pos += snprintf(buf + pos, buf_size - pos,
                ",\"class\":\"%s\"", device_class_string(_devices[i].device_class));
        }
        if (_devices[i].is_known && pos < (int)buf_size - 20) {
            pos += snprintf(buf + pos, buf_size - pos, ",\"known\":true");
        }
        if (_devices[i].is_random_mac && pos < (int)buf_size - 30) {
            pos += snprintf(buf + pos, buf_size - pos, ",\"random_mac\":true");
        }
        if (_devices[i].rotation_group >= 0 && pos < (int)buf_size - 30) {
            pos += snprintf(buf + pos, buf_size - pos, ",\"rotation_group\":%d",
                _devices[i].rotation_group);
        }
        pos += snprintf(buf + pos, buf_size - pos, "}");
    }

    pos += snprintf(buf + pos, buf_size - pos, "]");
    xSemaphoreGive(_mutex);
    return pos;
}

int get_devices_json_batch(char* buf, size_t buf_size, int offset, int batch_size) {
    if (!_mutex) return snprintf(buf, buf_size, "[]");
    int bs = (batch_size > 0) ? batch_size : _config.batch_size;
    if (bs <= 0) bs = 10;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    uint32_t now = millis();
    int pos = 0;
    pos += snprintf(buf + pos, buf_size - pos, "[");

    bool first = true;
    int visible_idx = 0;
    int written = 0;

    for (int i = 0; i < _device_count && pos < (int)buf_size - 120; i++) {
        if ((now - _devices[i].last_seen) >= BLE_DEVICE_TIMEOUT_MS) continue;

        // Skip devices before offset
        if (visible_idx < offset) {
            visible_idx++;
            continue;
        }

        // Stop after batch_size devices
        if (written >= bs) break;

        if (!first) pos += snprintf(buf + pos, buf_size - pos, ",");
        first = false;
        pos += snprintf(buf + pos, buf_size - pos,
            "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d,\"seen\":%u",
            _devices[i].addr[0], _devices[i].addr[1], _devices[i].addr[2],
            _devices[i].addr[3], _devices[i].addr[4], _devices[i].addr[5],
            _devices[i].rssi, (unsigned)_devices[i].seen_count);
        if (_devices[i].name[0] && pos < (int)buf_size - 60) {
            pos += snprintf(buf + pos, buf_size - pos,
                ",\"name\":\"%s\"", _devices[i].name);
        }
        if (_devices[i].device_type[0] && pos < (int)buf_size - 40) {
            pos += snprintf(buf + pos, buf_size - pos,
                ",\"device_type\":\"%s\"", _devices[i].device_type);
        }
        if (_devices[i].device_class != BleDeviceClass::UNKNOWN && pos < (int)buf_size - 40) {
            pos += snprintf(buf + pos, buf_size - pos,
                ",\"class\":\"%s\"", device_class_string(_devices[i].device_class));
        }
        if (_devices[i].is_known && pos < (int)buf_size - 20) {
            pos += snprintf(buf + pos, buf_size - pos, ",\"known\":true");
        }
        pos += snprintf(buf + pos, buf_size - pos, "}");

        visible_idx++;
        written++;
    }

    pos += snprintf(buf + pos, buf_size - pos, "]");
    xSemaphoreGive(_mutex);
    return pos;
}

int get_batch_count(int batch_size) {
    int bs = (batch_size > 0) ? batch_size : _config.batch_size;
    if (bs <= 0) bs = 10;
    int visible = get_visible_count();
    return (visible + bs - 1) / bs;  // ceiling division
}

bool is_cache_valid() {
    if (!_running || _last_scan_complete_ms == 0) return false;
    uint32_t age = millis() - _last_scan_complete_ms;
    return age < _config.cache_ttl_ms;
}

uint32_t cache_age_ms() {
    if (_last_scan_complete_ms == 0) return 0;
    return millis() - _last_scan_complete_ms;
}

int get_rssi_history_json(const uint8_t addr[6], char* buf, size_t buf_size) {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    int idx = find_device(addr);
    if (idx < 0) {
        xSemaphoreGive(_mutex);
        return 0;
    }

    const BleDevice& d = _devices[idx];
    uint32_t now = millis();
    int pos = 0;

    pos += snprintf(buf + pos, buf_size - pos,
        "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"count\":%d,\"min\":%d,\"max\":%d,",
        d.addr[0], d.addr[1], d.addr[2],
        d.addr[3], d.addr[4], d.addr[5],
        d.rssi_history_count, d.rssi_min, d.rssi_max);

    pos += snprintf(buf + pos, buf_size - pos, "\"trend\":\"%s\",", compute_rssi_trend(d));
    pos += snprintf(buf + pos, buf_size - pos, "\"readings\":[");

    // Output readings from oldest to newest
    int start = (d.rssi_history_head - d.rssi_history_count + BLE_RSSI_HISTORY_SIZE)
                % BLE_RSSI_HISTORY_SIZE;
    for (int i = 0; i < d.rssi_history_count && pos < (int)buf_size - 40; i++) {
        int ri = (start + i) % BLE_RSSI_HISTORY_SIZE;
        if (i > 0) buf[pos++] = ',';
        uint32_t age = now - d.rssi_history[ri].timestamp;
        pos += snprintf(buf + pos, buf_size - pos,
            "{\"rssi\":%d,\"age_ms\":%u}",
            d.rssi_history[ri].rssi, (unsigned)age);
    }

    pos += snprintf(buf + pos, buf_size - pos, "]}");
    xSemaphoreGive(_mutex);
    return pos;
}

int get_rssi_history_json_by_mac(const char* mac_str, char* buf, size_t buf_size) {
    unsigned int a[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        return 0;
    }
    uint8_t addr[6] = {(uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2],
                        (uint8_t)a[3], (uint8_t)a[4], (uint8_t)a[5]};
    return get_rssi_history_json(addr, buf, buf_size);
}

int get_rotation_group_count() {
    if (!_mutex) return 0;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    // Count unique non-negative rotation groups
    int8_t seen_groups[MAC_ROTATION_MAX_GROUPS];
    int group_count = 0;
    for (int i = 0; i < _device_count; i++) {
        if (_devices[i].rotation_group < 0) continue;
        bool found = false;
        for (int j = 0; j < group_count; j++) {
            if (seen_groups[j] == _devices[i].rotation_group) { found = true; break; }
        }
        if (!found && group_count < MAC_ROTATION_MAX_GROUPS) {
            seen_groups[group_count++] = _devices[i].rotation_group;
        }
    }
    xSemaphoreGive(_mutex);
    return group_count;
}

bool is_active() {
    return _running;
}

}  // namespace hal_ble_scanner

#endif  // !SIMULATOR
