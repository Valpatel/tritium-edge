// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_ble_scanner.h"
#include <cstdio>

#if defined(SIMULATOR)

// Stub implementation — running in simulator
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

// ESP-IDF NimBLE host implementation — no NimBLE-Arduino dependency
#include "tritium_compat.h"

#include <esp_bt.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_hs_adv.h>
#include <host/ble_hs_id.h>

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
static bool _host_synced = false;
static ScanConfig _config;
static TaskHandle_t _scan_task_handle = nullptr;
static uint8_t _own_addr_type = BLE_OWN_ADDR_PUBLIC;

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

// --- GAP event handler (called by NimBLE host for scan results) ---

static int gap_event_cb(struct ble_gap_event* event, void* arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc* disc = &event->disc;
        if (!_mutex) break;
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) break;

        const uint8_t* raw = disc->addr.val;
        uint32_t now = millis();

        int idx = find_device(raw);
        if (idx >= 0) {
            _devices[idx].rssi = disc->rssi;
            _devices[idx].last_seen = now;
            _devices[idx].seen_count++;
        } else if (_device_count < BLE_SCANNER_MAX_DEVICES) {
            BleDevice& d = _devices[_device_count];
            memcpy(d.addr, raw, 6);
            d.rssi = disc->rssi;
            d.addr_type = disc->addr.type;
            d.first_seen = now;
            d.last_seen = now;
            d.seen_count = 1;
            d.name[0] = '\0';

            // Parse advertisement data for local name
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0) {
                if (fields.name != nullptr && fields.name_len > 0) {
                    size_t copy_len = fields.name_len < sizeof(d.name) - 1
                                    ? fields.name_len : sizeof(d.name) - 1;
                    memcpy(d.name, fields.name, copy_len);
                    d.name[copy_len] = '\0';
                }
            }

            const char* label = nullptr;
            d.is_known = is_known_device(raw, &label);
            if (d.is_known && label) {
                strncpy(d.name, label, sizeof(d.name) - 1);
                d.name[sizeof(d.name) - 1] = '\0';
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
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        // Scan cycle finished — scan_task will restart
        break;

    default:
        break;
    }
    return 0;
}

// --- NimBLE host callbacks ---

static void on_host_reset(int reason) {
    Serial.printf("[ble_scan] NimBLE host reset, reason=%d\n", reason);
    _host_synced = false;
}

static void on_host_sync(void) {
    // Determine best address type
    int rc = ble_hs_id_infer_auto(0, &_own_addr_type);
    if (rc != 0) {
        Serial.printf("[ble_scan] Failed to infer address type, rc=%d\n", rc);
        _own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    _host_synced = true;
    Serial.printf("[ble_scan] NimBLE host synced (addr_type=%d)\n", _own_addr_type);
}

// --- Background scan task ---

static void scan_task(void* param) {
    // Wait for host to sync
    int wait_count = 0;
    while (!_host_synced && _running && wait_count < 50) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }

    if (!_host_synced) {
        Serial.printf("[ble_scan] ERROR: NimBLE host did not sync after 5s\n");
        _running = false;
        vTaskDelete(nullptr);
        return;
    }

    while (_running) {
        // Configure scan parameters
        // itvl and window are in 0.625ms units
        struct ble_gap_disc_params disc_params = {};
        disc_params.passive = _config.active_scan ? 0 : 1;
        disc_params.itvl = (uint16_t)(_config.scan_interval_ms * 1000 / 625);
        disc_params.window = (uint16_t)(_config.scan_window_ms * 1000 / 625);
        disc_params.filter_policy = 0;  // No whitelist
        disc_params.limited = 0;
        disc_params.filter_duplicates = 0;  // Don't filter — we track our own

        int32_t duration_ms = _config.scan_duration_s * 1000;

        Serial.printf("[ble_scan] Scanning for %lus...\n",
            (unsigned long)_config.scan_duration_s);

        int rc = ble_gap_disc(_own_addr_type, duration_ms,
                              &disc_params, gap_event_cb, nullptr);
        if (rc != 0) {
            Serial.printf("[ble_scan] Failed to start scan, rc=%d, retrying...\n", rc);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Wait for scan duration
        vTaskDelay(pdMS_TO_TICKS(duration_ms + 500));

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

// NimBLE host task — runs the NimBLE event loop
static void nimble_host_task(void* param) {
    nimble_port_run();
    // nimble_port_run() only returns on nimble_port_stop()
    nimble_port_freertos_deinit();
}

// --- Public API ---

bool init(const ScanConfig& config) {
    if (_running) return true;

    _config = config;
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) return false;

    _device_count = 0;
    _host_synced = false;

    // Initialize NimBLE port
    int rc = nimble_port_init();
    if (rc != 0) {
        Serial.printf("[ble_scan] ERROR: nimble_port_init() failed, rc=%d\n", rc);
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
        return false;
    }

    // Configure NimBLE host callbacks
    ble_hs_cfg.reset_cb = on_host_reset;
    ble_hs_cfg.sync_cb = on_host_sync;

    // Start the NimBLE host task
    nimble_port_freertos_init(nimble_host_task);

    _running = true;
    xTaskCreatePinnedToCore(scan_task, "ble_scan", 8192, nullptr, 1,
                            &_scan_task_handle, 1);

    Serial.printf("[ble_scan] Started (interval=%dms, window=%dms, passive=%s)\n",
        config.scan_interval_ms, config.scan_window_ms,
        config.active_scan ? "no" : "yes");
    return true;
}

void shutdown() {
    _running = false;
    if (_scan_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(500));
        _scan_task_handle = nullptr;
    }

    // Cancel any active scan
    ble_gap_disc_cancel();

    // Stop NimBLE host
    int rc = nimble_port_stop();
    if (rc != 0) {
        Serial.printf("[ble_scan] nimble_port_stop() failed, rc=%d\n", rc);
    }
    nimble_port_deinit();

    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
    _host_synced = false;
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
