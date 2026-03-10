#include "ota_app.h"
#include <cstring>
#include <cstdio>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>

#ifndef SIMULATOR
#include "esp_ota_ops.h"
#include <esp_ota_ops.h>
// SD card via VFS — use POSIX file ops on /sdcard/
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include <sys/stat.h>
#endif

// ============================================================================
// 5x7 Bitmap Font (space 0x20 through tilde 0x7E, 95 glyphs)
// ============================================================================
static const uint8_t font5x7[][5] PROGMEM = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x00,0x08,0x14,0x22,0x41}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x41,0x22,0x14,0x08,0x00}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x01,0x01}, // F
    {0x3E,0x41,0x41,0x51,0x32}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x04,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x00,0x7F,0x41,0x41,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20}, // backslash
    {0x00,0x41,0x41,0x7F,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0x00,0x01,0x02,0x04,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7E,0x09,0x01,0x02}, // f
    {0x0C,0x52,0x52,0x52,0x3E}, // g
    {0x7F,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7D,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00}, // j
    {0x7F,0x10,0x28,0x44,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78}, // m
    {0x7C,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7C}, // q
    {0x7C,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x20,0x7C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
    {0x00,0x08,0x36,0x41,0x00}, // {
    {0x00,0x00,0x7F,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00}, // }
    {0x10,0x08,0x08,0x10,0x08}, // ~
};

// ============================================================================
// Framebuffer Drawing Helpers
// ============================================================================
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);  // byte-swap for QSPI SPI transport
}

static void drawChar(uint16_t* fb, int fbw, int fbh, int x, int y,
                     char c, uint16_t color) {
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t* glyph = font5x7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = pgm_read_byte(&glyph[col]);
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < fbw && py >= 0 && py < fbh) {
                    fb[py * fbw + px] = color;
                }
            }
        }
    }
}

static void drawString(uint16_t* fb, int fbw, int fbh, int x, int y,
                       const char* str, uint16_t color) {
    while (*str) {
        drawChar(fb, fbw, fbh, x, y, *str, color);
        x += 6;  // 5px glyph + 1px spacing
        str++;
    }
}

// Draw string centered horizontally at the given y coordinate
static void drawStringCentered(uint16_t* fb, int fbw, int fbh, int y,
                               const char* str, uint16_t color) {
    int len = strlen(str);
    int px_width = len * 6 - 1;  // 5px glyph + 1px spacing, minus trailing space
    int x = (fbw - px_width) / 2;
    drawString(fb, fbw, fbh, x, y, str, color);
}

// Draw string centered horizontally with scale multiplier
static void drawStringCenteredScaled(uint16_t* fb, int fbw, int fbh, int y,
                                     const char* str, uint16_t color, int scale) {
    if (scale <= 1) {
        drawStringCentered(fb, fbw, fbh, y, str, color);
        return;
    }
    int len = strlen(str);
    int px_width = len * 6 * scale - scale;
    int sx = (fbw - px_width) / 2;
    for (const char* p = str; *p; p++) {
        char c = *p;
        if (c < 0x20 || c > 0x7E) { sx += 6 * scale; continue; }
        const uint8_t* glyph = font5x7[c - 0x20];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = pgm_read_byte(&glyph[col]);
            for (int row = 0; row < 7; row++) {
                if (bits & (1 << row)) {
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            int px = sx + col * scale + dx;
                            int py = y + row * scale + dy;
                            if (px >= 0 && px < fbw && py >= 0 && py < fbh) {
                                fb[py * fbw + px] = color;
                            }
                        }
                    }
                }
            }
        }
        sx += 6 * scale;
    }
}

static void fillRect(uint16_t* fb, int fbw, int fbh,
                     int rx, int ry, int rw, int rh, uint16_t color) {
    for (int row = ry; row < ry + rh; row++) {
        if (row < 0 || row >= fbh) continue;
        for (int col = rx; col < rx + rw; col++) {
            if (col >= 0 && col < fbw) {
                fb[row * fbw + col] = color;
            }
        }
    }
}

static void drawHLine(uint16_t* fb, int fbw, int fbh,
                      int x, int y, int len, uint16_t color) {
    if (y < 0 || y >= fbh) return;
    for (int i = 0; i < len; i++) {
        int px = x + i;
        if (px >= 0 && px < fbw) {
            fb[y * fbw + px] = color;
        }
    }
}

static void drawRect(uint16_t* fb, int fbw, int fbh,
                     int rx, int ry, int rw, int rh, uint16_t color) {
    drawHLine(fb, fbw, fbh, rx, ry, rw, color);           // top
    drawHLine(fb, fbw, fbh, rx, ry + rh - 1, rw, color);  // bottom
    for (int row = ry; row < ry + rh; row++) {
        if (row < 0 || row >= fbh) continue;
        if (rx >= 0 && rx < fbw) fb[row * fbw + rx] = color;
        int right = rx + rw - 1;
        if (right >= 0 && right < fbw) fb[row * fbw + right] = color;
    }
}

// ============================================================================
// Framebuffer Push (DMA chunked)
// ============================================================================
void OtaApp::pushFramebuffer() {
    for (int y = 0; y < _h; y += CHUNK_ROWS) {
        int rows = CHUNK_ROWS;
        if (y + rows > _h) rows = _h - y;
        int pixels = _w * rows;
        memcpy(_dma_buf, &_framebuf[y * _w], pixels * 2);
        esp_lcd_panel_draw_bitmap(_panel, 0, y, _w, y + rows, _dma_buf);
    }
}

