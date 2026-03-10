/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tritium-Edge firmware entry point — pure ESP-IDF (no Arduino framework).
 * Uses app_main() with a FreeRTOS main loop task.
 */
#include "tritium_compat.h"
#include "tritium_i2c.h"
#include <ctime>
#include "display.h"
#include "board_fingerprint.h"
#if defined(BOARD_UNIVERSAL)
#include "display_universal.h"
#include "board_config.h"
#endif
#include "tritium_splash.h"
#include "boot_sequence.h"
#include "app.h"
#include "service_registry.h"

// Tritium-OS Shell — wire live status data into the LVGL status bar
#if defined(ENABLE_SHELL) && __has_include("os_shell.h")
#include "os_shell.h"
#include "lvgl_driver.h"
#include "touch_input.h"
#include "shell_apps.h"
#define SHELL_AVAILABLE 1
#else
#define SHELL_AVAILABLE 0
#endif

// TouchHAL global instance — required by touch_input.cpp (extern TouchHAL touch)
#if SHELL_AVAILABLE
#include "hal_touch.h"
TouchHAL touch;
#endif

// --- Service adapters (each #include pulls in its HAL when enabled) ---
#if defined(ENABLE_WIFI)
#include "wifi_service.h"
static WifiService svc_wifi;
#endif

#if defined(ENABLE_HEARTBEAT)
#include "heartbeat_service.h"
static HeartbeatService svc_heartbeat;
#endif

#if defined(ENABLE_BLE_SCANNER)
#include "ble_scanner_service.h"
static BleScannerService svc_ble;
#endif

#if defined(ENABLE_WIFI_SCANNER)
#include "wifi_scanner_service.h"
static WifiScannerService svc_wifi_scan;
#endif

#if defined(ENABLE_SIGHTING_BUFFER)
#include "sighting_buffer_service.h"
static SightingBufferService svc_sighting;
#endif

#if defined(ENABLE_LORA)
#include "lora_service.h"
static LoraService svc_lora;
#endif

#if defined(ENABLE_COT)
#include "cot_service.h"
static CotService svc_cot;
#endif

#if defined(ENABLE_ESPNOW)
#include "espnow_service.h"
static EspNowService svc_espnow;
#endif

#if defined(ENABLE_DIAG)
#include "diag_service.h"
static DiagService svc_diag;
#endif

#if defined(HAS_SDCARD) && HAS_SDCARD && __has_include("seed_service.h")
#include "seed_service.h"
static SeedService svc_seed;
#endif

#if defined(HAS_AUDIO_CODEC) && HAS_AUDIO_CODEC && __has_include("acoustic_modem_service.h")
#include "acoustic_modem_service.h"
static AcousticModemService svc_modem;
#endif

#if defined(ENABLE_WEBSERVER)
#include "webserver_service.h"
static WebServerService svc_webserver;
#endif

#if defined(ENABLE_STORAGE) && __has_include("storage_service.h")
#include "storage_service.h"
static StorageService svc_storage;
#endif

#if defined(ENABLE_SETTINGS) && __has_include("settings_service.h")
#include "settings_service.h"
static SettingsService svc_settings;
#endif

#if defined(ENABLE_BLE_SERIAL) && __has_include("ble_serial_service.h")
#include "ble_serial_service.h"
static BleSerialService svc_ble_serial;
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
#include "wifi_setup_app.h"
static WifiSetupApp app_instance;
#elif defined(APP_UI_DEMO)
#include "ui_demo_app.h"
static UiDemoApp app_instance;
#elif defined(APP_CAMERA)
#include "camera_app.h"
static CameraApp app_instance;
#elif defined(APP_EFFECTS)
#include "effects_app.h"
static EffectsApp app_instance;
#elif defined(APP_OTA)
#include "ota_app.h"
static OtaApp app_instance;
#elif defined(APP_TEST)
#include "test_app.h"
static TestApp app_instance;
#else
#include "starfield_app.h"
static StarfieldApp app_instance;
#endif

static App* app = &app_instance;

// --- Serial command handling ---
static char _cmd_buf[512];
static uint8_t _cmd_idx = 0;

