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

static void prune_stale() {
    uint32_t now = millis();
    int write = 0;
    for (int i = 0; i < _device_count; i++) {
        if ((now - _devices[i].last_seen) < BLE_DEVICE_TIMEOUT_MS) {
            if (write != i) _devices[write] = _devices[i];
            write++;
        } else if (_devices[i].is_known) {
            BLE_DIAG_LOG(hal_diag::Severity::INFO,
                "Known BLE device departed: %s (seen %u times)",
                _devices[i].name, _devices[i].seen_count);
        }
    }
    _device_count = write;
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
            _devices[idx].rssi = dev->getRSSI();
            _devices[idx].last_seen = now;
            _devices[idx].seen_count++;
        } else if (_device_count < BLE_SCANNER_MAX_DEVICES) {
            BleDevice& d = _devices[_device_count];
            memcpy(d.addr, raw, 6);
            d.rssi = dev->getRSSI();
            d.addr_type = addr.getType();
            d.first_seen = now;
            d.last_seen = now;
            d.seen_count = 1;
            d.name[0] = '\0';

            if (dev->haveName()) {
                strncpy(d.name, dev->getName().c_str(), sizeof(d.name) - 1);
                d.name[sizeof(d.name) - 1] = '\0';
            }

            const char* label = nullptr;
            d.is_known = is_known_device(raw, &label);
            if (d.is_known && label) {
                strncpy(d.name, label, sizeof(d.name) - 1);
            }

            _device_count++;

            if (d.is_known) {
                Serial.printf("[ble_scan] Known device: %s (%02X:%02X:%02X:%02X:%02X:%02X) RSSI=%d\n",
                    d.name, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], d.rssi);
                BLE_DIAG_LOG(hal_diag::Severity::INFO,
                    "Known BLE device arrived: %s RSSI=%d", d.name, d.rssi);
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

        // Prune stale devices
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            prune_stale();
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
    for (int i = 0; i < _device_count && pos < (int)buf_size - 80; i++) {
        if ((now - _devices[i].last_seen) >= BLE_DEVICE_TIMEOUT_MS) continue;
        if (!first) pos += snprintf(buf + pos, buf_size - pos, ",");
        first = false;
        pos += snprintf(buf + pos, buf_size - pos,
            "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d,\"seen\":%u%s%s%s}",
            _devices[i].addr[0], _devices[i].addr[1], _devices[i].addr[2],
            _devices[i].addr[3], _devices[i].addr[4], _devices[i].addr[5],
            _devices[i].rssi, (unsigned)_devices[i].seen_count,
            _devices[i].name[0] ? ",\"name\":\"" : "",
            _devices[i].name[0] ? _devices[i].name : "",
            _devices[i].name[0] ? "\"" : "");
        if (_devices[i].is_known && pos < (int)buf_size - 20) {
            // Overwrite last } and add known flag
            pos--;  // back over }
            pos += snprintf(buf + pos, buf_size - pos, ",\"known\":true}");
        }
    }

    pos += snprintf(buf + pos, buf_size - pos, "]");
    xSemaphoreGive(_mutex);
    return pos;
}

bool is_active() {
    return _running;
}

}  // namespace hal_ble_scanner

#endif  // !SIMULATOR