// ============================================================================
// Setup
// ============================================================================
void OtaApp::setup(esp_lcd_panel_handle_t panel, int width, int height) {
    _panel = panel;
    _w = width;
    _h = height;

    // Allocate PSRAM framebuffer
    size_t fb_size = _w * _h * sizeof(uint16_t);
    _framebuf = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (!_framebuf) {
        Serial.printf("[ota] FATAL: framebuffer alloc failed\n");
        while (1) delay(1000);
    }
    memset(_framebuf, 0, fb_size);

    // Allocate DMA transfer buffer in internal SRAM
    size_t dma_size = _w * CHUNK_ROWS * sizeof(uint16_t);
    _dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA);
    if (!_dma_buf) {
        Serial.printf("[ota] FATAL: DMA buffer alloc failed\n");
        while (1) delay(1000);
    }

    // Init OTA HAL
    _ota_ok = _ota.init();
    Serial.printf("[ota] OTA HAL: %s (v%s, partition: %s, max: %u bytes)\n",
                  _ota_ok ? "OK" : "FAIL", _ota.getCurrentVersion(),
                  _ota.getRunningPartition(), _ota.getMaxFirmwareSize());

    // Delay app confirmation — gives the watchdog time to detect boot loops.
    // If the app crashes within the first 30s, ESP-IDF will auto-rollback
    // to the previous firmware on the next boot.
    // confirmApp() is called later in loop() after the stability window.
    _app_confirm_timer = millis();

    // Init provisioning
    _prov_ok = _provision.init();
    Serial.printf("[ota] Provisioning: %s (%s)\n",
                  _prov_ok ? "OK" : "FAIL",
                  _provision.isProvisioned() ? "provisioned" : "unprovisioned");

    // Init WiFi (auto-connect using stored credentials from NVS)
    _wifi.init();
    _wifi.connect();  // Connects to best saved network
    _wifi_ok = true;
    Serial.printf("[ota] WiFi: init, connecting to saved networks...\n");

    // Init SD card
    _sd_ok = _sd.init();
    if (_sd_ok) {
        Serial.printf("[ota] SD card: %s, %llu MB total\n",
                      _sd.getFilesystemType(), _sd.totalBytes() / (1024 * 1024));
        _have_firmware_file = _sd.exists("/firmware.bin") || _sd.exists("/firmware.ota");
        if (_have_firmware_file) {
            Serial.printf("[ota] Found firmware on SD card (%s)\n",
                          _sd.exists("/firmware.ota") ? ".ota" : ".bin");
        }
    } else {
        Serial.printf("[ota] SD card not available\n");
    }

    // Init ESP-NOW
    _espnow_ok = _espnow.init(EspNowRole::NODE, 1);
    if (_espnow_ok) {
        uint8_t mac[6];
        _espnow.getMAC(mac);
        Serial.printf("[ota] ESP-NOW: OK, MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        // Register mesh callback for OTA messages
        _espnow.onMeshReceive([this](const uint8_t* src, const uint8_t* data,
                                       uint8_t len, uint8_t hops) {
            handleEspNowOTA(src, data, len, hops);
        });
    } else {
        Serial.printf("[ota] ESP-NOW init failed\n");
    }

    // Check for SD card firmware on startup
    if (_sd_ok && _have_firmware_file) {
        snprintf(_status_line, sizeof(_status_line), "SD firmware found! Flashing...");
        drawStatus();
        if (checkSDUpdate()) {
            snprintf(_status_line, sizeof(_status_line), "SD OTA complete! Rebooting...");
            drawStatus();
            delay(1000);
            _ota.reboot();
        }
    }

    // Init BLE OTA
    _ble_ok = _ble_ota.init("ESP32-OTA");
    if (_ble_ok) {
        Serial.printf("[ota] BLE OTA: advertising\n");
        _ble_ota.onProgress([this](uint8_t pct, uint32_t received, uint32_t total) {
            _progress = pct;
            snprintf(_status_line, sizeof(_status_line), "BLE: %u%% (%u/%u)", pct, received, total);
        });
    }

    // Init mesh OTA (robust chunked P2P with flow control + encryption)
    if (_espnow_ok) {
        if (_mesh_ota.init(&_espnow)) {
            _mesh_ota.onProgress([this](size_t received, size_t total) {
                _progress = total > 0 ? (uint8_t)(received * 100 / total) : 0;
                snprintf(_status_line, sizeof(_status_line), "Mesh OTA: %u%%", _progress);
            });
            _mesh_ota.onResult([this](bool success, const char* version) {
                if (success) {
                    snprintf(_status_line, sizeof(_status_line), "Mesh OTA OK! v%s", version);
                    Serial.printf("[ota] Mesh OTA success, rebooting in 2s...\n");
                    delay(2000);
                    _ota.reboot();
                } else {
                    snprintf(_status_line, sizeof(_status_line), "Mesh OTA failed");
                }
            });
            // Auto-enable receiving by default
            _mesh_ota.enableReceive(true);
            Serial.printf("[ota] Mesh OTA: initialized, receive enabled\n");
        }
    }

    // If we have firmware.bin on SD, periodically offer it to mesh peers
    if (_sd_ok && _have_firmware_file && _espnow_ok) {
        _offer_timer = millis();
    }

    // Init OTA audit log
    _audit.init();
    Serial.printf("[ota] OTA audit: %d entries logged\n", _audit.getEntryCount());

    snprintf(_status_line, sizeof(_status_line), "Ready. Waiting for OTA...");
    Serial.printf("[ota] OTA app ready\n");
    Serial.printf("[ota]   SD OTA: %s\n", _sd_ok ? "available" : "no SD");
    Serial.printf("[ota]   Serial OTA: listening\n");
    Serial.printf("[ota]   BLE OTA: %s\n", _ble_ok ? "advertising" : "unavailable");
    Serial.printf("[ota]   ESP-NOW P2P: %s\n", _espnow_ok ? "active" : "unavailable");
    Serial.printf("[ota]   Partition: %s (max %u bytes)\n",
                  _ota.getRunningPartition(), _ota.getMaxFirmwareSize());

    Serial.printf("OTA_READY\n");
}

// ============================================================================
// Loop
// ============================================================================
void OtaApp::loop() {
    // If USB provisioning is active, let the provision HAL own Serial
    if (_provision.isUSBProvisionActive()) {
        bool done = _provision.processUSBProvision();
        if (done) {
            Serial.printf("[ota] USB provisioning completed, device_id: %s\n",
                          _provision.getIdentity().device_id);
            snprintf(_status_line, sizeof(_status_line), "Provisioned: %s",
                     _provision.getIdentity().device_id);
        }
        // Still redraw status
        uint32_t now = millis();
        if (now - _status_timer >= 250) {
            _status_timer = now;
            drawStatus();
        }
        return;
    }

    // Delayed app confirmation — confirm after stability window
    if (!_app_confirmed && _ota_ok && (millis() - _app_confirm_timer >= APP_CONFIRM_DELAY_MS)) {
        _ota.confirmApp();
        _app_confirmed = true;
        Serial.printf("[ota] App confirmed stable after %u ms\n", APP_CONFIRM_DELAY_MS);
    }

    handleSerialOTA();

    // Don't run ESP-NOW or other processing during serial OTA receive
    // (debug output would corrupt the binary data stream)
    if (_serial_state == SerialOtaState::RECEIVING) return;

    if (_ble_ok) {
        _ble_ota.process();
    }

    if (_espnow_ok) {
        _espnow.process();
        _mesh_ota.process();

        // Periodically offer firmware to mesh if we have it on SD
        if (_have_firmware_file && _p2p_state == P2PState::IDLE) {
            uint32_t now = millis();
            if (now - _offer_timer >= OFFER_INTERVAL_MS) {
                _offer_timer = now;
                offerFirmwareToMesh();
            }
        }

        // P2P timeout check
        if (_p2p_state == P2PState::RECEIVING || _p2p_state == P2PState::SENDING) {
            if (millis() - _p2p_last_activity > P2P_TIMEOUT_MS) {
                Serial.printf("[ota] P2P transfer timed out\n");
                snprintf(_status_line, sizeof(_status_line), "P2P timeout!");
                _p2p_state = P2PState::ERROR;
                _releaseOta();
#ifndef SIMULATOR
                Update.abort();
#endif
            }
        }
    }

    // Fleet heartbeat (if WiFi connected and provisioned)
    wifi_ap_record_t _hb_ap;
    if (_provision.isProvisioned() && esp_wifi_sta_get_ap_info(&_hb_ap) == ESP_OK) {
        uint32_t now2 = millis();
        if (now2 - _heartbeat_timer >= HEARTBEAT_INTERVAL_MS) {
            _heartbeat_timer = now2;
            sendHeartbeat();
        }
    }

    // Redraw status every 250ms
    uint32_t now = millis();
    if (now - _status_timer >= 250) {
        _status_timer = now;
        drawStatus();
    }
}

