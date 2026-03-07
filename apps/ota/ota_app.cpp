#include "ota_app.h"
#include "debug_log.h"
#include <cstring>
#include <cstdio>

#ifndef SIMULATOR
#include <Update.h>
#include <esp_ota_ops.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <HTTPClient.h>
#endif

static constexpr const char* TAG = "ota_app";

// ============================================================================
// Setup
// ============================================================================
void OtaApp::setup(LGFX& display) {
    // Create display canvas
    _canvas = new LGFX_Sprite(&display);
    _canvas->setPsram(true);
    _canvas->createSprite(display.width(), display.height());
    _canvas->setTextSize(2);

    // Init OTA HAL
    _ota_ok = _ota.init();
    DBG_INFO(TAG, "OTA HAL: %s (v%s, partition: %s, max: %u bytes)",
             _ota_ok ? "OK" : "FAIL", _ota.getCurrentVersion(),
             _ota.getRunningPartition(), _ota.getMaxFirmwareSize());

    // Delay app confirmation — gives the watchdog time to detect boot loops.
    // If the app crashes within the first 30s, ESP-IDF will auto-rollback
    // to the previous firmware on the next boot.
    // confirmApp() is called later in loop() after the stability window.
    _app_confirm_timer = millis();

    // Init provisioning
    _prov_ok = _provision.init();
    DBG_INFO(TAG, "Provisioning: %s (%s)",
             _prov_ok ? "OK" : "FAIL",
             _provision.isProvisioned() ? "provisioned" : "unprovisioned");

    // Init WiFi (auto-connect using stored credentials from NVS)
    _wifi.init();
    _wifi.connect();  // Connects to best saved network
    _wifi_ok = true;
    DBG_INFO(TAG, "WiFi: init, connecting to saved networks...");

    // Init SD card
    _sd_ok = _sd.init();
    if (_sd_ok) {
        DBG_INFO(TAG, "SD card: %s, %llu MB total",
                 _sd.getFilesystemType(), _sd.totalBytes() / (1024 * 1024));
        _have_firmware_file = _sd.exists("/firmware.bin") || _sd.exists("/firmware.ota");
        if (_have_firmware_file) {
            DBG_INFO(TAG, "Found firmware on SD card (%s)",
                     _sd.exists("/firmware.ota") ? ".ota" : ".bin");
        }
    } else {
        DBG_WARN(TAG, "SD card not available");
    }

    // Init ESP-NOW
    _espnow_ok = _espnow.init(EspNowRole::NODE, 1);
    if (_espnow_ok) {
        uint8_t mac[6];
        _espnow.getMAC(mac);
        DBG_INFO(TAG, "ESP-NOW: OK, MAC=%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        // Register mesh callback for OTA messages
        _espnow.onMeshReceive([this](const uint8_t* src, const uint8_t* data,
                                       uint8_t len, uint8_t hops) {
            handleEspNowOTA(src, data, len, hops);
        });
    } else {
        DBG_WARN(TAG, "ESP-NOW init failed");
    }

    // Check for SD card firmware on startup
    if (_sd_ok && _have_firmware_file) {
        snprintf(_status_line, sizeof(_status_line), "SD firmware found! Flashing...");
        drawStatus(display);
        if (checkSDUpdate()) {
            snprintf(_status_line, sizeof(_status_line), "SD OTA complete! Rebooting...");
            drawStatus(display);
            delay(1000);
            _ota.reboot();
        }
    }

    // Init BLE OTA
    _ble_ok = _ble_ota.init("ESP32-OTA");
    if (_ble_ok) {
        DBG_INFO(TAG, "BLE OTA: advertising");
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
                    DBG_INFO(TAG, "Mesh OTA success, rebooting in 2s...");
                    delay(2000);
                    _ota.reboot();
                } else {
                    snprintf(_status_line, sizeof(_status_line), "Mesh OTA failed");
                }
            });
            // Auto-enable receiving by default
            _mesh_ota.enableReceive(true);
            DBG_INFO(TAG, "Mesh OTA: initialized, receive enabled");
        }
    }

    // If we have firmware.bin on SD, periodically offer it to mesh peers
    if (_sd_ok && _have_firmware_file && _espnow_ok) {
        _offer_timer = millis();
    }

    // Init OTA audit log
    _audit.init();
    DBG_INFO(TAG, "OTA audit: %d entries logged", _audit.getEntryCount());

    snprintf(_status_line, sizeof(_status_line), "Ready. Waiting for OTA...");
    DBG_INFO(TAG, "OTA app ready");
    DBG_INFO(TAG, "  SD OTA: %s", _sd_ok ? "available" : "no SD");
    DBG_INFO(TAG, "  Serial OTA: listening");
    DBG_INFO(TAG, "  BLE OTA: %s", _ble_ok ? "advertising" : "unavailable");
    DBG_INFO(TAG, "  ESP-NOW P2P: %s", _espnow_ok ? "active" : "unavailable");
    DBG_INFO(TAG, "  Partition: %s (max %u bytes)",
             _ota.getRunningPartition(), _ota.getMaxFirmwareSize());

    Serial.println("OTA_READY");
}