static void handleSerialCommands() {
    Serial.poll();  // Pull data from USB CDC into ring buffer
    while (Serial.availableFromBuffer()) {
        char c = Serial.readFromBuffer();
        if (c == '\n' || c == '\r') {
            _cmd_buf[_cmd_idx] = '\0';
            if (_cmd_idx == 0) { continue; }

            // Split command and args at first space
            char* space = strchr(_cmd_buf, ' ');
            const char* cmd = _cmd_buf;
            const char* args = nullptr;
            if (space) {
                *space = '\0';
                args = space + 1;
            }

            // Built-in commands
            if (strcmp(cmd, "IDENTIFY") == 0) {
                const display_health_t* id_health = display_get_health();
                Serial.printf("{\"board\":\"%s\",\"display\":\"%dx%d\",\"app\":\"%s\","
                              "\"services\":%d}\n",
                              id_health ? id_health->driver : "unknown",
                              display_get_width(), display_get_height(),
                              app->name(), ServiceRegistry::count());
            }
            else if (strcmp(cmd, "SERVICES") == 0) {
                Serial.printf("[svc] %d services:\n", ServiceRegistry::count());
                for (int i = 0; i < ServiceRegistry::count(); i++) {
                    auto* s = ServiceRegistry::at(i);
                    Serial.printf("  %-20s pri=%3d cap=%02X\n",
                                  s->name(), s->initPriority(), s->capabilities());
                }
            }
            // Dispatch to services
            else if (!ServiceRegistry::dispatchCommand(cmd, args)) {
                Serial.printf("[cmd] Unknown: %s\n", cmd);
            }

            _cmd_idx = 0;
        } else if (_cmd_idx < sizeof(_cmd_buf) - 1) {
            _cmd_buf[_cmd_idx++] = c;
        }
    }
}

// --- Register all services ---
static void registerServices() {
#if defined(ENABLE_WIFI)
    ServiceRegistry::add(&svc_wifi);
#endif
#if defined(ENABLE_HEARTBEAT)
    ServiceRegistry::add(&svc_heartbeat);
#endif
#if defined(ENABLE_BLE_SCANNER)
    ServiceRegistry::add(&svc_ble);
#endif
#if defined(ENABLE_WIFI_SCANNER)
    ServiceRegistry::add(&svc_wifi_scan);
#endif
#if defined(ENABLE_SIGHTING_BUFFER)
    ServiceRegistry::add(&svc_sighting);
#endif
#if defined(ENABLE_LORA)
    ServiceRegistry::add(&svc_lora);
#endif
#if defined(ENABLE_COT)
    ServiceRegistry::add(&svc_cot);
#endif
#if defined(ENABLE_ESPNOW)
    ServiceRegistry::add(&svc_espnow);
#endif
#if defined(ENABLE_DIAG)
    ServiceRegistry::add(&svc_diag);
#endif
#if defined(HAS_SDCARD) && HAS_SDCARD && __has_include("seed_service.h")
    ServiceRegistry::add(&svc_seed);
#endif
#if defined(HAS_AUDIO_CODEC) && HAS_AUDIO_CODEC && __has_include("acoustic_modem_service.h")
    ServiceRegistry::add(&svc_modem);
#endif
#if defined(ENABLE_STORAGE) && __has_include("storage_service.h")
    ServiceRegistry::add(&svc_storage);
#endif
#if defined(ENABLE_WEBSERVER)
    ServiceRegistry::add(&svc_webserver);
#endif
#if defined(ENABLE_SETTINGS) && __has_include("settings_service.h")
    ServiceRegistry::add(&svc_settings);
#endif
#if defined(ENABLE_BLE_SERIAL) && __has_include("ble_serial_service.h")
    ServiceRegistry::add(&svc_ble_serial);
#endif
}

// ---------------------------------------------------------------------------
// Shell status bar — poll services every ~1s and push live data to the LVGL
// status bar indicators (WiFi, BLE, mesh, battery, clock).
// ---------------------------------------------------------------------------
#if SHELL_AVAILABLE
static void updateShellStatus() {
    static uint32_t last_update = 0;
    if (millis() - last_update < 1000) return;
    last_update = millis();

    // WiFi status
#if defined(ENABLE_WIFI)
    {
        auto* ws = ServiceRegistry::getAs<WifiService>("wifi");
        if (ws) {
            bool conn = ws->isConnected();
            int32_t rssi = 0;
            if (conn) {
                rssi = ws->manager().getRSSI();
            }
            tritium_shell::setWifiStatus(conn, rssi);
        }
    }
#endif

    // BLE device count
#if defined(ENABLE_BLE_SCANNER)
    {
        int n = hal_ble_scanner::get_visible_count();
        tritium_shell::setBleStatus(n);
    }
#endif

    // Mesh peer count
#if defined(ENABLE_ESPNOW)
    {
        auto* en = ServiceRegistry::getAs<EspNowService>("espnow");
        if (en) {
            tritium_shell::setMeshStatus(en->mesh().peerCount());
        }
    }
#endif

    // Clock — use NTP/RTC wall-clock if synced, otherwise show uptime
    {
        struct tm timeinfo;
        time_t now = time(nullptr);
        if (localtime_r(&now, &timeinfo) && timeinfo.tm_year > 100) {
            tritium_shell::setClock(timeinfo.tm_hour, timeinfo.tm_min);
        } else {
            uint32_t up = millis() / 1000;
            tritium_shell::setClock((up / 3600) % 24, (up / 60) % 60);
        }
    }
}
#endif  // SHELL_AVAILABLE