// ============================================================================
// Display — framebuffer rendering with 5x7 bitmap font
// ============================================================================
void OtaApp::drawStatus() {
    bool narrow = (_w < 200);  // 3.49 is 172px wide
    uint16_t colGreen  = rgb565(0, 255, 0);
    uint16_t colRed    = rgb565(255, 0, 0);
    uint16_t colWhite  = rgb565(255, 255, 255);
    uint16_t colYellow = rgb565(255, 255, 0);
    uint16_t colGray   = rgb565(123, 123, 123);

    // Clear framebuffer
    memset(_framebuf, 0, _w * _h * sizeof(uint16_t));

    // Scale text sizes based on display width
    int titleScale = narrow ? 1 : 2;
    int bodyScale = 1;
    int lineH = narrow ? 10 : 14;
    int margin = narrow ? 2 : 8;

    char buf[80];

    // Title
    int y = margin;
    drawStringCenteredScaled(_framebuf, _w, _h, y, "OTA Update", colGreen, titleScale);
    y += titleScale * 8 + 4;

    // Partition info
    if (narrow) {
        // Two shorter lines for narrow displays
        snprintf(buf, sizeof(buf), "%s", _ota.getRunningPartition());
        drawStringCentered(_framebuf, _w, _h, y, buf, colWhite); y += lineH;
        snprintf(buf, sizeof(buf), "Max:%uKB", (unsigned)(_ota.getMaxFirmwareSize() / 1024));
        drawStringCentered(_framebuf, _w, _h, y, buf, colWhite); y += lineH + 2;
    } else {
        drawStringCentered(_framebuf, _w, _h, y, _ota.getCurrentVersion(), colWhite); y += lineH;
        snprintf(buf, sizeof(buf), "Part: %s  Max: %uKB",
                 _ota.getRunningPartition(),
                 (unsigned)(_ota.getMaxFirmwareSize() / 1024));
        drawStringCentered(_framebuf, _w, _h, y, buf, colWhite); y += lineH + 4;
    }

    // Capabilities
    snprintf(buf, sizeof(buf), "SD:%s", _sd_ok ? "OK" : "N/A");
    drawStringCentered(_framebuf, _w, _h, y, buf, _sd_ok ? colGreen : colRed); y += lineH;

    int peers = _espnow_ok ? _espnow.getPeerCount() : 0;
    snprintf(buf, sizeof(buf), narrow ? "NOW:%s P:%d" : "ESP-NOW:%s Peers:%d",
             _espnow_ok ? "OK" : "N/A", peers);
    drawStringCentered(_framebuf, _w, _h, y, buf, _espnow_ok ? colGreen : colRed); y += lineH;

    snprintf(buf, sizeof(buf), narrow ? "BLE:%s" : "BLE:%s%s",
             _ble_ok ? "OK" : "N/A",
             (!narrow && _ble_ota.isConnected()) ? " Connected" : "");
    drawStringCentered(_framebuf, _w, _h, y, buf, _ble_ok ? colGreen : colRed); y += lineH;

    wifi_ap_record_t _ds_ap;
    bool wifiConn = (esp_wifi_sta_get_ap_info(&_ds_ap) == ESP_OK);
    if (wifiConn) {
        char _ds_ip[16] = "";
        if (!narrow) {
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    esp_ip4addr_ntoa(&ip_info.ip, _ds_ip, sizeof(_ds_ip));
                }
            }
        }
        snprintf(buf, sizeof(buf), narrow ? "WiFi:%s" : "WiFi:%s %s",
                 (const char*)_ds_ap.ssid,
                 narrow ? "" : _ds_ip);
    } else {
        snprintf(buf, sizeof(buf), "WiFi:N/A");
    }
    drawStringCentered(_framebuf, _w, _h, y, buf, wifiConn ? colGreen : colRed); y += lineH;

    drawStringCentered(_framebuf, _w, _h, y, "Serial:listen", colGreen); y += lineH + 2;

    // Firmware file on SD
    if (_have_firmware_file) {
        drawStringCentered(_framebuf, _w, _h, y, narrow ? "FW on SD!" : "firmware.bin on SD!", colYellow);
        y += lineH;
    }

    // Status
    y += 4;
    int statusScale = narrow ? 1 : 2;
    // Truncate status for narrow displays
    if (narrow && strlen(_status_line) > 20) {
        char trunc[24];
        strncpy(trunc, _status_line, 20);
        trunc[20] = '\0';
        drawStringCenteredScaled(_framebuf, _w, _h, y, trunc, colWhite, statusScale);
    } else {
        drawStringCenteredScaled(_framebuf, _w, _h, y, _status_line, colWhite, statusScale);
    }
    y += statusScale * 8 + 4;

    // Progress bar
    if (_progress > 0) {
        int barW = _w - margin * 2;
        int barH = narrow ? 10 : 16;
        int barX = margin;
        int barY = y;
        drawRect(_framebuf, _w, _h, barX, barY, barW, barH, colWhite);
        int fillW = (barW - 2) * _progress / 100;
        fillRect(_framebuf, _w, _h, barX + 1, barY + 1, fillW, barH - 2, colGreen);

        snprintf(buf, sizeof(buf), "%d%%", _progress);
        drawStringCentered(_framebuf, _w, _h, barY + barH + 2, buf, colWhite);
        y += barH + lineH;
    }

    // Instructions at bottom (only if enough room)
    if (_h - y > 40) {
        int bottomY = _h - (narrow ? 36 : 48);
        if (narrow) {
            drawStringCentered(_framebuf, _w, _h, bottomY, "Ser/SD/BLE/P2P", colGray);
            drawStringCentered(_framebuf, _w, _h, bottomY + 10, "OTA ready", colGray);
        } else {
            drawStringCentered(_framebuf, _w, _h, bottomY, "Serial: OTA_BEGIN <sz> <crc>", colGray);
            drawStringCentered(_framebuf, _w, _h, bottomY + 12, "SD: place firmware.bin", colGray);
            drawStringCentered(_framebuf, _w, _h, bottomY + 24, "BLE: connect to ESP32-OTA", colGray);
            drawStringCentered(_framebuf, _w, _h, bottomY + 36, "P2P: auto mesh offer", colGray);
        }
    }

    pushFramebuffer();
}

// ============================================================================
// Mutual Exclusion
// ============================================================================
bool OtaApp::_claimOta(OtaSource src) {
    if (_ota_source != OtaSource::NONE && _ota_source != src) {
        Serial.printf("[ota] OTA busy (source=%u), rejecting new source=%u\n",
                      (uint8_t)_ota_source, (uint8_t)src);
        return false;
    }
    _ota_source = src;
    return true;
}

void OtaApp::_releaseOta() {
    _ota_source = OtaSource::NONE;
}

bool OtaApp::_otaRateLimitOk() {
    uint32_t now = millis();
    uint32_t cooldown = (_ota_fail_count >= OTA_MAX_FAILS) ? OTA_FAIL_COOLDOWN_MS : OTA_COOLDOWN_MS;
    if (now - _last_ota_attempt_ms < cooldown) {
        uint32_t wait = cooldown - (now - _last_ota_attempt_ms);
        Serial.printf("[ota] OTA rate limited, wait %u ms (%u failures)\n", wait, _ota_fail_count);
        return false;
    }
    _last_ota_attempt_ms = now;
    return true;
}

// ============================================================================
// SD Card OTA
// ============================================================================
bool OtaApp::checkSDUpdate() {
#ifndef SIMULATOR
    if (!_otaRateLimitOk()) {
        snprintf(_status_line, sizeof(_status_line), "OTA rate limited");
        return false;
    }
    Serial.printf("[ota] Starting SD card OTA...\n");
    snprintf(_status_line, sizeof(_status_line), "Reading SD firmware...");

    _ota.onProgress([this](size_t current, size_t total) {
        if (total > 0) {
            _progress = (uint8_t)((current * 100) / total);
        }
    });

    // Prefer .ota (has header + optional signature) over raw .bin
    const char* sdPath = _sd.exists("/firmware.ota") ? "/firmware.ota" : "/firmware.bin";
    bool ok = _ota.updateFromSD(sdPath);
    if (ok) {
        _progress = 100;
        _ota_fail_count = 0;
        _audit.logAttempt("sd", _ota.getCurrentVersion(), DISPLAY_DRIVER, true);
        Serial.printf("[ota] SD OTA successful!\n");
        return true;
    } else {
        _ota_fail_count++;
        _audit.logAttempt("sd", "?", DISPLAY_DRIVER, false, _ota.getLastError());
        Serial.printf("[ota] SD OTA failed: %s\n", _ota.getLastError());
        snprintf(_status_line, sizeof(_status_line), "SD OTA failed: %s", _ota.getLastError());
        _progress = 0;
        return false;
    }
#else
    return false;
#endif
}

