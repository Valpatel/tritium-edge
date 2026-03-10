/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "ble_serial_service.h"
#include "hal_ble_serial.h"
#include "service_registry.h"
#include <cstring>
#include <cstdio>

#ifndef SIMULATOR
#include "tritium_compat.h"
#endif

// --- Command buffer for BLE-received data ---
static char _ble_cmd_buf[512];
static uint16_t _ble_cmd_idx = 0;
static bool _wifi_fallback_only = false;

// Forward declaration — process a complete command line received over BLE
static void processBleCommand(const char* line);

// --- BLE RX callback: accumulate bytes into command buffer ---
static void bleRxHandler(const uint8_t* data, size_t len, void* user_data) {
    (void)user_data;

    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];

        if (c == '\n' || c == '\r') {
            _ble_cmd_buf[_ble_cmd_idx] = '\0';
            if (_ble_cmd_idx > 0) {
                processBleCommand(_ble_cmd_buf);
            }
            _ble_cmd_idx = 0;
        } else if (_ble_cmd_idx < sizeof(_ble_cmd_buf) - 1) {
            _ble_cmd_buf[_ble_cmd_idx++] = c;
        }
    }
}

// --- Process a complete command received over BLE ---
static void processBleCommand(const char* line) {
    // Split command and args at first space (same as main.cpp pattern)
    char buf[512];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* space = strchr(buf, ' ');
    const char* cmd = buf;
    const char* args = nullptr;
    if (space) {
        *space = '\0';
        args = space + 1;
    }

#ifndef SIMULATOR
    Serial.printf("[ble_serial] RX cmd: %s%s%s\n", cmd,
                  args ? " " : "", args ? args : "");
#endif

    // Try BLE-specific commands first
    if (strcmp(cmd, "BLE_STATUS") == 0) {
        const auto& st = hal_ble_serial::getStatus();
        hal_ble_serial::printf(
            "{\"connected\":%s,\"advertising\":%s,\"bytes_rx\":%lu,"
            "\"bytes_tx\":%lu,\"client\":\"%s\",\"rssi\":%d}\r\n",
            st.connected ? "true" : "false",
            st.advertising ? "true" : "false",
            (unsigned long)st.bytes_rx, (unsigned long)st.bytes_tx,
            st.client_addr, st.rssi);
        return;
    }

    // Built-in IDENTIFY command — respond over BLE
    if (strcmp(cmd, "IDENTIFY") == 0) {
        hal_ble_serial::printf("{\"transport\":\"ble_serial\",\"services\":%d}\r\n",
                               ServiceRegistry::count());
        return;
    }

    // Built-in SERVICES command — respond over BLE
    if (strcmp(cmd, "SERVICES") == 0) {
        hal_ble_serial::printf("[svc] %d services:\r\n", ServiceRegistry::count());
        for (int i = 0; i < ServiceRegistry::count(); i++) {
            auto* s = ServiceRegistry::at(i);
            hal_ble_serial::printf("  %-20s pri=%3d cap=%02X\r\n",
                                   s->name(), s->initPriority(), s->capabilities());
        }
        return;
    }

    // Dispatch to service registry (same as USB serial path)
    if (!ServiceRegistry::dispatchCommand(cmd, args)) {
        hal_ble_serial::printf("[cmd] Unknown: %s\r\n", cmd);
    }
}

// --- ServiceInterface implementation ---

bool BleSerialService::init() {
    hal_ble_serial::BleSerialConfig cfg = {};
    cfg.device_name = "Tritium-Node";
    cfg.auto_advertise = true;
    cfg.wifi_fallback_only = false;
    cfg.mtu = 512;
    cfg.adv_interval_ms = 100;

    _wifi_fallback_only = cfg.wifi_fallback_only;

    if (!hal_ble_serial::init(&cfg)) {
#ifndef SIMULATOR
        Serial.printf("[ble_serial] Failed to initialize\n");
#endif
        return false;
    }

    // Register RX callback to process incoming commands
    hal_ble_serial::onReceive(bleRxHandler, nullptr);

#ifndef SIMULATOR
    Serial.printf("[ble_serial] Service ready\n");
#endif
    return true;
}

void BleSerialService::tick() {
    // wifi_fallback_only mode: manage advertising based on WiFi state
    if (_wifi_fallback_only) {
        // Check WiFi status via service registry (avoids hard dependency on wifi_service.h)
        auto* wifi_svc = ServiceRegistry::get("wifi");
        if (wifi_svc) {
            const auto& st = hal_ble_serial::getStatus();
            // Query WiFi status via toJson — connected if JSON contains "connected":true
            char wbuf[128];
            int wlen = wifi_svc->toJson(wbuf, sizeof(wbuf));
            bool wifi_up = (wlen > 0 && strstr(wbuf, "\"connected\":true") != nullptr);

            if (wifi_up && st.advertising && !st.connected) {
                // WiFi is back — stop BLE advertising (but don't kick connected clients)
                hal_ble_serial::stopAdvertising();
            } else if (!wifi_up && !st.advertising && !st.connected) {
                // WiFi is down — start BLE advertising
                hal_ble_serial::startAdvertising();
            }
        }
    }
}

bool BleSerialService::handleCommand(const char* cmd, const char* args) {
    if (strcmp(cmd, "BLE_STATUS") == 0) {
        const auto& st = hal_ble_serial::getStatus();
#ifndef SIMULATOR
        Serial.printf("[ble_serial] connected=%s advertising=%s clients=%u "
                      "rx=%lu tx=%lu addr=%s rssi=%d\n",
                      st.connected ? "yes" : "no",
                      st.advertising ? "yes" : "no",
                      st.client_count,
                      (unsigned long)st.bytes_rx, (unsigned long)st.bytes_tx,
                      st.client_addr, st.rssi);
#endif
        return true;
    }

    if (strcmp(cmd, "BLE_ADVERTISE") == 0) {
        if (args && strcmp(args, "off") == 0) {
            hal_ble_serial::stopAdvertising();
        } else {
            hal_ble_serial::startAdvertising();
        }
        return true;
    }

    if (strcmp(cmd, "BLE_DISCONNECT") == 0) {
        if (hal_ble_serial::isConnected()) {
            // Deinit and reinit to force disconnect
            hal_ble_serial::deinit();
            hal_ble_serial::BleSerialConfig cfg = {};
            cfg.device_name = "Tritium-Node";
            cfg.auto_advertise = true;
            cfg.wifi_fallback_only = _wifi_fallback_only;
            cfg.mtu = 512;
            cfg.adv_interval_ms = 100;
            hal_ble_serial::init(&cfg);
            hal_ble_serial::onReceive(bleRxHandler, nullptr);
#ifndef SIMULATOR
            Serial.printf("[ble_serial] Client disconnected, re-advertising\n");
#endif
        } else {
#ifndef SIMULATOR
            Serial.printf("[ble_serial] No client connected\n");
#endif
        }
        return true;
    }

    return false;
}

int BleSerialService::toJson(char* buf, size_t size) {
    const auto& st = hal_ble_serial::getStatus();
    return snprintf(buf, size,
        "{\"connected\":%s,\"advertising\":%s,\"bytes_rx\":%lu,"
        "\"bytes_tx\":%lu,\"client\":\"%s\",\"rssi\":%d}",
        st.connected ? "true" : "false",
        st.advertising ? "true" : "false",
        (unsigned long)st.bytes_rx, (unsigned long)st.bytes_tx,
        st.client_addr, st.rssi);
}
