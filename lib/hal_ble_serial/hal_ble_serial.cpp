/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hal_ble_serial.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>

// ============================================================================
// Platform: Desktop Simulator
// ============================================================================
#ifdef SIMULATOR

namespace hal_ble_serial {

static BleSerialStatus _status = {};

bool init(const BleSerialConfig*) {
    _status.initialized = true;
    _status.advertising = true;
    ::printf("[ble_serial-sim] Initialized (simulated)\n");
    return true;
}

bool startAdvertising() { _status.advertising = true; return true; }
bool stopAdvertising() { _status.advertising = false; return true; }
bool send(const uint8_t* data, size_t len) {
    ::printf("[ble_serial-sim] TX (%zu bytes): %.*s\n", len, (int)len, (const char*)data);
    return true;
}
bool sendString(const char* str) { return send((const uint8_t*)str, strlen(str)); }
bool printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return send((const uint8_t*)buf, n > 0 ? n : 0);
}
void onReceive(BleSerialRxCallback, void*) {}
const BleSerialStatus& getStatus() { return _status; }
bool isConnected() { return _status.connected; }
void deinit() { _status = {}; }

}  // namespace hal_ble_serial

// ============================================================================
// Platform: ESP32
// ============================================================================
#else

#include <Arduino.h>
#include <NimBLEDevice.h>

namespace hal_ble_serial {

// --- Internal state ---
static BleSerialStatus _status = {};
static BleSerialConfig _config = {};
static BleSerialRxCallback _rx_callback = nullptr;
static void* _rx_user_data = nullptr;

static NimBLEServer* _server = nullptr;
static NimBLEService* _nus_service = nullptr;
static NimBLECharacteristic* _tx_char = nullptr;
static NimBLECharacteristic* _rx_char = nullptr;
static uint16_t _negotiated_mtu = 23;  // BLE default

// Device Info Service characteristic UUIDs (Bluetooth SIG)
static const char* DIS_MANUFACTURER_UUID = "2A29";
static const char* DIS_MODEL_UUID        = "2A24";
static const char* DIS_FW_REV_UUID       = "2A26";

// --- Forward declarations ---
static void chunked_send(const uint8_t* data, size_t len);

// --- NimBLE Server Callbacks ---
// NimBLE 2.x (ESP-IDF 5.x) uses NimBLEConnInfo& in callbacks.
// NimBLE 1.x uses plain NimBLEServer* only.
// We provide both overloads so this compiles on either version.

class ServerCallbacks : public NimBLEServerCallbacks {
    // NimBLE 2.x signature
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
        _status.connected = true;
        _status.client_count = server->getConnectedCount();

        // Format client address
        NimBLEAddress addr = connInfo.getAddress();
        snprintf(_status.client_addr, sizeof(_status.client_addr),
                 "%s", addr.toString().c_str());

        Serial.printf("[ble_serial] Client connected: %s\n", _status.client_addr);
    }

    // NimBLE 2.x signature
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
        (void)connInfo;
        _status.connected = false;
        _status.client_count = server->getConnectedCount();
        _status.client_addr[0] = '\0';
        _status.rssi = 0;
        _negotiated_mtu = 23;

        Serial.printf("[ble_serial] Client disconnected (reason=%d)\n", reason);

        // Auto-restart advertising
        if (_status.initialized && _config.auto_advertise) {
            NimBLEDevice::startAdvertising();
        }
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        _negotiated_mtu = mtu;
        Serial.printf("[ble_serial] MTU negotiated: %u\n", mtu);
    }
};

// --- NimBLE RX Characteristic Callbacks ---

class RxCallbacks : public NimBLECharacteristicCallbacks {
    // NimBLE 2.x signature
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        NimBLEAttValue val = characteristic->getValue();
        size_t len = val.length();
        if (len == 0) return;

        _status.bytes_rx += len;

        if (_rx_callback) {
            _rx_callback(val.data(), len, _rx_user_data);
        }
    }
};

static ServerCallbacks _server_callbacks;
static RxCallbacks _rx_callbacks;

// --- Chunked send: split data into MTU-3 sized packets ---

static void chunked_send(const uint8_t* data, size_t len) {
    if (!_tx_char || !_status.connected) return;

    // ATT header is 3 bytes, so max payload = MTU - 3
    uint16_t chunk_size = (_negotiated_mtu > 3) ? (_negotiated_mtu - 3) : 20;

    size_t offset = 0;
    while (offset < len) {
        size_t remaining = len - offset;
        size_t to_send = (remaining < chunk_size) ? remaining : chunk_size;

        _tx_char->setValue(data + offset, to_send);
        _tx_char->notify();

        offset += to_send;
        _status.bytes_tx += to_send;
    }
}