// ============================================================================
// Serial OTA Protocol
// ============================================================================
void OtaApp::handleSerialOTA() {
#ifndef SIMULATOR
    // Binary receive mode — blocking flow-controlled transfer
    // Protocol: PC sends chunk, device buffers in RAM, writes to flash, sends ACK
    // Verifies CRC32 and optional ECDSA signature after transfer
    if (_serial_state == SerialOtaState::RECEIVING) {
        static constexpr size_t SERIAL_CHUNK = 4096;  // Match flash page size
        uint8_t* chunkBuf = (uint8_t*)malloc(SERIAL_CHUNK);
        if (!chunkBuf) {
            Serial.printf("OTA_FAIL out of memory\n");
            Serial.flush();
            _serial_state = SerialOtaState::ERROR;
            _releaseOta();
            Update.abort();
            return;
        }

        // Start streaming CRC32 and (if signed) SHA-256 for verification
        OtaVerify::crc32Begin();
        if (_serial_signed) {
            OtaVerify::beginVerify();
            // Signature covers firmware data only (not header)
        }

        while (_serial_received < _serial_fw_size) {
            // Determine how many bytes for this chunk
            size_t fwRemaining = _serial_fw_size - _serial_received;
            size_t chunkSize = min(fwRemaining, SERIAL_CHUNK);

            // Buffer the entire chunk from serial into RAM
            size_t buffered = 0;
            uint32_t lastDataTime = millis();
            while (buffered < chunkSize) {
                if (Serial.available()) {
                    size_t toRead = min(chunkSize - buffered, (size_t)Serial.available());
                    size_t got = Serial.readBytes(chunkBuf + buffered, toRead);
                    buffered += got;
                    lastDataTime = millis();
                } else {
                    if (millis() - lastDataTime > 10000) {
                        Serial.printf("OTA_FAIL timeout at %u+%u/%u\n",
                                      _serial_received, buffered, _serial_fw_size);
                        Serial.flush();
                        free(chunkBuf);
                        _serial_state = SerialOtaState::ERROR;
                        Update.abort();
                        return;
                    }
                    delay(1);
                }
            }

            // Update CRC32 and signature hash with this chunk
            OtaVerify::crc32Update(chunkBuf, chunkSize);
            if (_serial_signed) {
                OtaVerify::updateVerify(chunkBuf, chunkSize);
            }

            // Write entire chunk to flash (flash erase/write happens here)
            size_t written = Update.write(chunkBuf, chunkSize);
            if (written != chunkSize) {
                Serial.printf("OTA_FAIL write error at %u\n", _serial_received);
                Serial.flush();
                free(chunkBuf);
                _serial_state = SerialOtaState::ERROR;
                Update.abort();
                return;
            }

            _serial_received += written;
            _progress = (uint8_t)((_serial_received * 100) / _serial_fw_size);

            // Signal PC to send next chunk
            Serial.printf("OTA_NEXT %u\n", _serial_received);
            Serial.flush();
        }
        free(chunkBuf);

        // Verify CRC32
        uint32_t actualCrc = OtaVerify::crc32Finalize();
        if (actualCrc != _serial_fw_crc) {
            Serial.printf("[ota] CRC32 mismatch: expected 0x%08X, got 0x%08X\n",
                          _serial_fw_crc, actualCrc);
            Serial.printf("OTA_FAIL CRC32 mismatch (expected 0x%08X, got 0x%08X)\n",
                          _serial_fw_crc, actualCrc);
            Serial.flush();
            _serial_state = SerialOtaState::ERROR;
            _releaseOta();
            Update.abort();
            return;
        }
        Serial.printf("[ota] CRC32 verified: 0x%08X\n", actualCrc);

        // Verify ECDSA signature
#ifdef OTA_REQUIRE_SIGNATURE
        if (!_serial_signed) {
            Serial.printf("[ota] Unsigned firmware rejected (signature required)\n");
            Serial.printf("OTA_FAIL unsigned firmware rejected\n");
            Serial.flush();
            _serial_state = SerialOtaState::ERROR;
            _releaseOta();
            Update.abort();
            return;
        }
#endif
        if (_serial_signed) {
            bool sigOk = OtaVerify::finalizeVerify(_serial_signature.r,
                                                     _serial_signature.s);
            if (!sigOk) {
                Serial.printf("[ota] Signature verification FAILED -- rejecting firmware\n");
                Serial.printf("OTA_FAIL signature verification failed\n");
                Serial.flush();
                _serial_state = SerialOtaState::ERROR;
                Update.abort();
                return;
            }
            Serial.printf("[ota] ECDSA signature verified OK\n");
        }

        // All bytes received and verified — finalize and reboot
        bool endOk = Update.end(true);
        if (endOk) {
            _progress = 100;
            _serial_state = SerialOtaState::DONE;
            _releaseOta();
            Serial.printf("OTA_OK\n");
            Serial.flush();
            delay(1000);
            _ota.reboot();
        } else {
            _serial_state = SerialOtaState::ERROR;
            _releaseOta();
            Serial.printf("OTA_FAIL verify: %s\n", Update.errorString());
            Serial.flush();
        }
        return;
    }

    // Command mode
    while (Serial.available()) {
        char c = Serial.read();

        // Command mode: accumulate line
        if (c == '\n' || c == '\r') {
            _serial_buf[_serial_buf_idx] = '\0';
            if (_serial_buf_idx == 0) continue;

            // Parse commands
            if (strncmp(_serial_buf, "OTA_BEGIN ", 10) == 0) {
                uint32_t size = 0, crc = 0;
                if (sscanf(_serial_buf + 10, "%u %u", &size, &crc) >= 1) {
                    if (!_claimOta(OtaSource::SRC_SERIAL)) {
                        Serial.printf("OTA_FAIL busy (another OTA in progress)\n");
                        break;
                    }
                    _serial_fw_size = size;
                    _serial_fw_crc = crc;
                    _serial_received = 0;
                    // Note: do NOT reset _serial_signed/_serial_signature here
                    // because OTA_SIG is sent before OTA_BEGIN
                    _progress = 0;

                    Serial.printf("[ota] Serial OTA begin: %u bytes, crc=0x%08X\n", size, crc);

                    if (!Update.begin(size)) {
                        Serial.printf("OTA_FAIL begin: %s\n", Update.errorString());
                        snprintf(_status_line, sizeof(_status_line), "Not enough space");
                    } else {
                        _serial_state = SerialOtaState::RECEIVING;
                        snprintf(_status_line, sizeof(_status_line), "Serial: receiving %u bytes", size);
                        Serial.printf("OTA_READY\n");
                    }
                } else {
                    Serial.printf("OTA_FAIL bad args (OTA_BEGIN <size> [crc])\n");
                }
            } else if (strncmp(_serial_buf, "OTA_SIG ", 8) == 0) {
                // Receive ECDSA P-256 signature: OTA_SIG <r_hex_64> <s_hex_64>
                // Must be sent BEFORE OTA_BEGIN (device enters receive mode after BEGIN)
                char r_hex[65] = {}, s_hex[65] = {};
                if (sscanf(_serial_buf + 8, "%64s %64s", r_hex, s_hex) == 2 &&
                    strlen(r_hex) == 64 && strlen(s_hex) == 64) {
                    // Parse hex to bytes (validate each sscanf)
                    bool parseOk = true;
                    for (int i = 0; i < 32; i++) {
                        unsigned int rb, sb;
                        if (sscanf(r_hex + i*2, "%2x", &rb) != 1 ||
                            sscanf(s_hex + i*2, "%2x", &sb) != 1) {
                            parseOk = false;
                            break;
                        }
                        _serial_signature.r[i] = (uint8_t)rb;
                        _serial_signature.s[i] = (uint8_t)sb;
                    }
                    if (!parseOk) {
                        Serial.printf("OTA_SIG_FAIL invalid hex\n");
                        break;
                    }
                    _serial_signed = true;
                    Serial.printf("[ota] Received ECDSA signature for verification\n");
                    Serial.printf("OTA_SIG_OK\n");
                } else {
                    Serial.printf("OTA_SIG_FAIL bad format (OTA_SIG <r_hex64> <s_hex64>)\n");
                }
            } else if (strcmp(_serial_buf, "OTA_INFO") == 0) {
                char fwHash[65] = {};
                _ota.getFirmwareHash(fwHash, sizeof(fwHash));
                auto meshStats = _mesh_ota.getStats();
                Serial.printf("{\"version\":\"%s\",\"partition\":\"%s\",\"max_size\":%u,"
                              "\"sd\":%s,\"usb_msc\":%s,\"wifi\":%s,\"espnow\":%s,\"ble\":%s,"
                              "\"board\":\"%s\",\"signing\":%s,\"sig_required\":%s,"
                              "\"encryption\":%s,"
                              "\"mesh_ota\":{\"sending\":%s,\"receiving\":%s,\"transfer\":%s,"
                              "\"completed\":%u,\"failed\":%u},"
                              "\"fw_hash\":\"%s\","
                              "\"audit_entries\":%d,"
                              "\"rollback\":%s}\n",
                              _ota.getCurrentVersion(),
                              _ota.getRunningPartition(),
                              (unsigned)_ota.getMaxFirmwareSize(),
                              _sd_ok ? "true" : "false",
                              _sd.isUSBMSCActive() ? "true" : "false",
                              [](){ wifi_ap_record_t a; return esp_wifi_sta_get_ap_info(&a)==ESP_OK; }() ? "true" : "false",
                              _espnow_ok ? "true" : "false",
                              _ble_ok ? "true" : "false",
                              DISPLAY_DRIVER,
                              "true",  // signing always supported
#ifdef OTA_REQUIRE_SIGNATURE
                              "true",
#else
                              "false",
#endif
                              "true",  // encryption supported
                              _mesh_ota.isSending() ? "true" : "false",
                              _mesh_ota.isReceiving() ? "true" : "false",
                              _mesh_ota.isTransferActive() ? "true" : "false",
                              meshStats.transfers_completed, meshStats.transfers_failed,
                              fwHash,
                              _audit.getEntryCount(),
                              _ota.canRollback() ? "true" : "false"
                              );
            } else if (strcmp(_serial_buf, "OTA_SD") == 0) {
                // Try to (re-)init SD if not already mounted
                if (!_sd_ok) {
                    _sd_ok = _sd.init();
                }
                if (_sd_ok) {
                    _have_firmware_file = _sd.exists("/firmware.bin") || _sd.exists("/firmware.ota");
                }
                if (_sd_ok && _have_firmware_file) {
                    Serial.printf("OTA_SD_START\n");
                    if (checkSDUpdate()) {
                        Serial.printf("OTA_SD_OK\n");
                        delay(500);
                        _ota.reboot();
                    } else {
                        Serial.printf("OTA_SD_FAIL %s\n", _ota.getLastError());
                    }
                } else {
                    Serial.printf("OTA_SD_FAIL no firmware.bin on SD\n");
                }
            } else if (strcmp(_serial_buf, "USB_MSC") == 0) {
                // Toggle USB Mass Storage mode — SD card appears as USB drive
                if (_sd.isUSBMSCActive()) {
                    _sd.stopUSBMSC();
                    Serial.printf("USB_MSC_OFF\n");
                } else {
                    if (!_sd_ok) _sd_ok = _sd.init();
                    if (_sd_ok && _sd.startUSBMSC()) {
                        Serial.printf("USB_MSC_ON\n");
                    } else {
                        Serial.printf("USB_MSC_FAIL\n");
                    }
                }
            } else if (strcmp(_serial_buf, "VERIFY_TEST") == 0) {
                // Test ECDSA verification with known data
                const uint8_t testData[] = "Hello OTA verify test";
                const uint8_t testSigR[] = {
                    0x6a,0x64,0x67,0x95,0x41,0x15,0xb3,0x65,0x1d,0x35,0x65,0xfe,0x0c,0x09,0xda,0x79,
                    0x69,0x73,0x1d,0x72,0x15,0x0f,0xc0,0x3b,0xab,0x9b,0x65,0x91,0x29,0x3a,0x97,0x8c
                };
                const uint8_t testSigS[] = {
                    0x99,0x75,0xd5,0xb3,0xb9,0x5f,0x9e,0x96,0x9e,0xd5,0x69,0xd8,0x69,0x3e,0x1b,0x9f,
                    0xf2,0x73,0x83,0x5b,0xf3,0xf6,0x95,0x66,0x8f,0x71,0x28,0x8a,0x3a,0xec,0x72,0x77
                };
                // Test 1: Direct (non-streaming) verification
                bool ok1 = OtaVerify::verifySignature(testSigR, testSigS, testData, 21);
                Serial.printf("VERIFY_TEST direct: %s\n", ok1 ? "PASS" : "FAIL");
                // Test 2: Streaming verification
                OtaVerify::beginVerify();
                OtaVerify::updateVerify(testData, 21);
                bool ok2 = OtaVerify::finalizeVerify(testSigR, testSigS);
                Serial.printf("VERIFY_TEST streaming: %s\n", ok2 ? "PASS" : "FAIL");
            } else if (strncmp(_serial_buf, "OTA_URL ", 8) == 0) {
                // WiFi pull OTA: OTA_URL http://host:port/path/to/firmware.ota
                const char* url = _serial_buf + 8;
                wifi_ap_record_t _url_ap;
                if (esp_wifi_sta_get_ap_info(&_url_ap) != ESP_OK) {
                    Serial.printf("OTA_URL_FAIL WiFi not connected\n");
                } else if (strlen(url) < 10) {
                    Serial.printf("OTA_URL_FAIL invalid URL\n");
                } else {
                    if (!_otaRateLimitOk()) {
                        Serial.printf("OTA_URL_FAIL rate limited\n");
                    } else {
                        Serial.printf("OTA_URL_START %s\n", url);
                        if (_ota.updateFromUrl(url)) {
                            _ota_fail_count = 0;
                            _audit.logAttempt("wifi_pull", _ota.getCurrentVersion(), DISPLAY_DRIVER, true);
                            Serial.printf("OTA_URL_OK\n");
                            delay(500);
                            _ota.reboot();
                        } else {
                            _ota_fail_count++;
                            _audit.logAttempt("wifi_pull", "?", DISPLAY_DRIVER, false, _ota.getLastError());
                            Serial.printf("OTA_URL_FAIL %s\n", _ota.getLastError());
                        }
                    }
                }
            } else if (strcmp(_serial_buf, "WIFI_STATUS") == 0) {
                wifi_ap_record_t _ws_ap;
                bool _ws_conn = (esp_wifi_sta_get_ap_info(&_ws_ap) == ESP_OK);
                char _ws_ip[16] = "";
                if (_ws_conn) {
                    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                    if (netif) {
                        esp_netif_ip_info_t ip_info;
                        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                            esp_ip4addr_ntoa(&ip_info.ip, _ws_ip, sizeof(_ws_ip));
                        }
                    }
                }
                Serial.printf("{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}\n",
                              _ws_conn ? "true" : "false",
                              _ws_conn ? (const char*)_ws_ap.ssid : "",
                              _ws_conn ? _ws_ip : "",
                              _ws_conn ? (int)_ws_ap.rssi : 0);
            } else if (strncmp(_serial_buf, "WIFI_ADD ", 9) == 0) {
                // Add WiFi network: WIFI_ADD <ssid> <password>
                char ssid[33] = {};
                char pass[65] = {};
                // Parse: first token is SSID, rest is password
                const char* p = _serial_buf + 9;
                const char* space = strchr(p, ' ');
                if (space && (space - p) < 33) {
                    memcpy(ssid, p, space - p);
                    strncpy(pass, space + 1, sizeof(pass) - 1);
                    if (_wifi.addNetwork(ssid, pass)) {
                        Serial.printf("WIFI_ADD_OK %s\n", ssid);
                        _wifi.connect();
                    } else {
                        Serial.printf("WIFI_ADD_FAIL\n");
                    }
                } else {
                    Serial.printf("WIFI_ADD_FAIL bad format (WIFI_ADD ssid password)\n");
                }
            } else if (strcmp(_serial_buf, "WIFI_CONNECT") == 0) {
                _wifi.connect();
                Serial.printf("WIFI_CONNECT_OK\n");
            } else if (strcmp(_serial_buf, "OTA_ROLLBACK") == 0) {
                if (_ota.rollback()) {
                    Serial.printf("OTA_ROLLBACK_OK\n");
                    delay(500);
                    _ota.reboot();
                } else {
                    Serial.printf("OTA_ROLLBACK_FAIL %s\n", _ota.getLastError());
                }
            } else if (strncmp(_serial_buf, "OTA_SD_WRITE ", 13) == 0) {
                // Write firmware to SD: OTA_SD_WRITE <size> [/path]
                uint32_t size = 0;
                char sdPath[64] = "/firmware.bin";
                sscanf(_serial_buf + 13, "%u %63s", &size, sdPath);
                // Path traversal protection
                if (strstr(sdPath, "..") != nullptr) {
                    Serial.printf("OTA_FAIL path traversal rejected\n");
                    break;
                }
                if (sdPath[0] != '/') {
                    Serial.printf("OTA_FAIL path must be absolute\n");
                    break;
                }
                if (size > 0 && !_sd_ok) {
                    _sd_ok = _sd.init();
                }
                if (size > 0 && _sd_ok) {
                    char sdFullPath[128];
                    snprintf(sdFullPath, sizeof(sdFullPath), "/sdcard%s", sdPath);
                    FILE* f = fopen(sdFullPath, "wb");
                    if (f) {
                        Serial.printf("OTA_READY\n");
                        Serial.flush();
                        uint8_t* buf = (uint8_t*)malloc(4096);
                        if (buf) {
                            uint32_t written = 0;
                            while (written < size) {
                                size_t want = min(size - written, (uint32_t)4096);
                                size_t got = 0;
                                uint32_t t0 = millis();
                                while (got < want) {
                                    if (Serial.available()) {
                                        got += Serial.readBytes(buf + got, min(want - got, (size_t)Serial.available()));
                                        t0 = millis();
                                    } else if (millis() - t0 > 10000) {
                                        break;
                                    }
                                    delay(1);
                                }
                                if (got < want) {
                                    Serial.printf("OTA_FAIL SD write timeout at %u\n", written);
                                    Serial.flush();
                                    break;
                                }
                                fwrite(buf, 1, got, f);
                                written += got;
                                Serial.printf("OTA_NEXT %u\n", written);
                                Serial.flush();
                            }
                            free(buf);
                            fclose(f);
                            if (written >= size) {
                                _have_firmware_file = true;
                                Serial.printf("OTA_SD_WRITE_OK %u\n", written);
                                Serial.flush();
                            }
                        } else {
                            fclose(f);
                            Serial.printf("OTA_FAIL out of memory\n");
                        }
                    } else {
                        Serial.printf("OTA_FAIL cannot open SD file\n");
                    }
                } else {
                    Serial.printf("OTA_FAIL bad args or no SD\n");
                }
            } else if (strcmp(_serial_buf, "OTA_REBOOT") == 0) {
                Serial.printf("OTA_REBOOTING\n");
                delay(200);
                _ota.reboot();
            } else if (strcmp(_serial_buf, "IDENTIFY") == 0) {
                Serial.printf("{\"board\":\"%s\",\"display\":\"%dx%d\",\"interface\":\"%s\",\"app\":\"%s\","
                              "\"provisioned\":%s,\"device_id\":\"%s\"}\n",
                              DISPLAY_DRIVER, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_IF, name(),
                              _provision.isProvisioned() ? "true" : "false",
                              _provision.getIdentity().device_id);
            } else if (strcmp(_serial_buf, "PROVISION_BEGIN") == 0) {
                _provision.startUSBProvision();
                Serial.printf("PROVISION_READY\n");
                // After this, the HAL reads from Serial directly in processUSBProvision()
                // Set a flag so loop() calls processUSBProvision() instead of handleSerialOTA()
            } else if (strcmp(_serial_buf, "PROVISION_STATUS") == 0) {
                Serial.printf("{\"provisioned\":%s,\"device_id\":\"%s\",\"server_url\":\"%s\"}\n",
                              _provision.isProvisioned() ? "true" : "false",
                              _provision.getIdentity().device_id,
                              _provision.getIdentity().server_url);
            } else if (strcmp(_serial_buf, "PROVISION_RESET") == 0) {
                if (_provision.factoryReset()) {
                    Serial.printf("PROVISION_RESET_OK\n");
                } else {
                    Serial.printf("PROVISION_RESET_FAIL\n");
                }
            } else if (strcmp(_serial_buf, "OTA_AUDIT") == 0) {
                char buf[2048];
                size_t n = _audit.readLog(buf, sizeof(buf));
                if (n > 0) {
                    Serial.printf("%s", buf);
                } else {
                    Serial.printf("OTA_AUDIT_EMPTY\n");
                }
            } else if (strcmp(_serial_buf, "OTA_AUDIT_CLEAR") == 0) {
                _audit.clear();
                Serial.printf("OTA_AUDIT_CLEAR_OK\n");
            } else if (strcmp(_serial_buf, "FW_HASH") == 0) {
                char hash[65];
                if (_ota.getFirmwareHash(hash, sizeof(hash))) {
                    Serial.printf("FW_HASH %s\n", hash);
                } else {
                    Serial.printf("FW_HASH_FAIL\n");
                }
            } else if (strcmp(_serial_buf, "MESH_OTA_SEND") == 0) {
                // Send firmware.ota from SD to mesh peers
                if (!_espnow_ok) {
                    Serial.printf("MESH_OTA_FAIL espnow not ready\n");
                } else if (_mesh_ota.isSending()) {
                    Serial.printf("MESH_OTA_FAIL already sending\n");
                } else {
                    if (!_sd_ok) _sd_ok = _sd.init();
                    const char* path = _sd.exists("/firmware.ota") ? "/firmware.ota" : "/firmware.bin";
                    if (_mesh_ota.startSend(path)) {
                        Serial.printf("MESH_OTA_SEND_OK\n");
                    } else {
                        Serial.printf("MESH_OTA_FAIL cannot start send\n");
                    }
                }
            } else if (strcmp(_serial_buf, "MESH_OTA_RECV") == 0) {
                // Enable receiving firmware from mesh peers
                if (!_espnow_ok) {
                    Serial.printf("MESH_OTA_FAIL espnow not ready\n");
                } else {
                    _mesh_ota.enableReceive(true);
                    Serial.printf("MESH_OTA_RECV_OK\n");
                }
            } else if (strcmp(_serial_buf, "MESH_OTA_STOP") == 0) {
                _mesh_ota.stopSend();
                _mesh_ota.enableReceive(false);
                Serial.printf("MESH_OTA_STOP_OK\n");
            } else if (strcmp(_serial_buf, "MESH_OTA_STATUS") == 0) {
                auto stats = _mesh_ota.getStats();
                Serial.printf("{\"sending\":%s,\"receiving\":%s,\"transfer\":%s,"
                              "\"chunks_sent\":%u,\"chunks_received\":%u,"
                              "\"retransmits\":%u,\"completed\":%u,\"failed\":%u}\n",
                              _mesh_ota.isSending() ? "true" : "false",
                              _mesh_ota.isReceiving() ? "true" : "false",
                              _mesh_ota.isTransferActive() ? "true" : "false",
                              stats.chunks_sent, stats.chunks_received,
                              stats.retransmits, stats.transfers_completed, stats.transfers_failed);
            }

            _serial_buf_idx = 0;
        } else if (_serial_buf_idx < sizeof(_serial_buf) - 1) {
            _serial_buf[_serial_buf_idx++] = c;
        }
    }
