#include "ble_manager.h"
#include <cstdio>
#include <cstring>

BleManager* BleManager::_instance = nullptr;

BleManager* BleManager::instance() { return _instance; }
bool BleManager::isConnected() const { return _connected; }
bool BleManager::isAdvertising() const { return _advertising && !_connected; }

void BleManager::onReceive(BleReceiveCallback cb) { _callback = cb; }

void BleManager::send(const char* data) {
    send(data, strlen(data));
}

// ============================================================================
// Platform: ESP32
// ============================================================================
#ifndef SIMULATOR

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>

static const char* SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char* CHAR_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char* CHAR_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

static BLEServer* bleServer = nullptr;
static BLECharacteristic* txChar = nullptr;
static BLECharacteristic* rxChar = nullptr;

class BleServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* server) override {
        if (BleManager::_instance) {
            BleManager::_instance->_connected = true;
            Serial.println("[BLE] Client connected");
        }
    }
    void onDisconnect(BLEServer* server) override {
        if (BleManager::_instance) {
            BleManager::_instance->_connected = false;
            Serial.println("[BLE] Client disconnected");
            server->startAdvertising();
        }
    }
};

class BleSerialCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* characteristic) override {
        if (!BleManager::_instance) return;
        std::string val = characteristic->getValue();
        if (val.length() > 0 && BleManager::_instance->_callback) {
            BleManager::_instance->_callback(val.c_str(), val.length());
        }
    }
};

BleManager::BleManager() { _instance = this; }

BleManager::~BleManager() {
    shutdown();
    if (_instance == this) _instance = nullptr;
}

void BleManager::init(const char* deviceName) {
    if (_initialized) return;

    BLEDevice::init(deviceName);
    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new BleServerCallbacks());

    BLEService* service = bleServer->createService(SERVICE_UUID);

    txChar = service->createCharacteristic(CHAR_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    txChar->addDescriptor(new BLE2902());

    rxChar = service->createCharacteristic(CHAR_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rxChar->setCallbacks(new BleSerialCallbacks());

    service->start();

    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->setScanResponse(true);
    advertising->setMinPreferred(0x06);
    BLEDevice::startAdvertising();

    _advertising = true;
    _initialized = true;
    Serial.printf("[BLE] Advertising as \"%s\"\n", deviceName);
}

void BleManager::shutdown() {
    if (!_initialized) return;
    BLEDevice::deinit(true);
    _initialized = false;
    _connected = false;
    _advertising = false;
    bleServer = nullptr;
    txChar = nullptr;
    rxChar = nullptr;
}

void BleManager::send(const char* data, size_t len) {
    if (!_connected || !txChar) return;
    txChar->setValue((uint8_t*)data, len);
    txChar->notify();
}

// ============================================================================
// Platform: Desktop Simulator
// ============================================================================
#else // SIMULATOR

BleManager::BleManager() { _instance = this; }

BleManager::~BleManager() {
    shutdown();
    if (_instance == this) _instance = nullptr;
}

void BleManager::init(const char* deviceName) {
    if (_initialized) return;
    _initialized = true;
    _advertising = true;
    printf("[BLE-SIM] Advertising as \"%s\" (simulated)\n", deviceName);
}

void BleManager::shutdown() {
    _initialized = false;
    _connected = false;
    _advertising = false;
}

void BleManager::send(const char* data, size_t len) {
    if (!_connected) return;
    printf("[BLE-SIM] TX: %.*s\n", (int)len, data);
}

#endif // SIMULATOR