// --- Public API ---

bool init(const BleSerialConfig* config) {
    if (_status.initialized) return true;

    // Apply config with defaults
    if (config) {
        _config = *config;
    } else {
        _config.device_name = "Tritium-Node";
        _config.auto_advertise = true;
        _config.wifi_fallback_only = false;
        _config.mtu = 512;
        _config.adv_interval_ms = 100;
    }

    if (!_config.device_name) {
        _config.device_name = "Tritium-Node";
    }
    if (_config.mtu == 0) {
        _config.mtu = 512;
    }
    if (_config.adv_interval_ms == 0) {
        _config.adv_interval_ms = 100;
    }

    // Initialize NimBLE
    NimBLEDevice::init(_config.device_name);
    NimBLEDevice::setMTU(_config.mtu);

    // Create server
    _server = NimBLEDevice::createServer();
    _server->setCallbacks(&_server_callbacks);

    // --- Nordic UART Service (NUS) ---
    _nus_service = _server->createService(BLE_NUS_SERVICE_UUID);

    // TX characteristic: ESP32 -> phone (Notify)
    _tx_char = _nus_service->createCharacteristic(
        BLE_NUS_TX_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );

    // RX characteristic: phone -> ESP32 (Write / Write No Response)
    _rx_char = _nus_service->createCharacteristic(
        BLE_NUS_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    _rx_char->setCallbacks(&_rx_callbacks);

    _nus_service->start();

    // --- Device Information Service (DIS) ---
    NimBLEService* dis = _server->createService(BLE_DIS_SERVICE_UUID);

    NimBLECharacteristic* mfr = dis->createCharacteristic(
        DIS_MANUFACTURER_UUID, NIMBLE_PROPERTY::READ);
    mfr->setValue("Valpatel Software LLC");

    NimBLECharacteristic* model = dis->createCharacteristic(
        DIS_MODEL_UUID, NIMBLE_PROPERTY::READ);
    model->setValue("Tritium-Edge ESP32-S3");

    NimBLECharacteristic* fw = dis->createCharacteristic(
        DIS_FW_REV_UUID, NIMBLE_PROPERTY::READ);
    fw->setValue("1.0.0");

    dis->start();

    // Reset counters
    memset(&_status, 0, sizeof(_status));
    _status.initialized = true;

    Serial.printf("[ble_serial] Initialized as \"%s\" (MTU=%u)\n",
                  _config.device_name, _config.mtu);

    // Start advertising if configured
    if (_config.auto_advertise) {
        startAdvertising();
    }

    return true;
}

bool startAdvertising() {
    if (!_status.initialized || !_server) return false;

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_NUS_SERVICE_UUID);
    advertising->setScanResponse(true);
    advertising->setMinPreferred(0x06);  // For iOS compatibility

    NimBLEDevice::startAdvertising();

    _status.advertising = true;
    Serial.printf("[ble_serial] Advertising as \"%s\"\n", _config.device_name);
    return true;
}

bool stopAdvertising() {
    if (!_status.initialized) return false;

    NimBLEDevice::getAdvertising()->stop();
    _status.advertising = false;
    Serial.printf("[ble_serial] Advertising stopped\n");
    return true;
}

bool send(const uint8_t* data, size_t len) {
    if (!_status.connected || !_tx_char || len == 0) return false;
    chunked_send(data, len);
    return true;
}

bool sendString(const char* str) {
    if (!str) return false;
    return send((const uint8_t*)str, strlen(str));
}

bool printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return false;
    return send((const uint8_t*)buf, (size_t)n);
}

void onReceive(BleSerialRxCallback cb, void* user_data) {
    _rx_callback = cb;
    _rx_user_data = user_data;
}

const BleSerialStatus& getStatus() {
    return _status;
}

bool isConnected() {
    return _status.connected;
}

void deinit() {
    if (!_status.initialized) return;

    stopAdvertising();
    NimBLEDevice::deinit(true);

    _server = nullptr;
    _nus_service = nullptr;
    _tx_char = nullptr;
    _rx_char = nullptr;
    _rx_callback = nullptr;
    _rx_user_data = nullptr;
    _negotiated_mtu = 23;

    memset(&_status, 0, sizeof(_status));
    Serial.printf("[ble_serial] Shut down\n");
}

}  // namespace hal_ble_serial

#endif  // !SIMULATOR
