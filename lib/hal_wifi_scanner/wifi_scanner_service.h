#pragma once
// WiFi Scanner service adapter — wraps hal_wifi_scanner namespace as a ServiceInterface.
// Priority 50.

#include "service.h"

#if defined(ENABLE_WIFI_SCANNER) && __has_include("hal_wifi_scanner.h")
#include "hal_wifi_scanner.h"
#endif

#if defined(ENABLE_SIGHTING_LOGGER) && __has_include("hal_sighting_logger.h")
#include "hal_sighting_logger.h"
#endif

class WifiScannerService : public ServiceInterface {
public:
    const char* name() const override { return "wifi_scanner"; }
    uint8_t capabilities() const override { return SVC_SERIAL_CMD | SVC_TICK; }
    int initPriority() const override { return 50; }

    bool init() override {
#if defined(ENABLE_WIFI_SCANNER)
        hal_wifi_scanner::ScanConfig wifi_scan_cfg;
        wifi_scan_cfg.scan_interval_ms = 30000;  // Every 30s
        if (hal_wifi_scanner::init(wifi_scan_cfg)) {
            Serial.printf("[tritium] WiFi Scanner: active\n");
            _active = true;
            return true;
        }
        return false;
#else
        return false;
#endif
    }

    void tick() override {
#if defined(ENABLE_WIFI_SCANNER) && defined(ENABLE_SIGHTING_LOGGER)
        // Feed WiFi scan results to the SQLite logger every 30s
        if (_active && hal_sighting_logger::is_active()) {
            uint32_t now = millis();
            if ((now - _last_logger_feed) >= 30000) {
                _last_logger_feed = now;
                hal_wifi_scanner::WifiNetwork nets[WIFI_SCANNER_MAX_NETWORKS];
                int n = hal_wifi_scanner::get_networks(nets, WIFI_SCANNER_MAX_NETWORKS);
                for (int i = 0; i < n; i++) {
                    hal_sighting_logger::log_wifi(nets[i].ssid, nets[i].bssid,
                        nets[i].rssi, nets[i].channel, nets[i].auth_type);
                }
            }
        }
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(ENABLE_WIFI_SCANNER)
        if (strcmp(cmd, "WIFI_SCAN") == 0) {
            Serial.printf("[wifi_scan] Triggering immediate scan...\n");
            // The background task handles scanning; just print current state
            Serial.printf("[wifi_scan] %d networks tracked\n",
                hal_wifi_scanner::get_visible_count());
            return true;
        }
        if (strcmp(cmd, "WIFI_LIST") == 0) {
            hal_wifi_scanner::WifiNetwork nets[WIFI_SCANNER_MAX_NETWORKS];
            int n = hal_wifi_scanner::get_networks(nets, WIFI_SCANNER_MAX_NETWORKS);
            Serial.printf("[wifi_scan] %d networks:\n", n);
            for (int i = 0; i < n; i++) {
                Serial.printf("  %02X:%02X:%02X:%02X:%02X:%02X ch=%u rssi=%d "
                    "seen=%u %s\n",
                    nets[i].bssid[0], nets[i].bssid[1], nets[i].bssid[2],
                    nets[i].bssid[3], nets[i].bssid[4], nets[i].bssid[5],
                    (unsigned)nets[i].channel, nets[i].rssi,
                    (unsigned)nets[i].seen_count, nets[i].ssid);
            }
            return true;
        }
#endif
        return false;
    }

private:
    bool _active = false;
    uint32_t _last_logger_feed = 0;
};
