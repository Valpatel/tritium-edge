#include "system_app.h"

#ifdef SIMULATOR
#include "sim_hal.h"
#else
#include "tritium_compat.h"
#endif

#include <math.h>

// Layout constants
static constexpr int NAV_BAR_H       = 40;
static constexpr int MENU_ITEM_H     = 44;
static constexpr int MARGIN          = 10;
static constexpr int BAR_HEIGHT      = 16;
static constexpr uint32_t FPS_UPDATE_MS = 1000;
static constexpr uint32_t TOUCH_DEBOUNCE_MS = 250;

// Color palette
static constexpr uint32_t COL_BG         = 0x000000;
static constexpr uint32_t COL_NAV_BG     = 0x1A1A2E;
static constexpr uint32_t COL_NAV_TEXT   = 0xE0E0FF;
static constexpr uint32_t COL_TEXT       = 0xDDDDDD;
static constexpr uint32_t COL_TEXT_DIM   = 0x888888;
static constexpr uint32_t COL_ACCENT     = 0x0099FF;
static constexpr uint32_t COL_GREEN      = 0x00CC66;
static constexpr uint32_t COL_RED        = 0xFF4444;
static constexpr uint32_t COL_YELLOW     = 0xFFAA00;
static constexpr uint32_t COL_BAR_BG     = 0x222222;
static constexpr uint32_t COL_MENU_BG    = 0x111122;
static constexpr uint32_t COL_MENU_SEL   = 0x1A2A4A;

// ============================================================================
// Setup
// ============================================================================

void SystemApp::setup(LGFX& display) {
    DBG_INFO("system", "App: %s", name());

    _w = display.width();
    _h = display.height();
    _rotation = display.getRotation();

    // Create double-buffer sprite
    _canvas = new LGFX_Sprite(&display);
    _canvas->setPsram(true);
    _canvas->setColorDepth(16);

    if (!_canvas->createSprite(_w, _h)) {
        DBG_WARN("system", "Full sprite failed, trying half-height");
        if (!_canvas->createSprite(_w, _h / 2)) {
            DBG_ERROR("system", "Sprite allocation failed");
            delete _canvas;
            _canvas = nullptr;
        }
    }

    if (_canvas) {
        DBG_INFO("system", "Sprite: %dx%d", _canvas->width(), _canvas->height());
    }

    // LovyanGFX touch driver owns I2C_NUM_0 (SDA/SCL shared bus).
    // HALs use initLgfx() to share the bus via lgfx::i2c API,
    // avoiding conflicts with Arduino Wire on the same I2C peripheral.
#ifndef SIMULATOR
    DBG_INFO("system", "Using lgfx::i2c for shared I2C bus (port %d)", TOUCH_I2C_NUM);
#endif

    // Initialize available HALs
#if HAS_IMU
    _imu = new IMUHAL();
    _imuOwned = true;
#ifndef SIMULATOR
    _imu->initLgfx(TOUCH_I2C_NUM, IMU_I2C_ADDR);
#else
    _imu->init();
#endif
    DBG_INFO("system", "IMU: %s", _imu->available() ? "OK" : "FAIL");
#endif

#if HAS_PMIC
    _power = new PowerHAL();
    _powerOwned = true;
#ifndef SIMULATOR
    _power->initLgfx(TOUCH_I2C_NUM, PMIC_I2C_ADDR);
#else
    _power->init();
#endif
    DBG_INFO("system", "Power: %s", _power->available() ? "OK" : "FAIL");
#endif

#if HAS_SDCARD
    _sd = new SDCardHAL();
    _sdOwned = true;
    _sd->init();
    DBG_INFO("system", "SD Card: %s", _sd->isMounted() ? "Mounted" : "Not mounted");
#endif

#if HAS_CAMERA
    _camera = new CameraHAL();
    _cameraOwned = true;
    // Use QVGA for preview - fits most displays and is fast
    if (!_camera->init(CamResolution::QVGA_320x240, CamPixelFormat::RGB565)) {
        DBG_WARN("system", "Camera init failed (module may not be connected)");
    }
    DBG_INFO("system", "Camera: %s", _camera->available() ? "OK" : "FAIL");
#endif

#if HAS_RTC
    _rtc = new RTCHAL();
    _rtcOwned = true;
#ifndef SIMULATOR
    _rtc->initLgfx(TOUCH_I2C_NUM, RTC_I2C_ADDR);
#else
    _rtc->init();
#endif
    DBG_INFO("system", "RTC: %s", _rtc->available() ? "OK" : "FAIL");
#endif

#if HAS_AUDIO_CODEC
    _audio = new AudioHAL();
    _audioOwned = true;
#ifndef SIMULATOR
    _audio->initLgfx(TOUCH_I2C_NUM, AUDIO_CODEC_ADDR);
#else
    _audio->init();
#endif
    DBG_INFO("system", "Audio: %s (codec=%d, mic=%d, spk=%d)",
             _audio->available() ? "OK" : "FAIL",
             _audio->hasCodec(), _audio->hasMic(), _audio->hasSpeaker());

    // Voice command detection
    if (_audio->available()) {
        _voice = new VoiceHAL();
        _voiceOwned = true;
        if (_voice->init(_audio)) {
            // Register default commands for testing
            _voice->addCommand("yes", VOICE_CMD_YES);
            _voice->addCommand("no", VOICE_CMD_NO);
            _voice->addCommand("stop", VOICE_CMD_STOP);
            _voice->addCommand("go", VOICE_CMD_GO);
            DBG_INFO("system", "Voice: OK (%d commands registered)", _voice->getCommandCount());
        } else {
            DBG_WARN("system", "Voice: init failed");
        }
    }
#endif

    // WiFi manager (singleton pattern)
    _wifi = WifiManager::_instance;
    if (!_wifi) {
        _wifi = new WifiManager();
        _wifi->init();
    }

    buildMenuItems();

    _fpsTimer = millis();
    _frameCount = 0;
    _currentScreen = SystemScreen::HOME;

    // Boot beep to confirm speaker output
#if HAS_AUDIO_CODEC
    if (_audio && _audio->available()) {
        _audio->setSpeakerEnabled(true);
        _audio->playTone(1000, 50);  // 50ms beep at 1kHz
    }
#endif

    DBG_INFO("system", "Setup complete, %d menu items", _menuCount);
}

// ============================================================================
// Loop
// ============================================================================