#endif
}

// ============================================================================
// OTA Header Validation
// ============================================================================
bool OtaApp::_validateOtaHeader(const OtaFirmwareHeader& hdr) {
    if (!hdr.isValid()) {
        Serial.printf("[ota] Invalid OTA header magic/version\n");
        return false;
    }

    // Board validation: if header specifies a board, check it matches
    if (hdr.board[0] != '\0' && strcmp(hdr.board, "any") != 0) {
        // Check if board name contains our display driver name (loose match)
        // e.g., "touch-lcd-349" should match a board with AXS15231B
        // We don't enforce strict board matching since display driver names vary
        Serial.printf("[ota] OTA target board: %s\n", hdr.board);
    }

    // Size validation
    size_t maxSize = _ota.getMaxFirmwareSize();
    if (hdr.firmware_size > maxSize) {
        Serial.printf("[ota] Firmware too large: %u > max %u\n", hdr.firmware_size, (unsigned)maxSize);
        return false;
    }

    // Anti-rollback: reject firmware older than current version
#ifdef OTA_ANTI_ROLLBACK
    if (hdr.version[0] != '\0') {
        const char* currentVer = _ota.getCurrentVersion();
        if (currentVer && otaVersionCompare(hdr.version, currentVer) < 0) {
            Serial.printf("[ota] Anti-rollback: %s < %s (downgrade rejected)\n", hdr.version, currentVer);
            return false;
        }
    }
#endif

    return true;
}