// ---------------------------------------------------------------------------
// Main task — runs the firmware main loop in a FreeRTOS task
// ---------------------------------------------------------------------------
static void main_task(void* /*arg*/) {
    Serial.begin(115200);
    delay(500);

    Serial.printf("[tritium] Tritium-Edge booting...\n");

    /* Hardware fingerprint — identify the physical board before display init */
    const board_fingerprint_t* fp = board_fingerprint_scan();
    board_fingerprint_print(fp);
#if !defined(BOARD_UNIVERSAL)
    if (!fp->match && fp->detected != BOARD_ID_UNKNOWN) {
        Serial.printf("[tritium] WARNING: Firmware/hardware mismatch detected!\n");
    }
#endif

#if defined(BOARD_UNIVERSAL)
    /* Universal firmware: use fingerprint to select board config at runtime */
    const board_config_t* board_cfg = board_config_get(fp->detected);
    if (!board_cfg) {
        Serial.printf("[tritium] FATAL: No config for detected board %s\n",
                      board_id_to_name(fp->detected));
        vTaskDelete(nullptr);
        return;
    }
    Serial.printf("[tritium] Universal mode: detected %s\n", board_cfg->name);

    esp_err_t ret = display_init_universal(board_cfg);
#else
    esp_err_t ret = display_init();
#endif
    if (ret != ESP_OK) {
        Serial.printf("[tritium] Display init FAILED: 0x%x\n", ret);
        vTaskDelete(nullptr);
        return;
    }

    int w = display_get_width();
    int h = display_get_height();
    esp_lcd_panel_handle_t panel = display_get_panel();
    const display_health_t* dh = display_get_health();

    display_set_brightness(255);
    Serial.printf("[tritium] Board: %s %dx%d\n", dh->driver, w, h);
    if (dh->verified) {
        Serial.printf("[tritium] Display verified: %s (ID 0x%06lX)\n",
                      dh->driver, (unsigned long)dh->actual_id);
    } else {
        Serial.printf("[tritium] WARNING: Display NOT verified! Expected %s (0x%06lX), got 0x%06lX\n",
                      dh->driver, (unsigned long)dh->expected_id, (unsigned long)dh->actual_id);
        Serial.printf("[tritium] Firmware may be running on wrong board (%s)\n", dh->board_name);
    }

    // Boot display: OS builds get full boot sequence, others get simple splash
#if defined(ENABLE_SETTINGS) || defined(ENABLE_DIAG)
    boot_sequence::init(panel, w, h);
    boot_sequence::showLogo(TRITIUM_VERSION);
#else
    tritium_splash(panel, w, h);
#endif

    // Initialize I2C bus — needed by touch controller, IO expanders, sensors
#if defined(TOUCH_SDA) && defined(TOUCH_SCL)
    if (i2c0.begin(TOUCH_SDA, TOUCH_SCL, 400000)) {
        Serial.printf("[tritium] I2C bus initialized (SDA=%d, SCL=%d)\n", TOUCH_SDA, TOUCH_SCL);
    } else {
        Serial.printf("[tritium] I2C bus init FAILED\n");
    }
#endif

    // Register and init all services (boot sequence shows each one)
    registerServices();

#if defined(ENABLE_SETTINGS) || defined(ENABLE_DIAG)
    ServiceRegistry::initAll([](const char* name, bool ok, int /*index*/, int /*total*/) {
        boot_sequence::showService(name, ok ? "ok" : "fail");
    });
    boot_sequence::showReady();
    boot_sequence::finish();
#else
    ServiceRegistry::initAll();
#endif

    // Start the app (skip when shell is active — LVGL owns the display)
#if !SHELL_AVAILABLE
    app->setup(panel, w, h);
    Serial.printf("[tritium] App '%s' started\n", app->name());
#endif

#if !SHELL_AVAILABLE
    // Wire screen capture into web server (works for any app with a framebuffer)
#if defined(ENABLE_WEBSERVER)
    {
        auto* web = ServiceRegistry::getAs<WebServerService>("webserver");
        if (web) {
            svc_webserver.setScreenshotApp(app);
        }
    }
#endif

    // Post-init: show WiFi info on starfield overlay
#if defined(APP_STARFIELD) && defined(ENABLE_WIFI)
    {
        auto* ws = ServiceRegistry::getAs<WifiService>("wifi");
        if (ws && ws->isConnected()) {
            char l1[48];
            snprintf(l1, sizeof(l1), "%s", ws->getIP());
            app_instance.setOverlayText("TRITIUM", l1, nullptr);
        } else if (ws && ws->isAPMode()) {
            char l1[48], l2[48];
            snprintf(l1, sizeof(l1), "WIFI: %s", ws->getSSID());
            snprintf(l2, sizeof(l2), "HTTP://%s/", ws->getAPIP());
            app_instance.setOverlayText("TRITIUM SETUP", l1, l2);
        }
    }
#endif
#endif  // !SHELL_AVAILABLE

    // Initialize the Tritium-OS shell (LVGL window manager with status bar)
#if SHELL_AVAILABLE
    // Initialize LVGL display driver (must be before shell init)
    lv_display_t* lv_disp = lvgl_driver::init(panel, w, h);
    if (lv_disp) {
        Serial.printf("[tritium] LVGL display driver ready\n");
    } else {
        Serial.printf("[tritium] LVGL display driver FAILED\n");
    }

    // Initialize touch input using native I2C
    if (touch.init()) {
        Serial.printf("[tritium] Touch: %s detected\n",
                      touch.getDriver() == TouchHAL::GT911 ? "GT911" :
                      touch.getDriver() == TouchHAL::FT6336 ? "FT6336" :
                      touch.getDriver() == TouchHAL::FT3168 ? "FT3168" : "unknown");
    } else {
        Serial.printf("[tritium] Touch: not detected\n");
    }
    if (touch_input::init()) {
        Serial.printf("[tritium] Touch input: LVGL indev registered\n");
    } else {
        Serial.printf("[tritium] Touch input: FAILED to register indev\n");
    }

    if (tritium_shell::init(panel, w, h)) {
        Serial.printf("[tritium] Shell: initialized %dx%d\n", w, h);
    } else {
        Serial.printf("[tritium] Shell: init failed\n");
    }

    // Register built-in shell apps (Settings, WiFi, Monitor, Mesh, Files, Power)
    shell_apps::register_all_apps();
    Serial.printf("[tritium] Shell: %d apps registered\n", tritium_shell::getAppCount());

    // Show launcher now that all apps are registered
    tritium_shell::showLauncher();

    // Wire display framebuffer into web server screenshot provider
#if defined(ENABLE_WEBSERVER)
    {
        svc_webserver.setScreenshotProvider(
            [](int& w, int& h) -> uint16_t* {
                w = display_get_width();
                h = display_get_height();
                return display_get_framebuffer();
            });
        Serial.printf("[tritium] Screenshot provider: display framebuffer\n");
    }
#endif
#endif

    Serial.printf("[tritium] Ready.\n");

    // Main loop — runs forever as a FreeRTOS task
    for (;;) {
#if defined(ENABLE_DIAG)
        uint32_t loop_start = micros();
#endif

        handleSerialCommands();
        ServiceRegistry::tickAll();

#if SHELL_AVAILABLE
        lvgl_driver::tick();
        tritium_shell::tick();
        updateShellStatus();
#else
        app->loop();
#endif

#if defined(ENABLE_DIAG)
        hal_diag::report_loop_time(micros() - loop_start);
#endif

        // Yield to other tasks (1 tick = 1ms at 1kHz FreeRTOS)
        vTaskDelay(1);
    }
}

// ---------------------------------------------------------------------------
// ESP-IDF entry point
// ---------------------------------------------------------------------------
extern "C" void app_main(void) {
    // Install USB Serial JTAG driver for Serial input
    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 1024,
    };
    usb_serial_jtag_driver_install(&usb_cfg);

    // Launch main task with generous stack (display + LVGL + services)
    xTaskCreatePinnedToCore(main_task, "main", 16384, nullptr, 5, nullptr, 1);
}