void SystemApp::loop(LGFX& display) {
    uint32_t now = millis();

    handleTouch(display);

    // Process voice HAL when on voice screen
#if HAS_AUDIO_CODEC
    if (_currentScreen == SystemScreen::VOICE_COMMANDS && _voice) {
        // Handle training request
        if (_voiceTrainCmd >= 0) {
            int cmdIdx = _voiceTrainCmd;
            _voiceTrainCmd = -1;
            // trainCommand blocks while recording
            if (_voice->trainCommand(cmdIdx)) {
                DBG_INFO("voice", "Training complete for command %d", cmdIdx);
            } else {
                DBG_WARN("voice", "Training failed for command %d", cmdIdx);
            }
        }

        // Process VAD and recognition
        _voice->process();

        // Check for detection results
        if (_voice->hasCommand()) {
            _lastVoiceDet = _voice->getCommand();
            _voiceHasResult = true;
            _voiceResultTime = now;
            DBG_INFO("voice", "Detected: '%s' (conf=%.2f)",
                     _lastVoiceDet.label, _lastVoiceDet.confidence);
        }

        // Clear old results after 3 seconds
        if (_voiceHasResult && now - _voiceResultTime > 3000) {
            _voiceHasResult = false;
        }
    }
#endif

    // Camera preview renders directly to display (no sprite buffer needed)
    if (_currentScreen == SystemScreen::CAMERA_PREVIEW) {
#if HAS_CAMERA
        drawCameraPreview(display);
#endif
    } else if (_canvas) {
        _canvas->fillScreen(COL_BG);

        switch (_currentScreen) {
            case SystemScreen::HOME:    drawHome(*_canvas);    break;
            case SystemScreen::IMU:     drawIMU(*_canvas);     break;
            case SystemScreen::BATTERY: drawBattery(*_canvas); break;
            case SystemScreen::NETWORK: drawNetwork(*_canvas); break;
            case SystemScreen::STORAGE:    drawStorage(*_canvas);   break;
            case SystemScreen::AUDIO_TEST:     drawAudioTest(*_canvas);     break;
            case SystemScreen::VOICE_COMMANDS: drawVoiceCommands(*_canvas); break;
            case SystemScreen::SETTINGS:       drawSettings(*_canvas);      break;
            default: break;
        }

        _canvas->pushSprite(0, 0);
    } else {
        // No sprite — draw directly to display as diagnostic
        display.fillScreen(COL_BG);
        display.setTextColor(0xFF0000);
        display.setTextSize(2);
        display.setTextDatum(middle_center);
        display.drawString("NO SPRITE", _w / 2, _h / 2);
        Serial.printf("[DBG] No canvas! Drawing direct fallback.\n");
    }

    // FPS tracking
    _frameCount++;
    if (now - _fpsTimer >= FPS_UPDATE_MS) {
        _fps = _frameCount * 1000.0f / (now - _fpsTimer);
        _frameCount = 0;
        _fpsTimer = now;
    }
}

// ============================================================================
// Navigation
// ============================================================================

void SystemApp::buildMenuItems() {
    _menuCount = 0;

    // Always-available items
    _menuItems[_menuCount++] = {"IMU Sensors",     SystemScreen::IMU,
#if HAS_IMU
                                true
#else
                                false
#endif
    };

    _menuItems[_menuCount++] = {"Battery / Power", SystemScreen::BATTERY,
#if HAS_PMIC
                                true
#else
                                false
#endif
    };

    _menuItems[_menuCount++] = {"Network / WiFi",  SystemScreen::NETWORK,  true};

    _menuItems[_menuCount++] = {"Storage / SD",     SystemScreen::STORAGE,
#if HAS_SDCARD
                                true
#else
                                false
#endif
    };

    _menuItems[_menuCount++] = {"Camera Preview",   SystemScreen::CAMERA_PREVIEW,
#if HAS_CAMERA
                                true
#else
                                false
#endif
    };

    _menuItems[_menuCount++] = {"Audio / Mic",      SystemScreen::AUDIO_TEST,
#if HAS_AUDIO_CODEC
                                true
#else
                                false
#endif
    };

    _menuItems[_menuCount++] = {"Voice Commands",   SystemScreen::VOICE_COMMANDS,
#if HAS_AUDIO_CODEC
                                true
#else
                                false
#endif
    };

    _menuItems[_menuCount++] = {"Settings",         SystemScreen::SETTINGS, true};
}

void SystemApp::navigateTo(SystemScreen screen) {
    _currentScreen = screen;
    _scrollOffset = 0;

    if (screen == SystemScreen::NETWORK && !_scanRequested) {
        if (_wifi) {
            _wifi->startScan();
            _scanRequested = true;
            _lastScanTime = millis();
        }
    }

#if HAS_CAMERA
    if (screen == SystemScreen::CAMERA_PREVIEW) {
        _camFrameCount = 0;
        _camFpsTimer = millis();
        _camFps = 0.0f;
    }
#endif

    DBG_INFO("system", "Navigate -> %s", screenTitle(screen));
}

void SystemApp::handleTouch(LGFX& display) {
    lgfx::touch_point_t tp;
    bool touched = (display.getTouch(&tp) > 0);
    uint32_t now = millis();

    if (touched && !_touchDown && (now - _lastTouchTime > TOUCH_DEBOUNCE_MS)) {
        _touchDown = true;
        _lastTouchTime = now;

        int tx = tp.x;
        int ty = tp.y;

        // Top nav bar = back to HOME
        if (ty < NAV_BAR_H && _currentScreen != SystemScreen::HOME) {
            navigateTo(SystemScreen::HOME);
            return;
        }

        // HOME screen: menu item selection
        if (_currentScreen == SystemScreen::HOME) {
            // Menu starts below the info section
            // Info section is roughly NAV_BAR_H + some info lines
            int infoSectionH = NAV_BAR_H + 120; // approximate height of the info block
            int menuY = ty - infoSectionH + _scrollOffset;
            if (menuY >= 0) {
                int idx = menuY / MENU_ITEM_H;
                if (idx >= 0 && idx < _menuCount && _menuItems[idx].available) {
                    navigateTo(_menuItems[idx].screen);
                }
            }
        }

        // Settings screen: brightness slider
        if (_currentScreen == SystemScreen::SETTINGS) {
            int contentY = ty - NAV_BAR_H;
            // Brightness slider region (roughly y=40..70 in content area)
            if (contentY >= 35 && contentY <= 75) {
                int barX = MARGIN + 10;
                int barW = _w - 2 * MARGIN - 20;
                float ratio = (float)(tx - barX) / barW;
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                _brightness = (int)(ratio * 255);
                display.setBrightness(_brightness);
            }
            // Rotation selector region (roughly y=100..140)
            if (contentY >= 95 && contentY <= 140) {
                int segW = (_w - 2 * MARGIN) / 4;
                int seg = (tx - MARGIN) / segW;
                if (seg >= 0 && seg <= 3) {
                    _rotation = seg;
                    display.setRotation(_rotation);
                    _w = display.width();
                    _h = display.height();
                    // Recreate sprite for new dimensions
                    if (_canvas) {
                        _canvas->deleteSprite();
                        if (!_canvas->createSprite(_w, _h)) {
                            _canvas->createSprite(_w, _h / 2);
                        }
                    }
                }
            }
            // Sleep timeout region (roughly y=165..200)
            if (contentY >= 160 && contentY <= 205) {
                // Cycle through: Off, 30s, 60s, 120s, 300s
                static const int timeouts[] = {0, 30, 60, 120, 300};
                int cur = 0;
                for (int i = 0; i < 5; i++) {
                    if (_sleepTimeout == timeouts[i]) { cur = i; break; }
                }
                cur = (cur + 1) % 5;
                _sleepTimeout = timeouts[cur];
            }
        }

#if HAS_AUDIO_CODEC
        // Voice commands screen
        if (_currentScreen == SystemScreen::VOICE_COMMANDS && _voice) {
            int contentY = ty - NAV_BAR_H;

            // Command list area: each command row is 36px, starting at y~80
            int listStartY = 80;
            int rowH = 36;
            int cmdCount = _voice->getCommandCount();
            if (contentY >= listStartY && contentY < listStartY + cmdCount * rowH) {
                _voiceSelectedCmd = (contentY - listStartY) / rowH;
            }

            // Train button area (below list)
            int trainBtnY = listStartY + cmdCount * rowH + 12;
            if (contentY >= trainBtnY && contentY <= trainBtnY + 36) {
                if (tx < _w / 2) {
                    // Train button
                    if (_voiceSelectedCmd >= 0 && _voiceSelectedCmd < cmdCount) {
                        _voiceTrainCmd = _voiceSelectedCmd;
                        DBG_INFO("voice", "Training command %d...", _voiceSelectedCmd);
                    }
                } else {
                    // Listen/Test button
                    _voice->setEnabled(!_voice->isEnabled());
                    DBG_INFO("voice", "Voice detection: %s",
                             _voice->isEnabled() ? "enabled" : "disabled");
                }
            }
        }

        // Audio test screen: tone buttons and volume slider
        if (_currentScreen == SystemScreen::AUDIO_TEST && _audio && _audio->available()) {
            int contentY = ty - NAV_BAR_H;

            // Tone buttons region - approximate Y based on layout
            // Mic level (24+24+32) + Spectrum (24+60+18+8) + Tone label (24) = ~214
            int toneY = 214;
            if (contentY >= toneY && contentY <= toneY + 32) {
                const uint16_t tones[] = {262, 440, 880, 1000};
                int btnW = (_w - 2 * MARGIN - 12) / 4;
                int idx = (tx - MARGIN) / (btnW + 4);
                if (idx >= 0 && idx < 4) {
                    _toneFreq = tones[idx];
                    _toneRequested = true;
                }
            }

            // Volume slider region (below tone buttons)
            int volY = toneY + 54;
            if (contentY >= volY && contentY <= volY + 14) {
                int barX = MARGIN + 10;
                int barW = _w - 2 * MARGIN - 20;
                float ratio = (float)(tx - barX) / barW;
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                _audio->setVolume((uint8_t)(ratio * 100));
            }
        }
#endif
    }

    if (!touched) {
        _touchDown = false;
    }
}

