#include <Arduino.h>
#include "display_init.h"
#include "debug_log.h"
#include "app.h"
#include "hal_heartbeat.h"

// WiFi auto-connect (optional, enabled via build flags)
#if !defined(SIMULATOR) && defined(DEFAULT_WIFI_SSID)
#include <WiFi.h>
#define AUTO_WIFI 1
#endif

// Boot-time SD card OTA check (works with any app)
#if !defined(SIMULATOR) && HAS_SDCARD && defined(SD_MMC_D0) && defined(SD_MMC_CLK) && defined(SD_MMC_CMD)
#define BOOT_SD_OTA_CHECK 1
#include <SD_MMC.h>
#include <Update.h>
#include "ota_header.h"
#include "ota_verify.h"
#endif

// --- App selection via build flag ---
#if defined(APP_SYSTEM)
#include "system_app.h"
static SystemApp app_instance;

#elif defined(APP_WIFI_SETUP)
#include "wifi_setup_app.h"
static WifiSetupApp app_instance;

#elif defined(APP_UI_DEMO)
#include "ui_demo_app.h"
static UiDemoApp app_instance;

#elif defined(APP_OTA)
#include "ota_app.h"
static OtaApp app_instance;

#elif defined(APP_STARFIELD)
#include "starfield_app.h"
static StarfieldApp app_instance;

#else
// Default app: starfield
#include "starfield_app.h"
static StarfieldApp app_instance;
#endif

static LGFX display;
static App* app = &app_instance;

void setup() {
    Serial.begin(115200);
    delay(500);
    DebugLog::init(DBG_BACKEND_SERIAL);

    DBG_INFO("main", "Board: %s", DISPLAY_DRIVER);
    DBG_INFO("main", "Display: %dx%d via %s", DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_IF);

    // Board-specific pre-init (I/O expander display reset, etc.)
#if defined(BOARD_TOUCH_LCD_35BC)
    tca9554_reset_display();
#endif

    display.init();
    display.setRotation(DISPLAY_ROTATION);
    display.fillScreen(TFT_BLACK);
    display.setBrightness(255);

    // Manual backlight fallback — only for 3.5B-C where Light_PWM alone isn't enough
#if defined(BOARD_TOUCH_LCD_35BC) && defined(LCD_BL) && LCD_BL >= 0
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
#endif

    DBG_INFO("main", "Display ready: %dx%d (rotation %d)",
             display.width(), display.height(), DISPLAY_ROTATION);

    // Boot-time SD card OTA: if firmware.bin exists on SD, flash it and reboot
#ifdef BOOT_SD_OTA_CHECK
    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    if (SD_MMC.begin("/sdcard", true)) {
        // Check for both firmware.bin (raw) and firmware.ota (packaged with header)
        const char* fwPath = nullptr;
        if (SD_MMC.exists("/firmware.ota")) fwPath = "/firmware.ota";
        else if (SD_MMC.exists("/firmware.bin")) fwPath = "/firmware.bin";

        if (fwPath) {
            DBG_INFO("main", "Found %s on SD card — starting OTA", fwPath);
            display.setTextSize(display.width() < 200 ? 1 : 2);
            display.setTextColor(0xFFE0);
            display.setTextDatum(textdatum_t::top_center);
            display.drawString("SD OTA Update...", display.width() / 2, display.height() / 2 - 16);

            File fw = SD_MMC.open(fwPath, FILE_READ);
            if (fw && fw.size() > 0) {
                size_t fileSize = fw.size();
                size_t fwSize = fileSize;
                size_t fwOffset = 0;

                // Check for OTA header
                bool hasSig = false;
                OtaSignature sig = {};
                OtaFirmwareHeader hdr = {};
                uint32_t expectedCrc = 0;

                if (fileSize > sizeof(OtaFirmwareHeader)) {
                    fw.read((uint8_t*)&hdr, sizeof(hdr));
                    if (hdr.isValid()) {
                        DBG_INFO("main", "OTA header: v%s board=%s size=%u crc=0x%08X",
                                 hdr.version, hdr.board, hdr.firmware_size, hdr.firmware_crc32);
                        fwSize = hdr.firmware_size;
                        expectedCrc = hdr.firmware_crc32;
                        fwOffset = hdr.totalHeaderSize();

                        // Read signature if signed
                        if (hdr.isSigned()) {
                            fw.read((uint8_t*)&sig, sizeof(sig));
                            hasSig = true;
                            DBG_INFO("main", "Signed firmware — will verify ECDSA P-256");
                        }
                    } else {
                        // Not a packaged file, rewind
                        fw.seek(0);
                    }
                }

                if (Update.begin(fwSize)) {
                    // Start streaming verification
                    OtaVerify::crc32Begin();
                    if (hasSig) {
                        OtaVerify::beginVerify();
                        // Signature covers firmware data only (not header)
                    }

                    uint8_t buf[1024];
                    size_t written = 0;
                    while (fw.available() && written < fwSize) {
                        size_t toRead = fwSize - written;
                        if (toRead > sizeof(buf)) toRead = sizeof(buf);
                        size_t n = fw.read(buf, toRead);

                        // Update CRC32 and signature hash
                        OtaVerify::crc32Update(buf, n);
                        if (hasSig) OtaVerify::updateVerify(buf, n);

                        Update.write(buf, n);
                        written += n;
                        int pct = (written * 100) / fwSize;
                        int barY = display.height() / 2 + 8;
                        display.fillRect(10, barY, display.width() - 20, 16, TFT_BLACK);
                        display.fillRect(10, barY,
                                         (display.width() - 20) * pct / 100, 16, 0x07E0);
                    }
                    fw.close();

                    // Verify CRC32
                    uint32_t actualCrc = OtaVerify::crc32Finalize();
                    if (expectedCrc != 0 && actualCrc != expectedCrc) {
                        DBG_ERROR("main", "SD OTA CRC32 mismatch: expected 0x%08X, got 0x%08X",
                                  expectedCrc, actualCrc);
                        display.drawString("CRC ERROR!",
                                           display.width() / 2, display.height() / 2 + 32);
                        Update.abort();
                        delay(3000);

#ifdef OTA_REQUIRE_SIGNATURE
                    } else if (!hasSig) {
                        DBG_ERROR("main", "SD OTA rejected: unsigned firmware (signature required)");
                        display.setTextColor(0xF800);
                        display.drawString("UNSIGNED!",
                                           display.width() / 2, display.height() / 2 + 32);
                        Update.abort();
                        delay(3000);
#endif
                    } else if (hasSig && !OtaVerify::finalizeVerify(sig.r, sig.s)) {
                        DBG_ERROR("main", "SD OTA signature verification FAILED");
                        display.setTextColor(0xF800);
                        display.drawString("SIG INVALID!",
                                           display.width() / 2, display.height() / 2 + 32);
                        Update.abort();
                        delay(3000);
                    } else if (Update.end(true)) {
                        DBG_INFO("main", "SD OTA success, %u bytes from %s%s", written, fwPath,
                                 hasSig ? " (signature verified)" : "");
                        char bakPath[64];
                        snprintf(bakPath, sizeof(bakPath), "%s.bak", fwPath);
                        SD_MMC.remove(bakPath);
                        SD_MMC.rename(fwPath, bakPath);
                        display.drawString(hasSig ? "Verified! Rebooting..." : "OTA OK! Rebooting...",
                                           display.width() / 2, display.height() / 2 + 32);
                        delay(1000);
                        ESP.restart();
                    } else {
                        DBG_ERROR("main", "SD OTA verify failed: %s", Update.errorString());
                    }
                } else {
                    fw.close();
                    DBG_ERROR("main", "SD OTA begin failed: %s", Update.errorString());
                }
            }
        }
        SD_MMC.end();
    }
#endif

    app->setup(display);
    DBG_INFO("main", "App '%s' started", app->name());

    // WiFi auto-connect: if build flags provide credentials, connect before heartbeat
#ifdef AUTO_WIFI
    if (WiFi.status() != WL_CONNECTED) {
        DBG_INFO("main", "WiFi: connecting to %s...", DEFAULT_WIFI_SSID);
        WiFi.mode(WIFI_STA);
        WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
        uint32_t wifiStart = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart) < 10000) {
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED) {
            DBG_INFO("main", "WiFi: connected, IP=%s RSSI=%d",
                     WiFi.localIP().toString().c_str(), WiFi.RSSI());
        } else {
            DBG_INFO("main", "WiFi: connection failed (will retry in background)");
        }
    }
