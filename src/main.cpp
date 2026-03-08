#include <Arduino.h>
#include "display.h"
#include "tritium_splash.h"
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

#if defined(ENABLE_LORA) && __has_include("hal_lora.h")
#include "hal_lora.h"
static bool _lora_enabled = true;
#else
static bool _lora_enabled = false;
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

#if defined(ENABLE_DIAG) && __has_include("hal_diag.h")
#include "hal_diag.h"
#if HAS_PMIC && __has_include("hal_power.h")
#include "hal_power.h"
#endif
#if HAS_CAMERA && __has_include("hal_camera.h")
#include "hal_camera.h"
#endif
#if __has_include("hal_touch.h")
#include "hal_touch.h"
#endif
#if __has_include("hal_ntp.h")
#include "hal_ntp.h"
#endif
static bool _diag_enabled = true;
#else
static bool _diag_enabled = false;
#endif

#if defined(ENABLE_ESPNOW) && __has_include("hal_espnow.h")
#include "hal_espnow.h"
static EspNowHAL _espnow;
static bool _espnow_enabled = true;
#else
static bool _espnow_enabled = false;
#endif

// --- App selection via build flag ---
#if defined(APP_STARFIELD)
#include "starfield_app.h"
static StarfieldApp app_instance;

#elif defined(APP_SYSTEM)
#include "system_app.h"
static SystemApp app_instance;