const char* SystemApp::screenTitle(SystemScreen s) {
    switch (s) {
        case SystemScreen::HOME:           return "System Dashboard";
        case SystemScreen::IMU:            return "IMU Sensors";
        case SystemScreen::BATTERY:        return "Battery / Power";
        case SystemScreen::NETWORK:        return "Network / WiFi";
        case SystemScreen::STORAGE:        return "Storage / SD";
        case SystemScreen::CAMERA_PREVIEW: return "Camera Preview";
        case SystemScreen::AUDIO_TEST:     return "Audio / Mic";
        case SystemScreen::VOICE_COMMANDS: return "Voice Commands";
        case SystemScreen::SETTINGS:       return "Settings";
        default:                           return "Unknown";
    }
}

// ============================================================================
// Drawing Helpers
// ============================================================================

void SystemApp::drawNavBar(LGFX_Sprite& spr, const char* title) {
    spr.fillRect(0, 0, _w, NAV_BAR_H, COL_NAV_BG);

    spr.setTextColor(COL_NAV_TEXT);
    spr.setTextSize(2);
    spr.setTextDatum(middle_center);
    spr.drawString(title, _w / 2, NAV_BAR_H / 2);

    // Back indicator (if not on home)
    if (_currentScreen != SystemScreen::HOME) {
        spr.setTextSize(1);
        spr.setTextDatum(middle_left);
        spr.drawString("< Back", 8, NAV_BAR_H / 2);
    }
}

void SystemApp::drawProgressBar(LGFX_Sprite& spr, int x, int y, int w, int h,
                                 float value, uint32_t fg, uint32_t bg) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    spr.fillRect(x, y, w, h, bg);
    int filled = (int)(w * value);
    if (filled > 0) {
        spr.fillRect(x, y, filled, h, fg);
    }
    spr.drawRect(x, y, w, h, COL_TEXT_DIM);
}

// ============================================================================
// HOME Screen
// ============================================================================