// ============================================================================
// Loop
// ============================================================================
void OtaApp::loop(LGFX& display) {
    // If USB provisioning is active, let the provision HAL own Serial
    if (_provision.isUSBProvisionActive()) {
        bool done = _provision.processUSBProvision();
        if (done) {
            DBG_INFO(TAG, "USB provisioning completed, device_id: %s",
                     _provision.getIdentity().device_id);
            snprintf(_status_line, sizeof(_status_line), "Provisioned: %s",
                     _provision.getIdentity().device_id);
        }
        // Still redraw status
        uint32_t now = millis();
        if (now - _status_timer >= 250) {
            _status_timer = now;
            drawStatus(display);
        }
        return;
    }

    // Delayed app confirmation — confirm after stability window
    if (!_app_confirmed && _ota_ok && (millis() - _app_confirm_timer >= APP_CONFIRM_DELAY_MS)) {
        _ota.confirmApp();
        _app_confirmed = true;
        DBG_INFO(TAG, "App confirmed stable after %u ms", APP_CONFIRM_DELAY_MS);
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
                DBG_WARN(TAG, "P2P transfer timed out");
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
    if (_provision.isProvisioned() && WiFi.isConnected()) {
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
        drawStatus(display);
    }
}

// ============================================================================
// Display
// ============================================================================
void OtaApp::drawStatus(LGFX& display) {
    int w = display.width();
    int h = display.height();
    bool narrow = (w < 200);  // 3.49 is 172px wide
    _canvas->fillSprite(TFT_BLACK);
    _canvas->setTextDatum(textdatum_t::top_center);

    // Scale text sizes based on display width
    int titleSize = narrow ? 1 : 2;
    int bodySize = 1;
    int lineH = narrow ? 10 : 14;
    int margin = narrow ? 2 : 8;
    int cx = w / 2;

    // Title
    _canvas->setTextColor(0x07E0);
    _canvas->setTextSize(titleSize);
    int y = margin;
    _canvas->drawString("OTA Update", cx, y);
    y += titleSize * 8 + 4;

    // Partition info
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setTextSize(bodySize);
    char buf[80];

    if (narrow) {
        // Two shorter lines for narrow displays
        snprintf(buf, sizeof(buf), "%s", _ota.getRunningPartition());
        _canvas->drawString(buf, cx, y); y += lineH;
        snprintf(buf, sizeof(buf), "Max:%uKB", (unsigned)(_ota.getMaxFirmwareSize() / 1024));
        _canvas->drawString(buf, cx, y); y += lineH + 2;
    } else {
        _canvas->drawString(_ota.getCurrentVersion(), cx, y); y += lineH;
        snprintf(buf, sizeof(buf), "Part: %s  Max: %uKB",
                 _ota.getRunningPartition(),
                 (unsigned)(_ota.getMaxFirmwareSize() / 1024));
        _canvas->drawString(buf, cx, y); y += lineH + 4;
    }

    // Capabilities
    _canvas->setTextColor(_sd_ok ? 0x07E0 : 0xF800);
    snprintf(buf, sizeof(buf), "SD:%s", _sd_ok ? "OK" : "N/A");
    _canvas->drawString(buf, cx, y); y += lineH;

    _canvas->setTextColor(_espnow_ok ? 0x07E0 : 0xF800);
    int peers = _espnow_ok ? _espnow.getPeerCount() : 0;
    snprintf(buf, sizeof(buf), narrow ? "NOW:%s P:%d" : "ESP-NOW:%s Peers:%d",
             _espnow_ok ? "OK" : "N/A", peers);
    _canvas->drawString(buf, cx, y); y += lineH;

    _canvas->setTextColor(_ble_ok ? 0x07E0 : 0xF800);
    snprintf(buf, sizeof(buf), narrow ? "BLE:%s" : "BLE:%s%s",
             _ble_ok ? "OK" : "N/A",
             (!narrow && _ble_ota.isConnected()) ? " Connected" : "");
    _canvas->drawString(buf, cx, y); y += lineH;

    _canvas->setTextColor(WiFi.isConnected() ? 0x07E0 : 0xF800);
    if (WiFi.isConnected()) {
        snprintf(buf, sizeof(buf), narrow ? "WiFi:%s" : "WiFi:%s %s",
                 WiFi.SSID().c_str(),
                 narrow ? "" : WiFi.localIP().toString().c_str());
    } else {
        snprintf(buf, sizeof(buf), "WiFi:N/A");
    }
    _canvas->drawString(buf, cx, y); y += lineH;

    _canvas->setTextColor(0x07E0);
    _canvas->drawString("Serial:listen", cx, y); y += lineH + 2;

    // Firmware file on SD
    if (_have_firmware_file) {
        _canvas->setTextColor(0xFFE0);
        _canvas->drawString(narrow ? "FW on SD!" : "firmware.bin on SD!", cx, y);
        y += lineH;
    }

    // Status
    y += 4;
    _canvas->setTextColor(TFT_WHITE);
    _canvas->setTextSize(narrow ? 1 : 2);
    // Truncate status for narrow displays
    if (narrow && strlen(_status_line) > 20) {
        char trunc[24];
        strncpy(trunc, _status_line, 20);
        trunc[20] = '\0';
        _canvas->drawString(trunc, cx, y);
    } else {
        _canvas->drawString(_status_line, cx, y);
    }
    y += (narrow ? 1 : 2) * 8 + 4;

    // Progress bar
    if (_progress > 0) {
        int barW = w - margin * 2;
        int barH = narrow ? 10 : 16;
        int barX = margin;
        int barY = y;
        _canvas->drawRect(barX, barY, barW, barH, TFT_WHITE);
        int fillW = (barW - 2) * _progress / 100;
        _canvas->fillRect(barX + 1, barY + 1, fillW, barH - 2, 0x07E0);

        snprintf(buf, sizeof(buf), "%d%%", _progress);
        _canvas->setTextSize(1);
        _canvas->drawString(buf, cx, barY + barH + 2);
        y += barH + lineH;
    }

    // Instructions at bottom (only if enough room)
    if (h - y > 40) {
        int bottomY = h - (narrow ? 36 : 48);
        _canvas->setTextColor(0x7BEF);
        _canvas->setTextSize(1);
        if (narrow) {
            _canvas->drawString("Ser/SD/BLE/P2P", cx, bottomY);
            _canvas->drawString("OTA ready", cx, bottomY + 10);
        } else {
            _canvas->drawString("Serial: OTA_BEGIN <sz> <crc>", cx, bottomY);
            _canvas->drawString("SD: place firmware.bin", cx, bottomY + 12);
            _canvas->drawString("BLE: connect to ESP32-OTA", cx, bottomY + 24);
            _canvas->drawString("P2P: auto mesh offer", cx, bottomY + 36);
        }
    }

    _canvas->pushSprite(0, 0);
}

// ============================================================================
// Mutual Exclusion
// ============================================================================
bool OtaApp::_claimOta(OtaSource src) {
    if (_ota_source != OtaSource::NONE && _ota_source != src) {
        DBG_WARN(TAG, "OTA busy (source=%u), rejecting new source=%u",
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
        DBG_WARN(TAG, "OTA rate limited, wait %u ms (%u failures)", wait, _ota_fail_count);
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
    DBG_INFO(TAG, "Starting SD card OTA...");
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
        DBG_INFO(TAG, "SD OTA successful!");
        return true;
    } else {
        _ota_fail_count++;
        _audit.logAttempt("sd", "?", DISPLAY_DRIVER, false, _ota.getLastError());
        DBG_ERROR(TAG, "SD OTA failed: %s", _ota.getLastError());
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
            Serial.println("OTA_FAIL out of memory");
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
            DBG_ERROR(TAG, "CRC32 mismatch: expected 0x%08X, got 0x%08X",
                      _serial_fw_crc, actualCrc);
            Serial.printf("OTA_FAIL CRC32 mismatch (expected 0x%08X, got 0x%08X)\n",
                          _serial_fw_crc, actualCrc);
            Serial.flush();
            _serial_state = SerialOtaState::ERROR;
            _releaseOta();
            Update.abort();
            return;
        }
        DBG_INFO(TAG, "CRC32 verified: 0x%08X", actualCrc);

        // Verify ECDSA signature
#ifdef OTA_REQUIRE_SIGNATURE
        if (!_serial_signed) {
            DBG_ERROR(TAG, "Unsigned firmware rejected (signature required)");
            Serial.println("OTA_FAIL unsigned firmware rejected");
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
                DBG_ERROR(TAG, "Signature verification FAILED — rejecting firmware");
                Serial.println("OTA_FAIL signature verification failed");
                Serial.flush();
                _serial_state = SerialOtaState::ERROR;
                Update.abort();
                return;
            }
            DBG_INFO(TAG, "ECDSA signature verified OK");
        }

        // All bytes received and verified — finalize and reboot
        bool endOk = Update.end(true);
        if (endOk) {
            _progress = 100;
            _serial_state = SerialOtaState::DONE;
            _releaseOta();
            Serial.println("OTA_OK");
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
                        Serial.println("OTA_FAIL busy (another OTA in progress)");
                        break;
                    }
                    _serial_fw_size = size;
                    _serial_fw_crc = crc;
                    _serial_received = 0;
                    // Note: do NOT reset _serial_signed/_serial_signature here
                    // because OTA_SIG is sent before OTA_BEGIN
                    _progress = 0;

                    DBG_INFO(TAG, "Serial OTA begin: %u bytes, crc=0x%08X", size, crc);

                    if (!Update.begin(size)) {
                        Serial.printf("OTA_FAIL begin: %s\n", Update.errorString());
                        snprintf(_status_line, sizeof(_status_line), "Not enough space");
                    } else {
                        _serial_state = SerialOtaState::RECEIVING;
                        snprintf(_status_line, sizeof(_status_line), "Serial: receiving %u bytes", size);
                        Serial.println("OTA_READY");
                    }
                } else {
                    Serial.println("OTA_FAIL bad args (OTA_BEGIN <size> [crc])");
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
                        Serial.println("OTA_SIG_FAIL invalid hex");
                        break;
                    }
                    _serial_signed = true;
                    DBG_INFO(TAG, "Received ECDSA signature for verification");
                    Serial.println("OTA_SIG_OK");
                } else {
                    Serial.println("OTA_SIG_FAIL bad format (OTA_SIG <r_hex64> <s_hex64>)");
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
                              WiFi.isConnected() ? "true" : "false",
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
                    Serial.println("OTA_SD_START");
                    if (checkSDUpdate()) {
                        Serial.println("OTA_SD_OK");
                        delay(500);
                        _ota.reboot();
                    } else {
                        Serial.printf("OTA_SD_FAIL %s\n", _ota.getLastError());
                    }
                } else {
                    Serial.println("OTA_SD_FAIL no firmware.bin on SD");
                }
            } else if (strcmp(_serial_buf, "USB_MSC") == 0) {
                // Toggle USB Mass Storage mode — SD card appears as USB drive
                if (_sd.isUSBMSCActive()) {
                    _sd.stopUSBMSC();
                    Serial.println("USB_MSC_OFF");
                } else {
                    if (!_sd_ok) _sd_ok = _sd.init();
                    if (_sd_ok && _sd.startUSBMSC()) {
                        Serial.println("USB_MSC_ON");
                    } else {
                        Serial.println("USB_MSC_FAIL");
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
                if (!WiFi.isConnected()) {
                    Serial.println("OTA_URL_FAIL WiFi not connected");
                } else if (strlen(url) < 10) {
                    Serial.println("OTA_URL_FAIL invalid URL");
                } else {
                    if (!_otaRateLimitOk()) {
                        Serial.println("OTA_URL_FAIL rate limited");
                    } else {
                        Serial.printf("OTA_URL_START %s\n", url);
                        if (_ota.updateFromUrl(url)) {
                            _ota_fail_count = 0;
                            _audit.logAttempt("wifi_pull", _ota.getCurrentVersion(), DISPLAY_DRIVER, true);
                            Serial.println("OTA_URL_OK");
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
                Serial.printf("{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}\n",
                              WiFi.isConnected() ? "true" : "false",
                              WiFi.isConnected() ? WiFi.SSID().c_str() : "",
                              WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "",
                              WiFi.isConnected() ? WiFi.RSSI() : 0);
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
                        Serial.println("WIFI_ADD_FAIL");
                    }
                } else {
                    Serial.println("WIFI_ADD_FAIL bad format (WIFI_ADD ssid password)");
                }
            } else if (strcmp(_serial_buf, "WIFI_CONNECT") == 0) {
                _wifi.connect();
                Serial.println("WIFI_CONNECT_OK");
            } else if (strcmp(_serial_buf, "OTA_ROLLBACK") == 0) {
                if (_ota.rollback()) {
                    Serial.println("OTA_ROLLBACK_OK");
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
                    Serial.println("OTA_FAIL path traversal rejected");
                    break;
                }
                if (sdPath[0] != '/') {
                    Serial.println("OTA_FAIL path must be absolute");
                    break;
                }
                if (size > 0 && !_sd_ok) {
                    _sd_ok = _sd.init();
                }
                if (size > 0 && _sd_ok) {
                    File f = SD_MMC.open(sdPath, FILE_WRITE);
                    if (f) {
                        Serial.println("OTA_READY");
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
                                f.write(buf, got);
                                written += got;
                                Serial.printf("OTA_NEXT %u\n", written);
                                Serial.flush();
                            }
                            free(buf);
                            f.close();
                            if (written >= size) {
                                _have_firmware_file = true;
                                Serial.printf("OTA_SD_WRITE_OK %u\n", written);
                                Serial.flush();
                            }
                        } else {
                            f.close();
                            Serial.println("OTA_FAIL out of memory");
                        }
                    } else {
                        Serial.println("OTA_FAIL cannot open SD file");
                    }
                } else {
                    Serial.println("OTA_FAIL bad args or no SD");
                }
            } else if (strcmp(_serial_buf, "OTA_REBOOT") == 0) {
                Serial.println("OTA_REBOOTING");
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
                Serial.println("PROVISION_READY");
                // After this, the HAL reads from Serial directly in processUSBProvision()
                // Set a flag so loop() calls processUSBProvision() instead of handleSerialOTA()
            } else if (strcmp(_serial_buf, "PROVISION_STATUS") == 0) {
                Serial.printf("{\"provisioned\":%s,\"device_id\":\"%s\",\"server_url\":\"%s\"}\n",
                              _provision.isProvisioned() ? "true" : "false",
                              _provision.getIdentity().device_id,
                              _provision.getIdentity().server_url);
            } else if (strcmp(_serial_buf, "PROVISION_RESET") == 0) {
                if (_provision.factoryReset()) {
                    Serial.println("PROVISION_RESET_OK");
                } else {
                    Serial.println("PROVISION_RESET_FAIL");
                }
            } else if (strcmp(_serial_buf, "OTA_AUDIT") == 0) {
                char buf[2048];
                size_t n = _audit.readLog(buf, sizeof(buf));
                if (n > 0) {
                    Serial.print(buf);
                } else {
                    Serial.println("OTA_AUDIT_EMPTY");
                }
            } else if (strcmp(_serial_buf, "OTA_AUDIT_CLEAR") == 0) {
                _audit.clear();
                Serial.println("OTA_AUDIT_CLEAR_OK");
            } else if (strcmp(_serial_buf, "FW_HASH") == 0) {
                char hash[65];
                if (_ota.getFirmwareHash(hash, sizeof(hash))) {
                    Serial.printf("FW_HASH %s\n", hash);
                } else {
                    Serial.println("FW_HASH_FAIL");
                }
            } else if (strcmp(_serial_buf, "MESH_OTA_SEND") == 0) {
                // Send firmware.ota from SD to mesh peers
                if (!_espnow_ok) {
                    Serial.println("MESH_OTA_FAIL espnow not ready");
                } else if (_mesh_ota.isSending()) {
                    Serial.println("MESH_OTA_FAIL already sending");
                } else {
                    if (!_sd_ok) _sd_ok = _sd.init();
                    const char* path = _sd.exists("/firmware.ota") ? "/firmware.ota" : "/firmware.bin";
                    if (_mesh_ota.startSend(path)) {
                        Serial.println("MESH_OTA_SEND_OK");
                    } else {
                        Serial.println("MESH_OTA_FAIL cannot start send");
                    }
                }
            } else if (strcmp(_serial_buf, "MESH_OTA_RECV") == 0) {
                // Enable receiving firmware from mesh peers
                if (!_espnow_ok) {
                    Serial.println("MESH_OTA_FAIL espnow not ready");
                } else {
                    _mesh_ota.enableReceive(true);
                    Serial.println("MESH_OTA_RECV_OK");
                }
            } else if (strcmp(_serial_buf, "MESH_OTA_STOP") == 0) {
                _mesh_ota.stopSend();
                _mesh_ota.enableReceive(false);
                Serial.println("MESH_OTA_STOP_OK");
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
        DBG_WARN(TAG, "Invalid OTA header magic/version");
        return false;
    }

    // Board validation: if header specifies a board, check it matches
    if (hdr.board[0] != '\0' && strcmp(hdr.board, "any") != 0) {
        // Check if board name contains our display driver name (loose match)
        // e.g., "touch-lcd-349" should match a board with AXS15231B
        // We don't enforce strict board matching since display driver names vary
        DBG_INFO(TAG, "OTA target board: %s", hdr.board);
    }

    // Size validation
    size_t maxSize = _ota.getMaxFirmwareSize();
    if (hdr.firmware_size > maxSize) {
        DBG_ERROR(TAG, "Firmware too large: %u > max %u", hdr.firmware_size, (unsigned)maxSize);
        return false;
    }

    // Anti-rollback: reject firmware older than current version
#ifdef OTA_ANTI_ROLLBACK
    if (hdr.version[0] != '\0') {
        const char* currentVer = _ota.getCurrentVersion();
        if (currentVer && otaVersionCompare(hdr.version, currentVer) < 0) {
            DBG_ERROR(TAG, "Anti-rollback: %s < %s (downgrade rejected)", hdr.version, currentVer);
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

    char body[384];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"board\":\"%s\",\"partition\":\"%s\","
             "\"ip\":\"%s\",\"uptime_s\":%lu,\"free_heap\":%u,\"rssi\":%d,"
             "\"fw_hash\":\"%s\"}",
             _ota.getCurrentVersion(), DISPLAY_DRIVER,
             _ota.getRunningPartition(),
             WiFi.localIP().toString().c_str(),
             (unsigned long)(millis() / 1000),
             (unsigned)ESP.getFreeHeap(),
             WiFi.RSSI(),
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
                DBG_INFO(TAG, "Fleet server scheduled OTA: %s", otaUrl.c_str());
                snprintf(_status_line, sizeof(_status_line), "Server OTA: downloading...");
                // Use WiFi pull OTA
                _ota.updateFromUrl(otaUrl.c_str());
            }
        }
    } else {
        DBG_DEBUG(TAG, "Heartbeat: HTTP %d", code);
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
    File f = SD_MMC.open(fwPath, FILE_READ);
    if (!f) {
        fwPath = "/firmware.bin";
        f = SD_MMC.open(fwPath, FILE_READ);
    }
    if (!f) return;

    bool isOtaPkg = false;
    OtaFirmwareHeader hdr = {};
    OtaSignature sig = {};
    uint32_t fwSize = f.size();
    uint32_t fwDataSize = fwSize;  // Actual firmware bytes (excluding header)

    // Try to parse .ota header
    if (f.size() >= sizeof(OtaFirmwareHeader)) {
        f.read((uint8_t*)&hdr, sizeof(hdr));
        if (hdr.isValid()) {
            isOtaPkg = true;
            fwDataSize = hdr.firmware_size;
            if (hdr.isSigned() && f.size() >= sizeof(OtaFirmwareHeader) + sizeof(OtaSignature)) {
                f.read((uint8_t*)&sig, sizeof(sig));
            }
            // Skip to firmware data for CRC computation
            f.seek(hdr.totalHeaderSize());
        } else {
            f.seek(0);  // Not an OTA package, rewind
        }
    }

    // Compute CRC32 of firmware data only
    OtaVerify::crc32Begin();
    uint8_t buf[512];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        OtaVerify::crc32Update(buf, n);
    }
    uint32_t crc = OtaVerify::crc32Finalize();
    f.close();

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
    DBG_INFO(TAG, "Offered firmware: %u bytes, %u chunks, crc=0x%08X, signed=%d",
             fwDataSize, chunkCount, offer.crc32, offer.is_signed);

    // If signed, also broadcast the signature so receiver can verify
    if (offer.is_signed) {
        OtaSigMsg sigMsg = {};
        sigMsg.type = OtaMsgType::SIG;
        memcpy(sigMsg.r, sig.r, 32);
        memcpy(sigMsg.s, sig.s, 32);
        _espnow.meshBroadcast((const uint8_t*)&sigMsg, sizeof(sigMsg));
        DBG_INFO(TAG, "Sent ECDSA signature for P2P transfer");
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

            DBG_INFO(TAG, "P2P offer from %02X:%02X: %u bytes, %u chunks",
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
                    DBG_WARN(TAG, "P2P offer rejected: unsigned (signature required)");
                    break;
                }
#endif
                if (strcmp(offerVer, _ota.getCurrentVersion()) != 0) {
                    DBG_INFO(TAG, "Accepting P2P offer (current: %s, offered: %s, signed: %d)",
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
                    DBG_INFO(TAG, "P2P offer same version, ignoring");
                }
            }
            break;
        }

        case OtaMsgType::REQUEST: {
            if (len < sizeof(OtaChunkRequest)) return;
            const OtaChunkRequest* req = (const OtaChunkRequest*)data;

            DBG_DEBUG(TAG, "P2P chunk request: idx=%u", req->chunk_idx);

            // Send the requested chunk from SD card
            if (!_have_firmware_file || !_sd_ok) return;

            // Validate chunk index
            if (_p2p_chunk_count > 0 && req->chunk_idx >= _p2p_chunk_count) {
                DBG_WARN(TAG, "P2P invalid chunk_idx %u >= %u", req->chunk_idx, _p2p_chunk_count);
                return;
            }

            File f = SD_MMC.open("/firmware.ota", FILE_READ);
            if (!f) f = SD_MMC.open("/firmware.bin", FILE_READ);
            if (!f) return;

            // Determine header offset — skip OTA header to get to firmware data
            uint32_t headerOffset = 0;
            if (f.size() >= sizeof(OtaFirmwareHeader)) {
                OtaFirmwareHeader hdr = {};
                f.read((uint8_t*)&hdr, sizeof(hdr));
                if (hdr.isValid()) {
                    headerOffset = hdr.totalHeaderSize();
                }
            }

            uint32_t offset = headerOffset + (uint32_t)req->chunk_idx * OTA_CHUNK_DATA_SIZE;
            f.seek(offset);

            uint8_t pktBuf[sizeof(OtaChunk) + OTA_CHUNK_DATA_SIZE];
            OtaChunk* chunk = (OtaChunk*)pktBuf;
            chunk->type = OtaMsgType::CHUNK;
            chunk->chunk_idx = req->chunk_idx;
            chunk->data_len = f.read(pktBuf + sizeof(OtaChunk), OTA_CHUNK_DATA_SIZE);
            f.close();

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
                DBG_WARN(TAG, "P2P chunk data_len %u exceeds available %u or max %u",
                         chunk->data_len, availableData, OTA_CHUNK_DATA_SIZE);
                return;
            }
            if (chunk->data_len == 0) return;

            if (_p2p_state != P2PState::RECEIVING) return;
            if (chunk->chunk_idx != _p2p_next_chunk) {
                DBG_WARN(TAG, "P2P unexpected chunk %u (expected %u)",
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
                DBG_ERROR(TAG, "P2P write failed: %s", Update.errorString());
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
                    DBG_ERROR(TAG, "P2P CRC32 mismatch: 0x%08X vs 0x%08X",
                              _p2p_fw_crc, actualCrc);
                    snprintf(_status_line, sizeof(_status_line), "P2P CRC error");
                    p2p_verified = false;
                }

                // Signature verification
                if (p2p_verified && _p2p_signed) {
                    bool sigOk = OtaVerify::finalizeVerify(_p2p_signature.r, _p2p_signature.s);
                    if (!sigOk) {
                        DBG_ERROR(TAG, "P2P ECDSA signature verification FAILED");
                        snprintf(_status_line, sizeof(_status_line), "P2P sig FAIL");
                        p2p_verified = false;
                    } else {
                        DBG_INFO(TAG, "P2P ECDSA signature verified OK");
                    }
                }

#ifdef OTA_REQUIRE_SIGNATURE
                if (p2p_verified && !_p2p_signed) {
                    DBG_ERROR(TAG, "P2P unsigned firmware rejected (signature required)");
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

                    DBG_INFO(TAG, "P2P OTA complete, %u bytes, CRC32 verified: 0x%08X",
                             _p2p_received, actualCrc);
                    delay(1000);
                    _ota.reboot();
                } else {
                    _p2p_state = P2PState::ERROR;
                    _releaseOta();
                    snprintf(_status_line, sizeof(_status_line), "P2P verify failed");
                    DBG_ERROR(TAG, "P2P verify failed: %s", Update.errorString());
                }
            } else {
                requestNextChunk(src);
            }
            break;
        }

        case OtaMsgType::DONE:
            DBG_INFO(TAG, "P2P peer reports transfer done");
            _p2p_state = P2PState::IDLE;
            _releaseOta();
            snprintf(_status_line, sizeof(_status_line), "P2P transfer done");
            break;

        case OtaMsgType::SIG: {
            if (len < sizeof(OtaSigMsg)) return;
            const OtaSigMsg* sigMsg = (const OtaSigMsg*)data;

            if (_p2p_state != P2PState::RECEIVING || !_p2p_signed) {
                DBG_WARN(TAG, "P2P SIG received but not expecting one");
                return;
            }

            memcpy(_p2p_signature.r, sigMsg->r, 32);
            memcpy(_p2p_signature.s, sigMsg->s, 32);
            DBG_INFO(TAG, "P2P received ECDSA signature, starting chunk transfer");

            // Start streaming SHA-256 for signature verification
            OtaVerify::beginVerify();

            // Now start requesting chunks
            requestNextChunk(_p2p_peer);
            break;
        }

        case OtaMsgType::ABORT:
            DBG_WARN(TAG, "P2P peer aborted transfer");
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
        DBG_WARN(TAG, "P2P: OTA busy, rejecting");
        return false;
    }
    if (!Update.begin(size)) {
        DBG_ERROR(TAG, "P2P: Update.begin failed: %s", Update.errorString());
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
