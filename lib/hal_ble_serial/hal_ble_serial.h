/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Nordic UART Service UUIDs
#define BLE_NUS_SERVICE_UUID     "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_NUS_RX_CHAR_UUID     "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // Write (phone -> ESP32)
#define BLE_NUS_TX_CHAR_UUID     "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // Notify (ESP32 -> phone)

// Tritium Device Info Service
#define BLE_DIS_SERVICE_UUID     "180A"

namespace hal_ble_serial {

struct BleSerialConfig {
    const char* device_name;     // BLE advertised name (default: "Tritium-XXXX")
    bool auto_advertise;         // Start advertising on init
    bool wifi_fallback_only;     // Only advertise when WiFi is disconnected
    uint16_t mtu;                // Max MTU (default: 512)
    uint32_t adv_interval_ms;    // Advertising interval (default: 100ms)
};

struct BleSerialStatus {
    bool initialized;
    bool advertising;
    bool connected;
    uint8_t client_count;
    uint32_t bytes_rx;
    uint32_t bytes_tx;
    char client_addr[18];        // "XX:XX:XX:XX:XX:XX"
    int8_t rssi;
};

// Callback for received data
typedef void (*BleSerialRxCallback)(const uint8_t* data, size_t len, void* user_data);

// Initialize BLE serial with NUS service
bool init(const BleSerialConfig* config = nullptr);

// Start/stop advertising
bool startAdvertising();
bool stopAdvertising();

// Send data to connected client(s)
bool send(const uint8_t* data, size_t len);
bool sendString(const char* str);
bool printf(const char* fmt, ...);

// Receive callback
void onReceive(BleSerialRxCallback cb, void* user_data = nullptr);

// Status
const BleSerialStatus& getStatus();
bool isConnected();

// Shutdown
void deinit();

}  // namespace hal_ble_serial