void SystemApp::drawHome(LGFX_Sprite& spr) {
    drawNavBar(spr, "System Dashboard");

    int y = NAV_BAR_H + 8;
    int lineH = 18;
    char buf[80];

    spr.setTextSize(1);
    spr.setTextDatum(top_left);

    // Board info
    spr.setTextColor(COL_ACCENT);
    snprintf(buf, sizeof(buf), "Board: %s  %dx%d  %s",
             DISPLAY_DRIVER, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_IF);
    spr.drawString(buf, MARGIN, y);
    y += lineH;

    // FPS
    spr.setTextColor(COL_GREEN);
    snprintf(buf, sizeof(buf), "FPS: %.1f", _fps);
    spr.drawString(buf, MARGIN, y);
    y += lineH;

    // Free heap
    spr.setTextColor(COL_TEXT);
#ifndef SIMULATOR
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    snprintf(buf, sizeof(buf), "Heap: %lu / %lu KB", freeHeap / 1024, totalHeap / 1024);
#else
    snprintf(buf, sizeof(buf), "Heap: (simulator)");
#endif
    spr.drawString(buf, MARGIN, y);
    y += lineH;

    // PSRAM
#ifndef SIMULATOR
    if (ESP.getPsramSize() > 0) {
        uint32_t freePsram = ESP.getFreePsram();
        uint32_t totalPsram = ESP.getPsramSize();
        snprintf(buf, sizeof(buf), "PSRAM: %lu / %lu KB",
                 freePsram / 1024, totalPsram / 1024);
    } else {
        snprintf(buf, sizeof(buf), "PSRAM: Not available");
    }
#else
    snprintf(buf, sizeof(buf), "PSRAM: (simulator)");
#endif
    spr.drawString(buf, MARGIN, y);
    y += lineH;

    // Uptime
    uint32_t uptimeSec = millis() / 1000;
    uint32_t hours = uptimeSec / 3600;
    uint32_t mins = (uptimeSec % 3600) / 60;
    uint32_t secs = uptimeSec % 60;
    snprintf(buf, sizeof(buf), "Uptime: %luh %02lum %02lus",
             (unsigned long)hours, (unsigned long)mins, (unsigned long)secs);
    spr.drawString(buf, MARGIN, y);
    y += lineH;

    // RTC time if available
#if HAS_RTC
    if (_rtc && _rtc->available()) {
        RTCTime t = _rtc->getTime();
        snprintf(buf, sizeof(buf), "RTC: %04d-%02d-%02d %02d:%02d:%02d",
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
        spr.drawString(buf, MARGIN, y);
        y += lineH;
    }
#endif

    y += 8;

    // Menu divider
    spr.drawFastHLine(MARGIN, y, _w - 2 * MARGIN, COL_TEXT_DIM);
    y += 6;

    // Menu items
    for (int i = 0; i < _menuCount; i++) {
        int itemY = y + i * MENU_ITEM_H;
        if (itemY + MENU_ITEM_H < 0 || itemY > _h) continue;

        uint32_t bg = (i % 2 == 0) ? COL_MENU_BG : COL_BG;
        spr.fillRect(0, itemY, _w, MENU_ITEM_H, bg);

        if (_menuItems[i].available) {
            spr.setTextColor(COL_TEXT);
        } else {
            spr.setTextColor(COL_TEXT_DIM);
        }

        spr.setTextDatum(middle_left);
        spr.setTextSize(2);
        spr.drawString(_menuItems[i].label, MARGIN + 4, itemY + MENU_ITEM_H / 2);

        // Arrow indicator for available items
        if (_menuItems[i].available) {
            spr.setTextDatum(middle_right);
            spr.setTextSize(1);
            spr.setTextColor(COL_ACCENT);
            spr.drawString(">", _w - MARGIN, itemY + MENU_ITEM_H / 2);
        } else {
            spr.setTextDatum(middle_right);
            spr.setTextSize(1);
            spr.setTextColor(COL_TEXT_DIM);
            spr.drawString("N/A", _w - MARGIN, itemY + MENU_ITEM_H / 2);
        }
    }
}

// ============================================================================
// IMU Screen
// ============================================================================

void SystemApp::drawIMU(LGFX_Sprite& spr) {
    drawNavBar(spr, "IMU Sensors");

#if HAS_IMU
    if (!_imu || !_imu->available()) {
        spr.setTextColor(COL_RED);
        spr.setTextSize(2);
        spr.setTextDatum(middle_center);
        spr.drawString("IMU not available", _w / 2, _h / 2);
        return;
    }

    // Read fresh data
    _imu->readAll(_ax, _ay, _az, _gx, _gy, _gz);

    int y = NAV_BAR_H + 12;
    int barW = _w - 2 * MARGIN - 80;
    int lineH = 32;
    char buf[40];

    // Accelerometer section
    spr.setTextColor(COL_ACCENT);
    spr.setTextSize(2);
    spr.setTextDatum(top_left);
    spr.drawString("Accelerometer (g)", MARGIN, y);
    y += 24;

    spr.setTextSize(1);
    // Scale: +/- 8g mapped to bar width
    float accelScale = 8.0f;

    // X axis
    spr.setTextColor(COL_RED);
    snprintf(buf, sizeof(buf), "X: %+.2f", _ax);
    spr.drawString(buf, MARGIN, y + 2);
    float normX = (_ax / accelScale + 1.0f) / 2.0f;
    drawProgressBar(spr, MARGIN + 72, y, barW, BAR_HEIGHT, normX, COL_RED, COL_BAR_BG);
    // Center line
    spr.drawFastVLine(MARGIN + 72 + barW / 2, y, BAR_HEIGHT, COL_TEXT_DIM);
    y += lineH;

    // Y axis
    spr.setTextColor(COL_GREEN);
    snprintf(buf, sizeof(buf), "Y: %+.2f", _ay);
    spr.drawString(buf, MARGIN, y + 2);
    float normY = (_ay / accelScale + 1.0f) / 2.0f;
    drawProgressBar(spr, MARGIN + 72, y, barW, BAR_HEIGHT, normY, COL_GREEN, COL_BAR_BG);
    spr.drawFastVLine(MARGIN + 72 + barW / 2, y, BAR_HEIGHT, COL_TEXT_DIM);
    y += lineH;

    // Z axis
    spr.setTextColor(COL_ACCENT);
    snprintf(buf, sizeof(buf), "Z: %+.2f", _az);
    spr.drawString(buf, MARGIN, y + 2);
    float normZ = (_az / accelScale + 1.0f) / 2.0f;
    drawProgressBar(spr, MARGIN + 72, y, barW, BAR_HEIGHT, normZ, COL_ACCENT, COL_BAR_BG);
    spr.drawFastVLine(MARGIN + 72 + barW / 2, y, BAR_HEIGHT, COL_TEXT_DIM);
    y += lineH + 8;

    // Divider
    spr.drawFastHLine(MARGIN, y, _w - 2 * MARGIN, COL_TEXT_DIM);
    y += 12;

    // Gyroscope section
    spr.setTextColor(COL_YELLOW);
    spr.setTextSize(2);
    spr.setTextDatum(top_left);
    spr.drawString("Gyroscope (dps)", MARGIN, y);
    y += 24;

    spr.setTextSize(1);
    float gyroScale = 512.0f;

    // GX
    spr.setTextColor(COL_RED);
    snprintf(buf, sizeof(buf), "X: %+.1f", _gx);
    spr.drawString(buf, MARGIN, y + 2);
    float normGX = (_gx / gyroScale + 1.0f) / 2.0f;
    drawProgressBar(spr, MARGIN + 72, y, barW, BAR_HEIGHT, normGX, COL_RED, COL_BAR_BG);
    spr.drawFastVLine(MARGIN + 72 + barW / 2, y, BAR_HEIGHT, COL_TEXT_DIM);
    y += lineH;

    // GY
    spr.setTextColor(COL_GREEN);
    snprintf(buf, sizeof(buf), "Y: %+.1f", _gy);
    spr.drawString(buf, MARGIN, y + 2);
    float normGY = (_gy / gyroScale + 1.0f) / 2.0f;
    drawProgressBar(spr, MARGIN + 72, y, barW, BAR_HEIGHT, normGY, COL_GREEN, COL_BAR_BG);
    spr.drawFastVLine(MARGIN + 72 + barW / 2, y, BAR_HEIGHT, COL_TEXT_DIM);
    y += lineH;

    // GZ
    spr.setTextColor(COL_ACCENT);
    snprintf(buf, sizeof(buf), "Z: %+.1f", _gz);
    spr.drawString(buf, MARGIN, y + 2);
    float normGZ = (_gz / gyroScale + 1.0f) / 2.0f;
    drawProgressBar(spr, MARGIN + 72, y, barW, BAR_HEIGHT, normGZ, COL_ACCENT, COL_BAR_BG);
    spr.drawFastVLine(MARGIN + 72 + barW / 2, y, BAR_HEIGHT, COL_TEXT_DIM);

#else
    spr.setTextColor(COL_TEXT_DIM);
    spr.setTextSize(2);
    spr.setTextDatum(middle_center);
    spr.drawString("IMU not present", _w / 2, _h / 2);
    spr.setTextSize(1);
    spr.drawString("(HAS_IMU = 0)", _w / 2, _h / 2 + 24);
#endif
}

// ============================================================================
// Battery Screen
// ============================================================================

void SystemApp::drawBattery(LGFX_Sprite& spr) {
    drawNavBar(spr, "Battery / Power");

#if HAS_PMIC
    if (!_power || !_power->available()) {
        spr.setTextColor(COL_RED);
        spr.setTextSize(2);
        spr.setTextDatum(middle_center);
        spr.drawString("Power HAL not ready", _w / 2, _h / 2);
        return;
    }

    PowerInfo info = _power->getInfo();

    int y = NAV_BAR_H + 16;
    int barW = _w - 2 * MARGIN - 20;
    char buf[60];

    // Large battery percentage
    spr.setTextColor(COL_TEXT);
    spr.setTextSize(4);
    spr.setTextDatum(top_center);
    if (info.percentage >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", info.percentage);
    } else {
        snprintf(buf, sizeof(buf), "--%%");
    }
    spr.drawString(buf, _w / 2, y);
    y += 48;

    // Battery bar
    float pct = (info.percentage >= 0) ? info.percentage / 100.0f : 0.0f;
    uint32_t barColor = COL_GREEN;
    if (info.percentage < 20) barColor = COL_RED;
    else if (info.percentage < 50) barColor = COL_YELLOW;
    drawProgressBar(spr, MARGIN + 10, y, barW, 24, pct, barColor, COL_BAR_BG);
    y += 36;

    // Voltage
    spr.setTextSize(2);
    spr.setTextDatum(top_left);
    spr.setTextColor(COL_TEXT);
    snprintf(buf, sizeof(buf), "Voltage: %.2f V", info.voltage);
    spr.drawString(buf, MARGIN, y);
    y += 28;

    // Charging status
    if (info.is_charging) {
        spr.setTextColor(COL_GREEN);
        spr.drawString("Charging", MARGIN, y);
    } else {
        spr.setTextColor(COL_TEXT_DIM);
        spr.drawString("Not charging", MARGIN, y);
    }
    y += 28;

    // Power source
    spr.setTextColor(COL_TEXT);
    const char* srcStr = "Unknown";
    switch (info.source) {
        case PowerSource::USB:     srcStr = "USB"; break;
        case PowerSource::BATTERY: srcStr = "Battery"; break;
        default: break;
    }
    snprintf(buf, sizeof(buf), "Source: %s", srcStr);
    spr.drawString(buf, MARGIN, y);
    y += 28;

    // Has battery indicator
    snprintf(buf, sizeof(buf), "Battery: %s", info.has_battery ? "Present" : "Not detected");
    spr.drawString(buf, MARGIN, y);
    y += 28;

    // PMIC type
    snprintf(buf, sizeof(buf), "PMIC: %s  ADC: %s",
             _power->hasPMIC() ? "Yes" : "No",
             _power->hasADC() ? "Yes" : "No");
    spr.setTextSize(1);
    spr.setTextColor(COL_TEXT_DIM);
    spr.drawString(buf, MARGIN, y);

#else
    spr.setTextColor(COL_TEXT_DIM);
    spr.setTextSize(2);
    spr.setTextDatum(middle_center);
    spr.drawString("PMIC not present", _w / 2, _h / 2);
    spr.setTextSize(1);
    spr.drawString("(HAS_PMIC = 0)", _w / 2, _h / 2 + 24);
#endif
}

// ============================================================================
// Network Screen
// ============================================================================

void SystemApp::drawNetwork(LGFX_Sprite& spr) {
    drawNavBar(spr, "Network / WiFi");

    int y = NAV_BAR_H + 10;
    int barW = _w - 2 * MARGIN - 20;
    char buf[80];

    if (!_wifi) {
        spr.setTextColor(COL_RED);
        spr.setTextSize(2);
        spr.setTextDatum(middle_center);
        spr.drawString("WiFi not initialized", _w / 2, _h / 2);
        return;
    }

    spr.setTextSize(1);
    spr.setTextDatum(top_left);

    // WiFi state
    WifiState state = _wifi->getState();
    const char* stateStr = "Unknown";
    uint32_t stateColor = COL_TEXT;
    switch (state) {
        case WifiState::IDLE:         stateStr = "Idle";         stateColor = COL_TEXT_DIM; break;
        case WifiState::SCANNING:     stateStr = "Scanning...";  stateColor = COL_YELLOW;   break;
        case WifiState::CONNECTING:   stateStr = "Connecting...";stateColor = COL_YELLOW;   break;
        case WifiState::CONNECTED:    stateStr = "Connected";    stateColor = COL_GREEN;    break;
        case WifiState::DISCONNECTED: stateStr = "Disconnected"; stateColor = COL_RED;      break;
        case WifiState::FAILED:       stateStr = "Failed";       stateColor = COL_RED;      break;
    }

    spr.setTextColor(stateColor);
    spr.setTextSize(2);
    snprintf(buf, sizeof(buf), "WiFi: %s", stateStr);
    spr.drawString(buf, MARGIN, y);
    y += 28;

    spr.setTextSize(1);
    spr.setTextColor(COL_TEXT);

    // Connected info
    if (_wifi->isConnected()) {
        snprintf(buf, sizeof(buf), "SSID: %s", _wifi->getSSID());
        spr.drawString(buf, MARGIN, y);
        y += 18;

        snprintf(buf, sizeof(buf), "IP: %s", _wifi->getIP());
        spr.drawString(buf, MARGIN, y);
        y += 18;

        int32_t rssi = _wifi->getRSSI();
        snprintf(buf, sizeof(buf), "RSSI: %ld dBm", (long)rssi);
        spr.drawString(buf, MARGIN, y);
        y += 20;

        // RSSI bar: -100 dBm (bad) to -30 dBm (excellent)
        float rssiNorm = (float)(rssi + 100) / 70.0f;
        if (rssiNorm < 0.0f) rssiNorm = 0.0f;
        if (rssiNorm > 1.0f) rssiNorm = 1.0f;
        uint32_t rssiColor = COL_RED;
        if (rssiNorm > 0.7f) rssiColor = COL_GREEN;
        else if (rssiNorm > 0.4f) rssiColor = COL_YELLOW;
        drawProgressBar(spr, MARGIN + 10, y, barW, BAR_HEIGHT, rssiNorm, rssiColor, COL_BAR_BG);
        y += BAR_HEIGHT + 8;
    }

    y += 4;
    spr.drawFastHLine(MARGIN, y, _w - 2 * MARGIN, COL_TEXT_DIM);
    y += 8;

    // Scan results
    spr.setTextColor(COL_ACCENT);
    spr.setTextSize(2);
    spr.drawString("Scan Results", MARGIN, y);
    y += 24;

    // Refresh scan results from WiFi manager
    _scanResultCount = _wifi->getScanResults(_scanResults, WIFI_MAX_SCAN_RESULTS);

    spr.setTextSize(1);
    if (_scanResultCount == 0) {
        spr.setTextColor(COL_TEXT_DIM);
        if (_wifi->getState() == WifiState::SCANNING) {
            spr.drawString("Scanning...", MARGIN, y);
        } else {
            spr.drawString("No networks found. Touch to scan.", MARGIN, y);
        }
    } else {
        for (int i = 0; i < _scanResultCount && y < _h - 16; i++) {
            spr.setTextColor(COL_TEXT);
            snprintf(buf, sizeof(buf), "%s", _scanResults[i].ssid);
            spr.drawString(buf, MARGIN + 4, y);

            // RSSI indicator on right
            int32_t rssi = _scanResults[i].rssi;
            float rn = (float)(rssi + 100) / 70.0f;
            if (rn < 0.0f) rn = 0.0f;
            if (rn > 1.0f) rn = 1.0f;

            int miniBarW = 40;
            int miniBarX = _w - MARGIN - miniBarW - 40;
            drawProgressBar(spr, miniBarX, y + 1, miniBarW, 10, rn,
                           (rn > 0.5f ? COL_GREEN : COL_YELLOW), COL_BAR_BG);

            spr.setTextColor(COL_TEXT_DIM);
            snprintf(buf, sizeof(buf), "%ld", (long)rssi);
            spr.setTextDatum(top_right);
            spr.drawString(buf, _w - MARGIN, y);
            spr.setTextDatum(top_left);

            y += 18;
        }
    }
}

// ============================================================================
// Storage Screen
// ============================================================================

void SystemApp::drawStorage(LGFX_Sprite& spr) {
    drawNavBar(spr, "Storage / SD");

#if HAS_SDCARD
    if (!_sd) {
        spr.setTextColor(COL_RED);
        spr.setTextSize(2);
        spr.setTextDatum(middle_center);
        spr.drawString("SD HAL not ready", _w / 2, _h / 2);
        return;
    }

    int y = NAV_BAR_H + 16;
    int barW = _w - 2 * MARGIN - 20;
    char buf[60];

    spr.setTextDatum(top_left);

    if (!_sd->isMounted()) {
        spr.setTextColor(COL_YELLOW);
        spr.setTextSize(2);
        spr.drawString("SD Card not mounted", MARGIN, y);
        y += 28;
        spr.setTextSize(1);
        spr.setTextColor(COL_TEXT_DIM);
        spr.drawString("Insert card and restart", MARGIN, y);
        return;
    }

    uint64_t total = _sd->totalBytes();
    uint64_t used = _sd->usedBytes();
    uint64_t free = total - used;

    // Large status
    spr.setTextColor(COL_GREEN);
    spr.setTextSize(2);
    spr.drawString("SD Card Mounted", MARGIN, y);
    y += 32;

    // Usage bar
    float usedRatio = (total > 0) ? (float)used / (float)total : 0.0f;
    drawProgressBar(spr, MARGIN + 10, y, barW, 24, usedRatio, COL_ACCENT, COL_BAR_BG);
    y += 32;

    // Stats
    spr.setTextSize(1);
    spr.setTextColor(COL_TEXT);

    // Format sizes appropriately
    auto formatSize = [](uint64_t bytes, char* out, size_t outLen) {
        if (bytes >= (uint64_t)1024 * 1024 * 1024) {
            snprintf(out, outLen, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        } else if (bytes >= (uint64_t)1024 * 1024) {
            snprintf(out, outLen, "%.1f MB", bytes / (1024.0 * 1024.0));
        } else {
            snprintf(out, outLen, "%llu KB", (unsigned long long)(bytes / 1024));
        }
    };

    char sizeStr[20];

    formatSize(total, sizeStr, sizeof(sizeStr));
    snprintf(buf, sizeof(buf), "Total: %s", sizeStr);
    spr.drawString(buf, MARGIN, y);
    y += 20;

    formatSize(used, sizeStr, sizeof(sizeStr));
    snprintf(buf, sizeof(buf), "Used:  %s (%.1f%%)", sizeStr, usedRatio * 100.0f);
    spr.drawString(buf, MARGIN, y);
    y += 20;

    formatSize(free, sizeStr, sizeof(sizeStr));
    snprintf(buf, sizeof(buf), "Free:  %s", sizeStr);
    spr.drawString(buf, MARGIN, y);

#else
    spr.setTextColor(COL_TEXT_DIM);
    spr.setTextSize(2);
    spr.setTextDatum(middle_center);
    spr.drawString("SD Card not present", _w / 2, _h / 2);
    spr.setTextSize(1);
    spr.drawString("(HAS_SDCARD = 0)", _w / 2, _h / 2 + 24);
#endif
}

// ============================================================================
// Camera Preview Screen
// ============================================================================

void SystemApp::drawCameraPreview(LGFX& display) {
#if HAS_CAMERA
    if (!_camera || !_camera->available()) {
        display.fillScreen(COL_BG);
        display.setTextColor(COL_RED, COL_BG);
        display.setTextSize(2);
        display.setTextDatum(middle_center);
        display.drawString("Camera not available", _w / 2, _h / 2);
        return;
    }

    CameraFrame* frame = _camera->capture();
    if (frame && frame->data) {
        // Center the camera frame on screen
        int offsetX = (_w - frame->width) / 2;
        int offsetY = (_h - frame->height) / 2;
        if (offsetX < 0) offsetX = 0;
        if (offsetY < 0) offsetY = 0;

        // Clear borders if camera is smaller than screen
        if (offsetX > 0 || offsetY > 0) {
            display.fillScreen(COL_BG);
        }

        if (frame->format == CamPixelFormat::RGB565) {
            display.pushImage(offsetX, offsetY,
                              frame->width, frame->height,
                              (uint16_t*)frame->data);
        }

        _camera->releaseFrame();

        // FPS overlay
        _camFrameCount++;
        uint32_t now = millis();
        if (now - _camFpsTimer >= FPS_UPDATE_MS) {
            _camFps = _camFrameCount * 1000.0f / (now - _camFpsTimer);
            _camFrameCount = 0;
            _camFpsTimer = now;
        }

        // Draw FPS and nav hint on top
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f fps  %dx%d",
                 _camFps, frame->width, frame->height);
        display.setTextColor(COL_GREEN, COL_BG);
        display.setTextSize(1);
        display.setTextDatum(top_right);
        display.drawString(buf, _w - 4, 4);

        // Back hint
        display.setTextDatum(top_left);
        display.setTextColor(COL_NAV_TEXT, COL_BG);
        display.drawString("< Back", 4, 4);
    } else {
        display.setTextColor(COL_YELLOW, COL_BG);
        display.setTextSize(1);
        display.setTextDatum(middle_center);
        display.drawString("Waiting for frame...", _w / 2, _h / 2);
    }

#else
    display.fillScreen(COL_BG);
    display.setTextColor(COL_TEXT_DIM, COL_BG);
    display.setTextSize(2);
    display.setTextDatum(middle_center);
    display.drawString("Camera not present", _w / 2, _h / 2);
    display.setTextSize(1);
    display.drawString("(HAS_CAMERA = 0)", _w / 2, _h / 2 + 24);
#endif
}

// ============================================================================
// Audio Test Screen
// ============================================================================

void SystemApp::drawAudioTest(LGFX_Sprite& spr) {
    drawNavBar(spr, "Audio / Mic");

#if HAS_AUDIO_CODEC
    if (!_audio || !_audio->available()) {
        spr.setTextColor(COL_RED);
        spr.setTextSize(2);
        spr.setTextDatum(middle_center);
        spr.drawString("Audio not available", _w / 2, _h / 2);
        return;
    }

    int y = NAV_BAR_H + 12;
    int barW = _w - 2 * MARGIN - 20;
    char buf[60];

    spr.setTextDatum(top_left);

    // Status line
    spr.setTextColor(COL_GREEN);
    spr.setTextSize(2);
    spr.drawString("ES8311 Codec Active", MARGIN, y);
    y += 24;

    spr.setTextSize(1);
    spr.setTextColor(COL_TEXT);
    snprintf(buf, sizeof(buf), "Sample Rate: %lu Hz  Codec: %s  Mic: %s",
             (unsigned long)_audio->getSampleRate(),
             _audio->hasCodec() ? "Yes" : "No",
             _audio->hasMic() ? "Yes" : "No");
    spr.drawString(buf, MARGIN, y);
    y += 20;

    // Divider
    spr.drawFastHLine(MARGIN, y, _w - 2 * MARGIN, COL_TEXT_DIM);
    y += 8;

    // Single mic read shared between level meter and spectrum
    static int16_t micBuf[512];
    size_t micRead = _audio->readMic(micBuf, 512);

    // Compute RMS from shared buffer
    if (micRead > 0) {
        float sum = 0.0f;
        for (size_t i = 0; i < micRead; i++) {
            float s = micBuf[i] / 32768.0f;
            sum += s * s;
        }
        _micLevel = sqrtf(sum / micRead);
        if (_micLevel > 1.0f) _micLevel = 1.0f;
    }

    // Microphone level meter
    spr.setTextColor(COL_ACCENT);
    spr.setTextSize(2);
    spr.drawString("Mic Level", MARGIN, y);
    y += 24;

    uint32_t levelColor = COL_GREEN;
    if (_micLevel > 0.7f) levelColor = COL_RED;
    else if (_micLevel > 0.4f) levelColor = COL_YELLOW;
    drawProgressBar(spr, MARGIN + 10, y, barW, 20, _micLevel, levelColor, COL_BAR_BG);

    spr.setTextSize(1);
    spr.setTextColor(COL_TEXT);
    snprintf(buf, sizeof(buf), "%.1f%%", _micLevel * 100.0f);
    spr.setTextDatum(middle_right);
    spr.drawString(buf, _w - MARGIN, y + 10);
    spr.setTextDatum(top_left);
    y += 32;

    // Spectrum visualization
    spr.setTextColor(COL_ACCENT);
    spr.setTextSize(2);
    spr.drawString("Spectrum", MARGIN, y);
    y += 24;

    // Compute spectrum from shared buffer
    if (micRead > 0) {
        _audio->getSpectrum(_spectrumBins, 16, micBuf, micRead);
    }

    // Draw spectrum bars
    int specH = 60;
    int binW = (barW - 16) / 16;
    for (int i = 0; i < 16; i++) {
        float val = _spectrumBins[i] * 10.0f; // Scale up for visibility
        if (val > 1.0f) val = 1.0f;
        int barH = (int)(val * specH);
        int bx = MARGIN + 10 + i * binW;

        // Gradient color based on frequency
        uint32_t col;
        if (i < 4) col = COL_GREEN;
        else if (i < 8) col = COL_ACCENT;
        else if (i < 12) col = COL_YELLOW;
        else col = COL_RED;

        if (barH > 0) {
            spr.fillRect(bx, y + specH - barH, binW - 2, barH, col);
        }
        spr.drawRect(bx, y, binW - 2, specH, COL_BAR_BG);
    }
    y += specH + 8;

    // Frequency labels
    spr.setTextSize(1);
    spr.setTextColor(COL_TEXT_DIM);
    spr.drawString("0", MARGIN + 10, y);
    snprintf(buf, sizeof(buf), "%d Hz", (int)(_audio->getSampleRate() / 2));
    spr.setTextDatum(top_right);
    spr.drawString(buf, _w - MARGIN - 10, y);
    spr.setTextDatum(top_left);
    y += 18;

    // Divider
    spr.drawFastHLine(MARGIN, y, _w - 2 * MARGIN, COL_TEXT_DIM);
    y += 8;

    // Tone test buttons
    spr.setTextColor(COL_ACCENT);
    spr.setTextSize(2);
    spr.drawString("Tone Test", MARGIN, y);
    y += 24;

    // Play tone button areas (touch targets)
    const uint16_t tones[] = {262, 440, 880, 1000};
    const char* toneLabels[] = {"C4", "A4", "A5", "1kHz"};
    int btnW = (_w - 2 * MARGIN - 12) / 4;
    for (int i = 0; i < 4; i++) {
        int bx = MARGIN + i * (btnW + 4);
        spr.fillRect(bx, y, btnW, 32, COL_MENU_BG);
        spr.drawRect(bx, y, btnW, 32, COL_ACCENT);
        spr.setTextColor(COL_TEXT);
        spr.setTextSize(2);
        spr.setTextDatum(middle_center);
        spr.drawString(toneLabels[i], bx + btnW / 2, y + 16);
    }
    spr.setTextDatum(top_left);
    y += 40;

    // Volume control
    spr.setTextColor(COL_TEXT);
    spr.setTextSize(1);
    snprintf(buf, sizeof(buf), "Volume: %d%%", _audio->getVolume());
    spr.drawString(buf, MARGIN, y);
    y += 14;
    float volNorm = _audio->getVolume() / 100.0f;
    drawProgressBar(spr, MARGIN + 10, y, barW, 14, volNorm, COL_ACCENT, COL_BAR_BG);

    // Handle tone request
    if (_toneRequested) {
        _toneRequested = false;
        _audio->playTone(_toneFreq, 200);
    }

#else
    spr.setTextColor(COL_TEXT_DIM);
    spr.setTextSize(2);
    spr.setTextDatum(middle_center);
    spr.drawString("Audio not present", _w / 2, _h / 2);
    spr.setTextSize(1);
    spr.drawString("(HAS_AUDIO_CODEC = 0)", _w / 2, _h / 2 + 24);
#endif
}

// ============================================================================
// Voice Commands Screen
// ============================================================================

void SystemApp::drawVoiceCommands(LGFX_Sprite& spr) {
    drawNavBar(spr, "Voice Commands");

#if HAS_AUDIO_CODEC
    if (!_voice || !_audio || !_audio->available()) {
        spr.setTextColor(COL_RED);
        spr.setTextSize(2);
        spr.setTextDatum(middle_center);
        spr.drawString("Voice HAL not ready", _w / 2, _h / 2);
        return;
    }

    int y = NAV_BAR_H + 8;
    int barW = _w - 2 * MARGIN - 20;
    char buf[80];

    spr.setTextDatum(top_left);

    // --- VAD Status ---
    VoiceState vState = _voice->getState();
    const char* stateStr = "IDLE";
    uint32_t stateColor = COL_TEXT_DIM;
    switch (vState) {
        case VoiceState::IDLE:          stateStr = "Idle";          stateColor = COL_TEXT_DIM; break;
        case VoiceState::LISTENING:     stateStr = "Listening...";  stateColor = COL_GREEN;    break;
        case VoiceState::PROCESSING:    stateStr = "Processing..."; stateColor = COL_YELLOW;   break;
        case VoiceState::COMMAND_READY: stateStr = "Detected!";     stateColor = COL_ACCENT;   break;
    }

    spr.setTextColor(stateColor);
    spr.setTextSize(2);
    snprintf(buf, sizeof(buf), "VAD: %s", stateStr);
    spr.drawString(buf, MARGIN, y);
    y += 24;

    // Mic level bar
    float level = _voice->getInstantLevel();
    spr.setTextSize(1);
    spr.setTextColor(COL_TEXT);
    spr.drawString("Mic Level:", MARGIN, y);
    uint32_t levelCol = COL_GREEN;
    if (level > 0.7f) levelCol = COL_RED;
    else if (level > 0.3f) levelCol = COL_YELLOW;
    drawProgressBar(spr, MARGIN + 70, y, barW - 60, 12, level, levelCol, COL_BAR_BG);
    y += 18;

    // VAD threshold indicator
    VADConfig vCfg = _voice->getVADConfig();
    int threshX = MARGIN + 70 + (int)((barW - 60) * vCfg.energyThreshold);
    spr.drawFastVLine(threshX, y - 18, 12, COL_RED);

    // Detection enabled indicator
    spr.setTextColor(_voice->isEnabled() ? COL_GREEN : COL_RED);
    snprintf(buf, sizeof(buf), "Detection: %s", _voice->isEnabled() ? "ON" : "OFF");
    spr.drawString(buf, MARGIN, y);
    y += 16;

    // Divider
    spr.drawFastHLine(MARGIN, y, _w - 2 * MARGIN, COL_TEXT_DIM);
    y += 8;

    // --- Command List ---
    spr.setTextColor(COL_ACCENT);
    spr.setTextSize(2);
    spr.drawString("Commands", MARGIN, y);
    y += 24;

    int cmdCount = _voice->getCommandCount();
    int rowH = 36;
    spr.setTextSize(1);

    for (int i = 0; i < cmdCount; i++) {
        int rowY = y + i * rowH;
        bool selected = (i == _voiceSelectedCmd);

        // Row background
        spr.fillRect(0, rowY, _w, rowH, selected ? COL_MENU_SEL : ((i % 2) ? COL_BG : COL_MENU_BG));

        // Command name
        spr.setTextColor(COL_TEXT);
        spr.setTextSize(2);
        spr.setTextDatum(middle_left);
        // Access command label from voice HAL
        snprintf(buf, sizeof(buf), "%d: %s", i + 1,
                 (i == 0) ? "yes" : (i == 1) ? "no" : (i == 2) ? "stop" : "go");
        spr.drawString(buf, MARGIN + 4, rowY + rowH / 2);

        // Training status indicator on right
        spr.setTextDatum(middle_right);
        spr.setTextSize(1);
        spr.setTextColor(COL_TEXT_DIM);
        spr.drawString("untrained", _w - MARGIN - 4, rowY + rowH / 2);

        // Selection marker
        if (selected) {
            spr.fillRect(0, rowY, 4, rowH, COL_ACCENT);
        }
    }
    y += cmdCount * rowH + 8;
    spr.setTextDatum(top_left);

    // Divider
    spr.drawFastHLine(MARGIN, y, _w - 2 * MARGIN, COL_TEXT_DIM);
    y += 8;

    // --- Action Buttons ---
    int btnW = (_w - 2 * MARGIN - 8) / 2;
    int btnH = 36;

    // Train button
    spr.fillRect(MARGIN, y, btnW, btnH, COL_MENU_BG);
    spr.drawRect(MARGIN, y, btnW, btnH, COL_YELLOW);
    spr.setTextColor(COL_YELLOW);
    spr.setTextSize(2);
    spr.setTextDatum(middle_center);
    spr.drawString("Train", MARGIN + btnW / 2, y + btnH / 2);

    // Listen button
    int listenX = MARGIN + btnW + 8;
    bool listening = _voice->isEnabled();
    spr.fillRect(listenX, y, btnW, btnH, listening ? COL_GREEN : COL_MENU_BG);
    spr.drawRect(listenX, y, btnW, btnH, listening ? COL_GREEN : COL_ACCENT);
    spr.setTextColor(listening ? COL_BG : COL_ACCENT);
    spr.drawString(listening ? "Stop" : "Listen", listenX + btnW / 2, y + btnH / 2);

    y += btnH + 12;
    spr.setTextDatum(top_left);

    // --- Detection Result ---
    if (_voiceHasResult) {
        spr.drawFastHLine(MARGIN, y, _w - 2 * MARGIN, COL_TEXT_DIM);
        y += 8;

        spr.setTextColor(COL_GREEN);
        spr.setTextSize(2);
        snprintf(buf, sizeof(buf), "Detected: %s", _lastVoiceDet.label ? _lastVoiceDet.label : "?");
        spr.drawString(buf, MARGIN, y);
        y += 24;

        spr.setTextSize(1);
        spr.setTextColor(COL_TEXT);
        snprintf(buf, sizeof(buf), "Confidence: %.0f%%  Time: %lums ago",
                 _lastVoiceDet.confidence * 100.0f,
                 (unsigned long)(millis() - _lastVoiceDet.timestamp));
        spr.drawString(buf, MARGIN, y);
        y += 16;

        // Confidence bar
        drawProgressBar(spr, MARGIN + 10, y, barW, 14,
                        _lastVoiceDet.confidence, COL_GREEN, COL_BAR_BG);
    } else {
        spr.drawFastHLine(MARGIN, y, _w - 2 * MARGIN, COL_TEXT_DIM);
        y += 8;
        spr.setTextColor(COL_TEXT_DIM);
        spr.setTextSize(1);
        spr.drawString("Speak a command to test detection", MARGIN, y);
        y += 14;
        spr.drawString("Train first, then enable listening", MARGIN, y);
    }

#else
    spr.setTextColor(COL_TEXT_DIM);
    spr.setTextSize(2);
    spr.setTextDatum(middle_center);
    spr.drawString("Audio not present", _w / 2, _h / 2);
    spr.setTextSize(1);
    spr.drawString("(HAS_AUDIO_CODEC = 0)", _w / 2, _h / 2 + 24);
#endif
}

// ============================================================================
// Settings Screen
// ============================================================================

void SystemApp::drawSettings(LGFX_Sprite& spr) {
    drawNavBar(spr, "Settings");

    int y = NAV_BAR_H + 12;
    int barW = _w - 2 * MARGIN - 20;
    char buf[40];

    spr.setTextDatum(top_left);

    // --- Brightness ---
    spr.setTextColor(COL_ACCENT);
    spr.setTextSize(2);
    spr.drawString("Brightness", MARGIN, y);
    y += 24;

    // Brightness slider bar
    float brtNorm = _brightness / 255.0f;
    drawProgressBar(spr, MARGIN + 10, y, barW, 20, brtNorm, COL_ACCENT, COL_BAR_BG);

    spr.setTextSize(1);
    spr.setTextColor(COL_TEXT);
    snprintf(buf, sizeof(buf), "%d", _brightness);
    spr.setTextDatum(middle_right);
    spr.drawString(buf, _w - MARGIN, y + 10);
    spr.setTextDatum(top_left);
    y += 36;

    // --- Rotation ---
    spr.setTextColor(COL_ACCENT);
    spr.setTextSize(2);
    spr.drawString("Rotation", MARGIN, y);
    y += 24;

    int segW = (_w - 2 * MARGIN) / 4;
    for (int i = 0; i < 4; i++) {
        int sx = MARGIN + i * segW;
        bool sel = (i == _rotation);
        spr.fillRect(sx + 2, y, segW - 4, 28, sel ? COL_ACCENT : COL_BAR_BG);
        spr.drawRect(sx + 2, y, segW - 4, 28, COL_TEXT_DIM);

        spr.setTextColor(sel ? COL_BG : COL_TEXT);
        spr.setTextSize(2);
        spr.setTextDatum(middle_center);
        snprintf(buf, sizeof(buf), "%d", i * 90);
        spr.drawString(buf, sx + segW / 2, y + 14);
    }
    spr.setTextDatum(top_left);
    y += 42;

    // --- Auto-sleep ---
    spr.setTextColor(COL_ACCENT);
    spr.setTextSize(2);
    spr.drawString("Auto Sleep", MARGIN, y);
    y += 24;

    spr.setTextColor(COL_TEXT);
    spr.setTextSize(2);
    if (_sleepTimeout == 0) {
        spr.drawString("Off (tap to cycle)", MARGIN + 4, y);
    } else {
        snprintf(buf, sizeof(buf), "%ds (tap to cycle)", _sleepTimeout);
        spr.drawString(buf, MARGIN + 4, y);
    }
    y += 32;

    // --- Info footer ---
    y += 8;
    spr.drawFastHLine(MARGIN, y, _w - 2 * MARGIN, COL_TEXT_DIM);
    y += 10;

    spr.setTextSize(1);
    spr.setTextColor(COL_TEXT_DIM);
    snprintf(buf, sizeof(buf), "Display: %dx%d  %s  %s",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_DRIVER, DISPLAY_IF);
    spr.drawString(buf, MARGIN, y);
}
