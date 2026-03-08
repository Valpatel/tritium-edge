#include <Arduino.h>
#include "display.h"
#include "app.h"

// SD card support (for SD_FORMAT command) — only when networking libs are available
#if defined(HAS_SDCARD) && HAS_SDCARD && defined(SD_MMC_D0) && defined(ENABLE_WIFI)
#include <SD_MMC.h>
#include <FS.h>
#define SD_FORMAT_AVAILABLE 1
#else
#define SD_FORMAT_AVAILABLE 0
#endif

// --- Optional background services (enabled via build flags) ---
#if defined(ENABLE_WIFI) && __has_include("wifi_manager.h")
#include "wifi_manager.h"
static WifiManager wifi;
static bool _wifi_enabled = true;
#else
static bool _wifi_enabled = false;
#endif

#if defined(ENABLE_HEARTBEAT) && __has_include("hal_heartbeat.h")
#include "hal_heartbeat.h"
static bool _heartbeat_enabled = true;
#else
static bool _heartbeat_enabled = false;
#endif

#if defined(ENABLE_BLE_SCANNER) && __has_include("hal_ble_scanner.h")
#include "hal_ble_scanner.h"
static bool _ble_scanner_enabled = true;
#else
static bool _ble_scanner_enabled = false;
#endif

#if defined(ENABLE_WEBSERVER) && __has_include("hal_webserver.h")
#include "hal_webserver.h"
#include <WiFi.h>
#include <LittleFS.h>
static WebServerHAL _webserver;
static bool _webserver_enabled = true;
#else
static bool _webserver_enabled = false;
#endif

// --- App selection via build flag ---
#if defined(APP_STARFIELD)
#include "starfield_app.h"
static StarfieldApp app_instance;

#elif defined(APP_SYSTEM)
#include "system_app.h"
static SystemApp app_instance;

#elif defined(APP_WIFI_SETUP)
#error "wifi_setup app not yet ported to esp_lcd"
#elif defined(APP_UI_DEMO)
#error "ui_demo app not yet ported to esp_lcd"
#elif defined(APP_CAMERA)
#error "camera app not yet ported to esp_lcd"
#elif defined(APP_EFFECTS)
#error "effects app not yet ported to esp_lcd"
#elif defined(APP_TEST)
#error "test app not yet ported to esp_lcd"
#else
// Default app: starfield
#include "starfield_app.h"
static StarfieldApp app_instance;
#endif

static App* app = &app_instance;

// Serial command buffer
static char _cmd_buf[128];
static uint8_t _cmd_idx = 0;

static void handleSerialCommands() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            _cmd_buf[_cmd_idx] = '\0';
            if (strcmp(_cmd_buf, "IDENTIFY") == 0) {
                Serial.printf("{\"board\":\"%s\",\"display\":\"%dx%d\",\"app\":\"%s\"}\n",
                              DISPLAY_DRIVER,
                              display_get_width(), display_get_height(),
                              app->name());
            }
#if SD_FORMAT_AVAILABLE
            else if (strcmp(_cmd_buf, "SD_FORMAT") == 0) {
                Serial.printf("[sd] Formatting SD card...\n");
                SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
                if (SD_MMC.begin("/sdcard", true)) {  // 1-bit mode
                    File root = SD_MMC.open("/");
                    if (root) {
                        File f = root.openNextFile();
                        while (f) {
                            char path[128];
                            snprintf(path, sizeof(path), "/%s", f.name());
                            f.close();
                            SD_MMC.remove(path);
                            Serial.printf("[sd] Deleted: %s\n", path);
                            f = root.openNextFile();
                        }
                        root.close();
                    }
                    Serial.printf("[sd] Card cleared. Total: %lluMB, Used: %lluMB\n",
                        (unsigned long long)(SD_MMC.totalBytes() / (1024*1024)),
                        (unsigned long long)(SD_MMC.usedBytes() / (1024*1024)));
                    SD_MMC.end();
                } else {
                    Serial.printf("[sd] Could not mount SD card\n");
                }
            }
#endif
#if defined(ENABLE_BLE_SCANNER)
            // BLE_ADD AA:BB:CC:DD:EE:FF Label — register a known BLE device
            else if (strncmp(_cmd_buf, "BLE_ADD ", 8) == 0) {
                unsigned int a[6];
                char label[32] = {};
                if (sscanf(_cmd_buf + 8, "%x:%x:%x:%x:%x:%x %31[^\n]",
                           &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], label) >= 6) {
                    uint8_t addr[6] = {(uint8_t)a[0],(uint8_t)a[1],(uint8_t)a[2],
                                       (uint8_t)a[3],(uint8_t)a[4],(uint8_t)a[5]};
                    hal_ble_scanner::add_known_device(addr, label[0] ? label : "known");
                    Serial.printf("[ble] Added known: %02X:%02X:%02X:%02X:%02X:%02X = %s\n",
                                  a[0],a[1],a[2],a[3],a[4],a[5], label[0] ? label : "known");
                } else {
                    Serial.printf("[ble] Usage: BLE_ADD AA:BB:CC:DD:EE:FF Label\n");
                }
            }
            else if (strcmp(_cmd_buf, "BLE_LIST") == 0) {
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
            }
