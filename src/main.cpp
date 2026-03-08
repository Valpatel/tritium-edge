#include <Arduino.h>
#include "display.h"
#include "app.h"

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

// Serial command buffer for board identification
static char _cmd_buf[32];
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
        Serial.printf("[tritium] WiFi: not connected (will retry in background)\n");
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
}

static void services_tick() {
#if defined(ENABLE_HEARTBEAT)
    hal_heartbeat::tick();
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
