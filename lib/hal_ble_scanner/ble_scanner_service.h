#pragma once
// BLE Scanner service adapter — wraps hal_ble_scanner namespace as a ServiceInterface.
// Priority 50.

#include "service.h"

#if defined(ENABLE_BLE_SCANNER) && __has_include("hal_ble_scanner.h")
#include "hal_ble_scanner.h"
#endif

#if defined(ENABLE_SIGHTING_BUFFER) && __has_include("hal_sighting_buffer.h")
#include "hal_sighting_buffer.h"
#endif

class BleScannerService : public ServiceInterface {
public:
    const char* name() const override { return "ble_scanner"; }
    uint8_t capabilities() const override { return SVC_SERIAL_CMD | SVC_WEB_API; }
    int initPriority() const override { return 50; }

    bool init() override {
#if defined(ENABLE_BLE_SCANNER)
        hal_ble_scanner::ScanConfig ble_cfg;
        ble_cfg.scan_duration_s = 5;
        ble_cfg.pause_between_ms = 10000;  // Scan every 10s
        ble_cfg.active_scan = false;       // Passive — less intrusive
        if (hal_ble_scanner::init(ble_cfg)) {
            Serial.printf("[tritium] BLE Scanner: active\n");
            _active = true;
            return true;
        } else {
            Serial.printf("[tritium] BLE Scanner: failed to start\n");
            return false;
        }
#else
        return false;
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(ENABLE_BLE_SCANNER)
        // BLE_ADD AA:BB:CC:DD:EE:FF Label — register a known BLE device
        if (strcmp(cmd, "BLE_ADD") == 0) {
            if (!args || args[0] == '\0') {
                Serial.printf("[ble] Usage: BLE_ADD AA:BB:CC:DD:EE:FF Label\n");
                return true;
            }
            unsigned int a[6];
            char label[32] = {};
            if (sscanf(args, "%x:%x:%x:%x:%x:%x %31[^\n]",
                       &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], label) >= 6) {
                uint8_t addr[6] = {(uint8_t)a[0],(uint8_t)a[1],(uint8_t)a[2],
                                   (uint8_t)a[3],(uint8_t)a[4],(uint8_t)a[5]};
                hal_ble_scanner::add_known_device(addr, label[0] ? label : "known");
                Serial.printf("[ble] Added known: %02X:%02X:%02X:%02X:%02X:%02X = %s\n",
                              a[0],a[1],a[2],a[3],a[4],a[5], label[0] ? label : "known");
            } else {
                Serial.printf("[ble] Usage: BLE_ADD AA:BB:CC:DD:EE:FF Label\n");
            }
            return true;
        }
        if (strcmp(cmd, "BLE_RSSI") == 0) {
            if (!args || args[0] == '\0') {
                Serial.printf("[ble] Usage: BLE_RSSI AA:BB:CC:DD:EE:FF\n");
                return true;
            }
            char json_buf[1024];
            int len = hal_ble_scanner::get_rssi_history_json_by_mac(args, json_buf, sizeof(json_buf));
            if (len > 0) {
                Serial.printf("[ble] RSSI history: %s\n", json_buf);
            } else {
                Serial.printf("[ble] Device not found: %s\n", args);
            }
            return true;
        }
        if (strcmp(cmd, "BLE_EXT") == 0) {
            if (!args || args[0] == '\0') {
                Serial.printf("[ble] Usage: BLE_EXT AA:BB:CC:DD:EE:FF\n");
                return true;
            }
            unsigned int a[6];
            if (sscanf(args, "%x:%x:%x:%x:%x:%x",
                       &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
                uint8_t addr[6] = {(uint8_t)a[0],(uint8_t)a[1],(uint8_t)a[2],
                                    (uint8_t)a[3],(uint8_t)a[4],(uint8_t)a[5]};
                char json_buf[512];
                int len = hal_ble_scanner::get_device_extended_json(addr, json_buf, sizeof(json_buf));
                if (len > 0) {
                    Serial.printf("[ble] Extended: %s\n", json_buf);
                } else {
                    Serial.printf("[ble] Device not found\n");
                }
            } else {
                Serial.printf("[ble] Usage: BLE_EXT AA:BB:CC:DD:EE:FF\n");
            }
            return true;
        }
        if (strcmp(cmd, "BLE_LIST") == 0) {
            BleDevice devs[16];
            int n = hal_ble_scanner::get_devices(devs, 16);
            Serial.printf("[ble] %d devices:\n", n);
            for (int i = 0; i < n; i++) {
                Serial.printf("  %02X:%02X:%02X:%02X:%02X:%02X rssi=%d seen=%lu %s%s\n",
                    devs[i].addr[0],devs[i].addr[1],devs[i].addr[2],
                    devs[i].addr[3],devs[i].addr[4],devs[i].addr[5],
                    devs[i].rssi, (unsigned long)devs[i].seen_count,
                    devs[i].is_known ? "[KNOWN] " : "",
                    devs[i].name);
            }
            return true;
        }
#endif
        return false;
    }

    int toJson(char* buf, size_t size) override {
#if defined(ENABLE_BLE_SCANNER)
        if (!hal_ble_scanner::is_active()) return 0;

        BleDevice devs[16];
        int n = hal_ble_scanner::get_devices(devs, 16);
        int known = 0;
        for (int i = 0; i < n; i++) if (devs[i].is_known) known++;

        // Buffer BLE sightings for offline storage
#if defined(ENABLE_SIGHTING_BUFFER)
        for (int i = 0; i < n; i++) {
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str),
                "%02X:%02X:%02X:%02X:%02X:%02X",
                devs[i].addr[0], devs[i].addr[1], devs[i].addr[2],
                devs[i].addr[3], devs[i].addr[4], devs[i].addr[5]);
            hal_sighting_buffer::add_ble_sighting(mac_str, devs[i].name,
                devs[i].rssi, devs[i].is_known, devs[i].seen_count);
        }
#endif

        int pos = snprintf(buf, size,
            "{\"active\":true,\"total\":%d,\"known\":%d,\"devices\":[", n, known);
        for (int i = 0; i < n && pos < (int)size - 100; i++) {
            if (i > 0) buf[pos++] = ',';
            pos += snprintf(buf + pos, size - pos,
                "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                "\"rssi\":%d,\"name\":\"%s\",\"seen\":%lu,\"known\":%s}",
                devs[i].addr[0], devs[i].addr[1], devs[i].addr[2],
                devs[i].addr[3], devs[i].addr[4], devs[i].addr[5],
                devs[i].rssi, devs[i].name,
                (unsigned long)devs[i].seen_count,
                devs[i].is_known ? "true" : "false");
        }
        pos += snprintf(buf + pos, size - pos, "]}");
        return pos;
#else
        return 0;
#endif
    }

private:
    bool _active = false;
};