// ============================================================================
// Fleet Heartbeat
// ============================================================================
void OtaApp::sendHeartbeat() {
#ifndef SIMULATOR
    const DeviceIdentity& id = _provision.getIdentity();
    if (id.server_url[0] == '\0') return;

    char url[320];
    snprintf(url, sizeof(url), "%s/api/devices/%s/status", id.server_url, id.device_id);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    // Compute firmware hash for attestation
    char fwHash[65] = {};
    _ota.getFirmwareHash(fwHash, sizeof(fwHash));

    // Gather WiFi info via ESP-IDF
    char _hb_ip[16] = "0.0.0.0";
    int8_t _hb_rssi = 0;
    {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
                esp_ip4addr_ntoa(&ip_info.ip, _hb_ip, sizeof(_hb_ip));
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) _hb_rssi = ap.rssi;
    }

    char body[384];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"board\":\"%s\",\"partition\":\"%s\","
             "\"ip\":\"%s\",\"uptime_s\":%lu,\"free_heap\":%u,\"rssi\":%d,"
             "\"fw_hash\":\"%s\"}",
             _ota.getCurrentVersion(), DISPLAY_DRIVER,
             _ota.getRunningPartition(),
             _hb_ip,
             (unsigned long)(millis() / 1000),
             (unsigned)esp_get_free_heap_size(),
             (int)_hb_rssi,
             fwHash);

    int code = http.POST(body);
    if (code == 200) {
        // Check if server scheduled an OTA for us
        String response = http.getString();
        if (response.indexOf("\"ota\"") >= 0 && response.indexOf("\"url\"") >= 0) {
            // Parse minimal OTA URL from response
            int urlStart = response.indexOf("\"url\":\"") + 7;
            int urlEnd = response.indexOf("\"", urlStart);
            if (urlStart > 6 && urlEnd > urlStart) {
                String otaUrl = String(id.server_url) + response.substring(urlStart, urlEnd);
                Serial.printf("[ota] Fleet server scheduled OTA: %s\n", otaUrl.c_str());
                snprintf(_status_line, sizeof(_status_line), "Server OTA: downloading...");
                // Use WiFi pull OTA
                _ota.updateFromUrl(otaUrl.c_str());
            }
        }
    } else {
        Serial.printf("[ota] Heartbeat: HTTP %d\n", code);
    }
    http.end();