#endif

    // Initialize fleet heartbeat (auto-loads config from provisioning/NVS).
    // If AUTO_WIFI connected and DEFAULT_SERVER_URL is set, use those.
#if defined(DEFAULT_SERVER_URL) && defined(DEFAULT_DEVICE_ID)
    {
        hal_heartbeat::HeartbeatConfig cfg;
        cfg.server_url = DEFAULT_SERVER_URL;
        cfg.device_id = DEFAULT_DEVICE_ID;
        cfg.interval_ms = 30000;  // 30s for active development
        hal_heartbeat::init(cfg);
    }
#else
    hal_heartbeat::init();
#endif
}

// Serial command buffer for board identification
static char _cmd_buf[32];
static uint8_t _cmd_idx = 0;

static void handleSerialCommands() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            _cmd_buf[_cmd_idx] = '\0';
            if (strcmp(_cmd_buf, "IDENTIFY") == 0) {
                Serial.printf("{\"board\":\"%s\",\"display\":\"%dx%d\",\"interface\":\"%s\",\"app\":\"%s\"}\n",
                              DISPLAY_DRIVER, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_IF, app->name());
            }
            _cmd_idx = 0;
        } else if (_cmd_idx < sizeof(_cmd_buf) - 1) {
            _cmd_buf[_cmd_idx++] = c;
        }
    }
}

void loop() {
#if !defined(APP_OTA)
    handleSerialCommands();
#endif
    app->loop(display);
    hal_heartbeat::tick();
}