#endif
            _cmd_idx = 0;
        } else if (_cmd_idx < sizeof(_cmd_buf) - 1) {
            _cmd_buf[_cmd_idx++] = c;
        }
    }
}

// --- Optional WiFi + Heartbeat background services ---
static void services_init() {
#if defined(ENABLE_WIFI)
    Serial.printf("[tritium] WiFi: connecting...\n");
    wifi.init();

    // Add default network if provided via build flags
#if defined(DEFAULT_WIFI_SSID) && defined(DEFAULT_WIFI_PASS)
    if (wifi.getSavedCount() == 0) {
        wifi.addNetwork(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
    }
#endif

    wifi.connect();

    // Wait up to 10s for connection
    uint32_t start = millis();
    while (!wifi.isConnected() && (millis() - start) < 10000) {
        delay(100);
    }

    if (wifi.isConnected()) {
        Serial.printf("[tritium] WiFi: %s (%s)\n", wifi.getSSID(), wifi.getIP());
    } else {
        Serial.printf("[tritium] WiFi: not connected\n");
#if defined(ENABLE_WEBSERVER)
        // No WiFi? Start AP mode for phone commissioning
        Serial.printf("[tritium] Starting AP mode for commissioning...\n");
        wifi.startAP();  // Creates "Tritium-XXYY" open network
#else
        Serial.printf("[tritium] Will retry in background\n");
#endif
    }
#endif

#if defined(ENABLE_HEARTBEAT)
    if (_wifi_enabled) {
        hal_heartbeat::HeartbeatConfig hb_cfg;
#if defined(DEFAULT_SERVER_URL)
        hb_cfg.server_url = DEFAULT_SERVER_URL;
#endif
#if defined(DEFAULT_DEVICE_ID)
        hb_cfg.device_id = DEFAULT_DEVICE_ID;
#endif
        hb_cfg.interval_ms = 30000;  // 30s heartbeats
        if (hal_heartbeat::init(hb_cfg)) {
            Serial.printf("[tritium] Heartbeat: active\n");
        } else {
            Serial.printf("[tritium] Heartbeat: not configured\n");
        }
    }
#endif

#if defined(ENABLE_BLE_SCANNER)
    {
        hal_ble_scanner::ScanConfig ble_cfg;
        ble_cfg.scan_duration_s = 5;
        ble_cfg.pause_between_ms = 10000;  // Scan every 10s
        ble_cfg.active_scan = false;       // Passive — less intrusive
        if (hal_ble_scanner::init(ble_cfg)) {
            Serial.printf("[tritium] BLE Scanner: active\n");
        } else {
            Serial.printf("[tritium] BLE Scanner: failed to start\n");
        }
    }
#endif

#if defined(ENABLE_WEBSERVER)
    if (_wifi_enabled && (wifi.isConnected() || wifi.isAPMode())) {
        LittleFS.begin(true);  // Format on first mount

        uint16_t web_port = 80;
        if (_webserver.init(web_port)) {
            _webserver.addAllPages();

            // Wire BLE data into web server if BLE scanner is active
#if defined(ENABLE_BLE_SCANNER)
            _webserver.setBleProvider([](char* buf, size_t size) -> int {
                if (!hal_ble_scanner::is_active()) return 0;

                BleDevice devs[16];
                int n = hal_ble_scanner::get_devices(devs, 16);
                int known = 0;
                for (int i = 0; i < n; i++) if (devs[i].is_known) known++;

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
            });
#endif

            // In AP mode, start captive portal for auto-redirect
            if (wifi.isAPMode()) {
                _webserver.startCaptivePortal();
                Serial.printf("[tritium] Web server: http://%s:%u/ (captive portal)\n",
                              wifi.getAPIP(), web_port);
                Serial.printf("[tritium] Connect phone to WiFi '%s' for setup\n",
                              wifi.getSSID());
            } else {
                // Normal mode: start mDNS for easy phone discovery
                uint8_t mac[6];
                WiFi.macAddress(mac);
                char hostname[32];
                snprintf(hostname, sizeof(hostname), "tritium-%02x%02x", mac[4], mac[5]);
                _webserver.startMDNS(hostname);
                Serial.printf("[tritium] Web server: http://%s:%u/ (mDNS: %s.local)\n",
                              wifi.getIP(), web_port, hostname);
            }
        }
    }
#endif
}

static void services_tick() {
#if defined(ENABLE_HEARTBEAT)
    hal_heartbeat::tick();
#endif
#if defined(ENABLE_WEBSERVER)
    _webserver.process();
#endif
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.printf("[tritium] Tritium-Edge booting...\n");

    esp_err_t ret = display_init();
    if (ret != ESP_OK) {
        Serial.printf("[tritium] Display init FAILED: 0x%x\n", ret);
        return;
    }

    int w = display_get_width();
    int h = display_get_height();
    esp_lcd_panel_handle_t panel = display_get_panel();

    display_set_brightness(255);

    Serial.printf("[tritium] Board: %s %dx%d\n", DISPLAY_DRIVER, w, h);

    // Start the app first (display something while WiFi connects)
    app->setup(panel, w, h);
    Serial.printf("[tritium] App '%s' started\n", app->name());

    // Then bring up background services
    services_init();

    Serial.printf("[tritium] Ready.\n");
}

void loop() {
    handleSerialCommands();
    app->loop();
    services_tick();
}