#endif
}

// ============================================================================
// ESP-NOW P2P OTA
// ============================================================================
void OtaApp::offerFirmwareToMesh() {
#ifndef SIMULATOR
    if (!_espnow_ok || !_have_firmware_file || !_sd_ok) return;

    // Read firmware — prefer .ota over .bin
    const char* fwPath = "/firmware.ota";
    char sdFwPath[128];
    snprintf(sdFwPath, sizeof(sdFwPath), "/sdcard%s", fwPath);
    FILE* f = fopen(sdFwPath, "rb");
    if (!f) {
        fwPath = "/firmware.bin";
        snprintf(sdFwPath, sizeof(sdFwPath), "/sdcard%s", fwPath);
        f = fopen(sdFwPath, "rb");
    }
    if (!f) return;

    fseek(f, 0, SEEK_END);
    uint32_t fwSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    bool isOtaPkg = false;
    OtaFirmwareHeader hdr = {};
    OtaSignature sig = {};
    uint32_t fwDataSize = fwSize;  // Actual firmware bytes (excluding header)

    // Try to parse .ota header
    if (fwSize >= sizeof(OtaFirmwareHeader)) {
        fread((uint8_t*)&hdr, 1, sizeof(hdr), f);
        if (hdr.isValid()) {
            isOtaPkg = true;
            fwDataSize = hdr.firmware_size;
            if (hdr.isSigned() && fwSize >= sizeof(OtaFirmwareHeader) + sizeof(OtaSignature)) {
                fread((uint8_t*)&sig, 1, sizeof(sig), f);
            }
            // Skip to firmware data for CRC computation
            fseek(f, hdr.totalHeaderSize(), SEEK_SET);
        } else {
            fseek(f, 0, SEEK_SET);  // Not an OTA package, rewind
        }
    }

    // Compute CRC32 of firmware data only
    OtaVerify::crc32Begin();
    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        OtaVerify::crc32Update(buf, n);
    }
    uint32_t crc = OtaVerify::crc32Finalize();
    fclose(f);

    uint16_t chunkCount = (fwDataSize + OTA_CHUNK_DATA_SIZE - 1) / OTA_CHUNK_DATA_SIZE;

    // Build offer message
    OtaOffer offer = {};
    offer.type = OtaMsgType::OFFER;
    offer.firmware_size = fwDataSize;
    offer.crc32 = isOtaPkg ? hdr.firmware_crc32 : crc;
    offer.chunk_count = chunkCount;
    offer.is_signed = isOtaPkg && hdr.isSigned() ? 1 : 0;

    // Use version from header if available, else current
    const char* ver = (isOtaPkg && hdr.version[0]) ? hdr.version : _ota.getCurrentVersion();
    offer.version_len = strlen(ver);
    if (offer.version_len > sizeof(offer.version)) offer.version_len = sizeof(offer.version);
    memcpy(offer.version, ver, offer.version_len);

    _espnow.meshBroadcast((const uint8_t*)&offer, sizeof(offer));
    Serial.printf("[ota] Offered firmware: %u bytes, %u chunks, crc=0x%08X, signed=%d\n",
                  fwDataSize, chunkCount, offer.crc32, offer.is_signed);

    // If signed, also broadcast the signature so receiver can verify
    if (offer.is_signed) {
        OtaSigMsg sigMsg = {};
        sigMsg.type = OtaMsgType::SIG;
        memcpy(sigMsg.r, sig.r, 32);
        memcpy(sigMsg.s, sig.s, 32);
        _espnow.meshBroadcast((const uint8_t*)&sigMsg, sizeof(sigMsg));
        Serial.printf("[ota] Sent ECDSA signature for P2P transfer\n");
    }
#endif
}