#elif defined(APP_DIAG)
#include "diag_app.h"
static DiagApp app_instance;

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
#if defined(ENABLE_DIAG)
            else if (strcmp(_cmd_buf, "DIAG") == 0) {
                static char dbuf[2048];
                int len = hal_diag::full_report_json(dbuf, sizeof(dbuf));
                if (len > 0) Serial.println(dbuf);
            }
            else if (strcmp(_cmd_buf, "HEALTH") == 0) {
                static char hbuf[1024];
                int len = hal_diag::health_to_json(hbuf, sizeof(hbuf));
                if (len > 0) Serial.println(hbuf);
            }
            else if (strcmp(_cmd_buf, "ANOMALIES") == 0) {
                static char abuf[1024];
                int len = hal_diag::anomalies_to_json(abuf, sizeof(abuf));
                if (len > 0) Serial.println(abuf);
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
#if defined(APP_STARFIELD)
        {
            char l1[48];
            snprintf(l1, sizeof(l1), "%s", wifi.getIP());
            app_instance.setOverlayText("TRITIUM", l1, nullptr);
        }
#endif
    } else {
        Serial.printf("[tritium] WiFi: not connected\n");
#if defined(ENABLE_WEBSERVER)
        // No WiFi? Start AP mode for phone commissioning
        Serial.printf("[tritium] Starting AP mode for commissioning...\n");
        wifi.startAP();  // Creates "Tritium-XXYY" open network
        // Show commissioning info on display overlay
#if defined(APP_STARFIELD)
        {
            char l1[48], l2[48];
            snprintf(l1, sizeof(l1), "WIFI: %s", wifi.getSSID());
            snprintf(l2, sizeof(l2), "HTTP://%s/", wifi.getAPIP());
            app_instance.setOverlayText("TRITIUM SETUP", l1, l2);
        }
#endif
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

#if defined(ENABLE_LORA)
    {
        hal_lora::LoRaConfig lora_cfg;
        if (hal_lora::init(lora_cfg)) {
            Serial.printf("[tritium] LoRa: active (%s)\n", hal_lora::get_mode());
        } else {
            Serial.printf("[tritium] LoRa: init failed\n");
        }
    }
#endif

#if defined(ENABLE_DIAG)
    {
        hal_diag::DiagConfig diag_cfg;
        diag_cfg.health_interval_ms = 30000;
        diag_cfg.log_to_serial = true;
        diag_cfg.anomaly_detection = true;
        if (hal_diag::init(diag_cfg)) {
            Serial.printf("[tritium] Diagnostics: active\n");
            hal_diag::log(hal_diag::Severity::INFO, "system", "Tritium-Edge boot complete");

            // Wire power HAL into diagnostics on boards with PMIC
#if HAS_PMIC && __has_include("hal_power.h")
            {
                static PowerHAL _diag_power;
                _diag_power.initLgfx(0, 0x34);
                hal_diag::set_power_provider([](hal_diag::PowerInfo& out) -> bool {
                    auto info = _diag_power.getInfo();
                    out.battery_voltage = info.voltage;
                    out.battery_percent = (info.percentage >= 0) ? (float)info.percentage : 0.0f;
                    out.charge_current_ma = 0.0f;  // Not exposed by PowerHAL yet
                    out.power_source = info.is_usb_powered ? 1 : (info.has_battery ? 2 : 0);
                    out.pmic_temp_c = 0.0f;  // TODO: add PMIC temp read to PowerHAL
                    return true;
                });
                Serial.printf("[tritium] Diagnostics: power provider wired\n");
            }
#endif

            // Wire camera diagnostics on boards with camera hardware
#if HAS_CAMERA && __has_include("hal_camera.h")
            {
                static CameraHAL _diag_camera;
                if (_diag_camera.init(CamResolution::QVGA_320x240, CamPixelFormat::RGB565)) {
                    hal_diag::set_camera_provider([](hal_diag::CameraInfo& out) -> bool {
                        out.available    = _diag_camera.available();
                        out.frame_count  = _diag_camera.getFrameCount();
                        out.fail_count   = _diag_camera.getFailCount();
                        out.last_capture_us = _diag_camera.getLastCaptureUs();
                        out.max_capture_us  = _diag_camera.getMaxCaptureUs();
                        out.avg_fps      = _diag_camera.getAvgFps();
                        return true;
                    });
                    Serial.printf("[tritium] Diagnostics: camera provider wired\n");
                }
            }
#endif

            // Wire touch diagnostics on boards with touch pins
#if __has_include("hal_touch.h") && defined(TOUCH_SDA)
            {
                static TouchHAL _diag_touch;
#if defined(TOUCH_I2C_NUM) && TOUCH_I2C_NUM == 1
                static TwoWire _touch_wire(1);
                _touch_wire.begin(TOUCH_SDA, TOUCH_SCL);
                _touch_wire.setClock(400000);
                _diag_touch.init(_touch_wire);
#else
                _diag_touch.init(Wire);
#endif
                hal_diag::set_touch_provider([](bool& available) -> bool {
                    available = _diag_touch.available();
                    return true;
                });
                Serial.printf("[tritium] Diagnostics: touch provider wired (%s)\n",
                              _diag_touch.available() ? "detected" : "not found");
            }
#endif

            // Wire NTP diagnostics when WiFi is available
#if __has_include("hal_ntp.h") && defined(ENABLE_WIFI)
            {
                static NtpHAL _diag_ntp;
                _diag_ntp.init();
                hal_diag::set_ntp_provider([](hal_diag::NtpInfo& out) -> bool {
                    out.synced = _diag_ntp.isSynced();
                    uint32_t last = _diag_ntp.getLastSyncEpoch();
                    if (last > 0) {
                        uint32_t now = _diag_ntp.getEpoch();
                        out.last_sync_age_s = (now > last) ? (now - last) : 0;
                    } else {
                        out.last_sync_age_s = 0;
                    }
                    return true;
                });
                Serial.printf("[tritium] Diagnostics: NTP provider wired\n");
            }
#endif
        } else {
            Serial.printf("[tritium] Diagnostics: init failed\n");
        }
    }
#endif

#if defined(ENABLE_ESPNOW)
    {
        // Initialize ESP-NOW mesh with default node role
        // Note: ESP-NOW uses WiFi STA internally but does not conflict with
        // an active WiFi connection on the same channel.
        if (_espnow.init(EspNowRole::NODE, 1)) {
            Serial.printf("[tritium] ESP-NOW Mesh: active\n");
#if defined(ENABLE_DIAG)
            _espnow.enableDiagLogging(true);
            // Wire mesh stats into diagnostics
            hal_diag::set_mesh_provider([](hal_diag::MeshInfo& out) -> bool {
                auto stats = _espnow.getStats();
                out.peer_count = _espnow.getPeerCount();
                out.route_count = (uint8_t)stats.route_count;
                out.tx_count = stats.tx_count;
                out.rx_count = stats.rx_count;
                out.tx_fail = stats.tx_fail;
                out.relay_count = stats.relay_count;
                // Populate peer list for topology mapping
                EspNowPeer peers[hal_diag::MeshInfo::MAX_PEERS];
                int n = _espnow.getPeers(peers, hal_diag::MeshInfo::MAX_PEERS);
                out.peer_list_count = (uint8_t)n;
                for (int i = 0; i < n && i < hal_diag::MeshInfo::MAX_PEERS; i++) {
                    memcpy(out.peers[i].mac, peers[i].mac, 6);
                    out.peers[i].rssi = peers[i].rssi;
                    out.peers[i].hops = peers[i].hop_count;
                }
                return true;
            });
#endif
            // Run initial discovery to find neighbors immediately
            _espnow.meshDiscovery();
        } else {
            Serial.printf("[tritium] ESP-NOW Mesh: init failed\n");
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

            // Wire diagnostics data into web server
#if defined(ENABLE_DIAG)
            _webserver.setDiagProvider([](char* buf, size_t size) -> int {
                return hal_diag::full_report_json(buf, size);
            });
            _webserver.setDiagHealthProvider([](char* buf, size_t size) -> int {
                return hal_diag::health_to_json(buf, size);
            });
            _webserver.setDiagEventsProvider([](char* buf, size_t size) -> int {
                return hal_diag::events_to_json(buf, size, 50);
            });
            _webserver.setDiagAnomaliesProvider([](char* buf, size_t size) -> int {
                return hal_diag::anomalies_to_json(buf, size);
            });
#endif

            // Wire mesh topology data into web server
#if defined(ENABLE_ESPNOW)
            _webserver.setMeshProvider([](char* buf, size_t size) -> int {
                return _espnow.meshToJson(buf, size);
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
#if defined(ENABLE_DIAG)
    hal_diag::tick();
#endif
#if defined(ENABLE_ESPNOW)
    _espnow.process();
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

    // Boot splash — shows on every boot before app starts
    tritium_splash(panel, w, h);

    // Start the app (display something while WiFi connects)
    app->setup(panel, w, h);
    Serial.printf("[tritium] App '%s' started\n", app->name());

    // Then bring up background services
    services_init();

    Serial.printf("[tritium] Ready.\n");
}

void loop() {
#if defined(ENABLE_DIAG)
    uint32_t loop_start = micros();
#endif

    handleSerialCommands();
    app->loop();
    services_tick();

#if defined(ENABLE_DIAG)
    hal_diag::report_loop_time(micros() - loop_start);
#endif
}