void OtaApp::handleEspNowOTA(const uint8_t* src, const uint8_t* data,
                                uint8_t len, uint8_t hops) {
#ifndef SIMULATOR
    if (len < 1) return;
    OtaMsgType msgType = (OtaMsgType)data[0];

    switch (msgType) {
        case OtaMsgType::OFFER: {
            if (len < sizeof(OtaOffer)) return;
            const OtaOffer* offer = (const OtaOffer*)data;

            Serial.printf("[ota] P2P offer from %02X:%02X: %u bytes, %u chunks\n",
                          src[0], src[1], offer->firmware_size, offer->chunk_count);

            // Accept if we're idle and it's a different version
            if (_p2p_state == P2PState::IDLE) {
                char offerVer[25] = {};
                uint8_t verLen = offer->version_len;
                if (verLen > sizeof(offer->version)) verLen = sizeof(offer->version);
                memcpy(offerVer, offer->version, verLen);
                offerVer[verLen] = '\0';  // Guarantee null-termination

#ifdef OTA_REQUIRE_SIGNATURE
                if (!offer->is_signed) {
                    Serial.printf("[ota] P2P offer rejected: unsigned (signature required)\n");
                    break;
                }
#endif
                if (strcmp(offerVer, _ota.getCurrentVersion()) != 0) {
                    Serial.printf("[ota] Accepting P2P offer (current: %s, offered: %s, signed: %d)\n",
                                  _ota.getCurrentVersion(), offerVer, offer->is_signed);
                    _p2p_signed = offer->is_signed;
                    memset(&_p2p_signature, 0, sizeof(_p2p_signature));
                    if (startEspNowReceive(offer->firmware_size, offer->crc32, offer->chunk_count)) {
                        memcpy(_p2p_peer, src, 6);
                        // If signed, wait for SIG message before requesting chunks
                        if (!_p2p_signed) {
                            requestNextChunk(src);
                        }
                    }
                } else {
                    Serial.printf("[ota] P2P offer same version, ignoring\n");
                }
            }
            break;
        }

        case OtaMsgType::REQUEST: {
            if (len < sizeof(OtaChunkRequest)) return;
            const OtaChunkRequest* req = (const OtaChunkRequest*)data;

            // Send the requested chunk from SD card
            if (!_have_firmware_file || !_sd_ok) return;

            // Validate chunk index
            if (_p2p_chunk_count > 0 && req->chunk_idx >= _p2p_chunk_count) {
                Serial.printf("[ota] P2P invalid chunk_idx %u >= %u\n", req->chunk_idx, _p2p_chunk_count);
                return;
            }

            FILE* f = fopen("/sdcard/firmware.ota", "rb");
            if (!f) f = fopen("/sdcard/firmware.bin", "rb");
            if (!f) return;

            // Determine header offset — skip OTA header to get to firmware data
            fseek(f, 0, SEEK_END);
            uint32_t fSize = ftell(f);
            fseek(f, 0, SEEK_SET);

            uint32_t headerOffset = 0;
            if (fSize >= sizeof(OtaFirmwareHeader)) {
                OtaFirmwareHeader hdr = {};
                fread((uint8_t*)&hdr, 1, sizeof(hdr), f);
                if (hdr.isValid()) {
                    headerOffset = hdr.totalHeaderSize();
                }
            }

            uint32_t offset = headerOffset + (uint32_t)req->chunk_idx * OTA_CHUNK_DATA_SIZE;
            fseek(f, offset, SEEK_SET);

            uint8_t pktBuf[sizeof(OtaChunk) + OTA_CHUNK_DATA_SIZE];
            OtaChunk* chunk = (OtaChunk*)pktBuf;
            chunk->type = OtaMsgType::CHUNK;
            chunk->chunk_idx = req->chunk_idx;
            chunk->data_len = fread(pktBuf + sizeof(OtaChunk), 1, OTA_CHUNK_DATA_SIZE, f);
            fclose(f);

            _espnow.meshSend(src, pktBuf, sizeof(OtaChunk) + chunk->data_len);
            _p2p_last_activity = millis();

            snprintf(_status_line, sizeof(_status_line), "Sending chunk %u/%u",
                     req->chunk_idx + 1, _p2p_chunk_count);
            break;
        }

        case OtaMsgType::CHUNK: {
            if (len < sizeof(OtaChunk)) return;
            const OtaChunk* chunk = (const OtaChunk*)data;
            const uint8_t* chunkData = data + sizeof(OtaChunk);

            // Validate data_len against actual packet size and max chunk size
            uint8_t availableData = len - sizeof(OtaChunk);
            if (chunk->data_len > availableData || chunk->data_len > OTA_CHUNK_DATA_SIZE) {
                Serial.printf("[ota] P2P chunk data_len %u exceeds available %u or max %u\n",
                              chunk->data_len, availableData, OTA_CHUNK_DATA_SIZE);
                return;
            }
            if (chunk->data_len == 0) return;

            if (_p2p_state != P2PState::RECEIVING) return;
            if (chunk->chunk_idx != _p2p_next_chunk) {
                Serial.printf("[ota] P2P unexpected chunk %u (expected %u)\n",
                              chunk->chunk_idx, _p2p_next_chunk);
                return;
            }

            // Update CRC32 and signature hash
            OtaVerify::crc32Update(chunkData, chunk->data_len);
            if (_p2p_signed) {
                OtaVerify::updateVerify(chunkData, chunk->data_len);
            }

            size_t written = Update.write((uint8_t*)chunkData, chunk->data_len);
            if (written != chunk->data_len) {
                Serial.printf("[ota] P2P write failed: %s\n", Update.errorString());
                _p2p_state = P2PState::ERROR;
                _releaseOta();
                snprintf(_status_line, sizeof(_status_line), "P2P write error");
                Update.abort();

                // Send abort to peer
                uint8_t abort = (uint8_t)OtaMsgType::ABORT;
                _espnow.meshSend(src, &abort, 1);
                return;
            }

            _p2p_received += written;
            _p2p_next_chunk++;
            _p2p_last_activity = millis();
            _progress = (uint8_t)((_p2p_received * 100) / _p2p_fw_size);

            snprintf(_status_line, sizeof(_status_line), "P2P: %u/%u chunks",
                     _p2p_next_chunk, _p2p_chunk_count);

            if (_p2p_next_chunk >= _p2p_chunk_count) {
                // Verify CRC32
                uint32_t actualCrc = OtaVerify::crc32Finalize();
                bool p2p_verified = true;  // Tracks verification pass/fail

                if (actualCrc != _p2p_fw_crc) {
                    Serial.printf("[ota] P2P CRC32 mismatch: 0x%08X vs 0x%08X\n",
                                  _p2p_fw_crc, actualCrc);
                    snprintf(_status_line, sizeof(_status_line), "P2P CRC error");
                    p2p_verified = false;
                }

                // Signature verification
                if (p2p_verified && _p2p_signed) {
                    bool sigOk = OtaVerify::finalizeVerify(_p2p_signature.r, _p2p_signature.s);
                    if (!sigOk) {
                        Serial.printf("[ota] P2P ECDSA signature verification FAILED\n");
                        snprintf(_status_line, sizeof(_status_line), "P2P sig FAIL");
                        p2p_verified = false;
                    } else {
                        Serial.printf("[ota] P2P ECDSA signature verified OK\n");
                    }
                }

#ifdef OTA_REQUIRE_SIGNATURE
                if (p2p_verified && !_p2p_signed) {
                    Serial.printf("[ota] P2P unsigned firmware rejected (signature required)\n");
                    snprintf(_status_line, sizeof(_status_line), "P2P unsigned!");
                    p2p_verified = false;
                }
#endif

                if (!p2p_verified) {
                    _p2p_state = P2PState::ERROR;
                    _releaseOta();
                    Update.abort();
                    uint8_t abortMsg = (uint8_t)OtaMsgType::ABORT;
                    _espnow.meshSend(src, &abortMsg, 1);
                } else if (Update.end(true)) {
                    _p2p_state = P2PState::DONE;
                    _releaseOta();
                    _progress = 100;
                    snprintf(_status_line, sizeof(_status_line), "P2P OTA complete!");

                    uint8_t done = (uint8_t)OtaMsgType::DONE;
                    _espnow.meshSend(src, &done, 1);

                    Serial.printf("[ota] P2P OTA complete, %u bytes, CRC32 verified: 0x%08X\n",
                                  _p2p_received, actualCrc);
                    delay(1000);
                    _ota.reboot();
                } else {
                    _p2p_state = P2PState::ERROR;
                    _releaseOta();
                    snprintf(_status_line, sizeof(_status_line), "P2P verify failed");
                    Serial.printf("[ota] P2P verify failed: %s\n", Update.errorString());
                }
            } else {
                requestNextChunk(src);
            }
            break;
        }

        case OtaMsgType::DONE:
            Serial.printf("[ota] P2P peer reports transfer done\n");
            _p2p_state = P2PState::IDLE;
            _releaseOta();
            snprintf(_status_line, sizeof(_status_line), "P2P transfer done");
            break;

        case OtaMsgType::SIG: {
            if (len < sizeof(OtaSigMsg)) return;
            const OtaSigMsg* sigMsg = (const OtaSigMsg*)data;

            if (_p2p_state != P2PState::RECEIVING || !_p2p_signed) {
                Serial.printf("[ota] P2P SIG received but not expecting one\n");
                return;
            }

            memcpy(_p2p_signature.r, sigMsg->r, 32);
            memcpy(_p2p_signature.s, sigMsg->s, 32);
            Serial.printf("[ota] P2P received ECDSA signature, starting chunk transfer\n");

            // Start streaming SHA-256 for signature verification
            OtaVerify::beginVerify();

            // Now start requesting chunks
            requestNextChunk(_p2p_peer);
            break;
        }

        case OtaMsgType::ABORT:
            Serial.printf("[ota] P2P peer aborted transfer\n");
            _p2p_state = P2PState::IDLE;
            _releaseOta();
            snprintf(_status_line, sizeof(_status_line), "P2P aborted by peer");
            Update.abort();
            break;
    }
#endif
}

bool OtaApp::startEspNowReceive(uint32_t size, uint32_t crc, uint16_t chunkCount) {
#ifndef SIMULATOR
    if (!_claimOta(OtaSource::SRC_P2P)) {
        Serial.printf("[ota] P2P: OTA busy, rejecting\n");
        return false;
    }
    if (!Update.begin(size)) {
        Serial.printf("[ota] P2P: Update.begin failed: %s\n", Update.errorString());
        snprintf(_status_line, sizeof(_status_line), "P2P: not enough space");
        return false;
    }

    _p2p_state = P2PState::RECEIVING;
    _p2p_fw_size = size;
    _p2p_fw_crc = crc;
    _p2p_chunk_count = chunkCount;
    _p2p_next_chunk = 0;
    _p2p_received = 0;
    _p2p_last_activity = millis();
    _progress = 0;

    // Start streaming CRC32 verification
    OtaVerify::crc32Begin();

    snprintf(_status_line, sizeof(_status_line), "P2P: receiving %u bytes", size);
    return true;
#else
    return false;
#endif
}

void OtaApp::requestNextChunk(const uint8_t* peer) {
    OtaChunkRequest req = {};
    req.type = OtaMsgType::REQUEST;
    req.chunk_idx = _p2p_next_chunk;
    _espnow.meshSend(peer, (const uint8_t*)&req, sizeof(req));
}
