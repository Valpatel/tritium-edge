#include "system_app.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <cstring>
#include <cstdio>

// ============================================================================
// 5x7 bitmap font (ASCII 32-126)
// ============================================================================
// Each character is 5 pixels wide, 7 pixels tall, stored as 5 column bytes.
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 space
    {0x00,0x00,0x5F,0x00,0x00}, // 33 !
    {0x00,0x07,0x00,0x07,0x00}, // 34 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
    {0x23,0x13,0x08,0x64,0x62}, // 37 %
    {0x36,0x49,0x55,0x22,0x50}, // 38 &
    {0x00,0x05,0x03,0x00,0x00}, // 39 '
    {0x00,0x1C,0x22,0x41,0x00}, // 40 (
    {0x00,0x41,0x22,0x1C,0x00}, // 41 )
    {0x08,0x2A,0x1C,0x2A,0x08}, // 42 *
    {0x08,0x08,0x3E,0x08,0x08}, // 43 +
    {0x00,0x50,0x30,0x00,0x00}, // 44 ,
    {0x08,0x08,0x08,0x08,0x08}, // 45 -
    {0x00,0x60,0x60,0x00,0x00}, // 46 .
    {0x20,0x10,0x08,0x04,0x02}, // 47 /
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
    {0x00,0x42,0x7F,0x40,0x00}, // 49 1
    {0x42,0x61,0x51,0x49,0x46}, // 50 2
    {0x21,0x41,0x45,0x4B,0x31}, // 51 3
    {0x18,0x14,0x12,0x7F,0x10}, // 52 4
    {0x27,0x45,0x45,0x45,0x39}, // 53 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
    {0x01,0x71,0x09,0x05,0x03}, // 55 7
    {0x36,0x49,0x49,0x49,0x36}, // 56 8
    {0x06,0x49,0x49,0x29,0x1E}, // 57 9
    {0x00,0x36,0x36,0x00,0x00}, // 58 :
    {0x00,0x56,0x36,0x00,0x00}, // 59 ;
    {0x00,0x08,0x14,0x22,0x41}, // 60 <
    {0x14,0x14,0x14,0x14,0x14}, // 61 =
    {0x41,0x22,0x14,0x08,0x00}, // 62 >
    {0x02,0x01,0x51,0x09,0x06}, // 63 ?
    {0x32,0x49,0x79,0x41,0x3E}, // 64 @
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
    {0x7F,0x49,0x49,0x49,0x36}, // 66 B
    {0x3E,0x41,0x41,0x41,0x22}, // 67 C
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
    {0x7F,0x49,0x49,0x49,0x41}, // 69 E
    {0x7F,0x09,0x09,0x01,0x01}, // 70 F
    {0x3E,0x41,0x41,0x51,0x32}, // 71 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
    {0x00,0x41,0x7F,0x41,0x00}, // 73 I
    {0x20,0x40,0x41,0x3F,0x01}, // 74 J
    {0x7F,0x08,0x14,0x22,0x41}, // 75 K
    {0x7F,0x40,0x40,0x40,0x40}, // 76 L
    {0x7F,0x02,0x04,0x02,0x7F}, // 77 M
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
    {0x7F,0x09,0x09,0x09,0x06}, // 80 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 82 R
    {0x46,0x49,0x49,0x49,0x31}, // 83 S
    {0x01,0x01,0x7F,0x01,0x01}, // 84 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
    {0x7F,0x20,0x18,0x20,0x7F}, // 87 W
    {0x63,0x14,0x08,0x14,0x63}, // 88 X
    {0x03,0x04,0x78,0x04,0x03}, // 89 Y
    {0x61,0x51,0x49,0x45,0x43}, // 90 Z
    {0x00,0x00,0x7F,0x41,0x41}, // 91 [
    {0x02,0x04,0x08,0x10,0x20}, // 92 backslash
    {0x41,0x41,0x7F,0x00,0x00}, // 93 ]
    {0x04,0x02,0x01,0x02,0x04}, // 94 ^
    {0x40,0x40,0x40,0x40,0x40}, // 95 _
    {0x00,0x01,0x02,0x04,0x00}, // 96 `
    {0x20,0x54,0x54,0x54,0x78}, // 97 a
    {0x7F,0x48,0x44,0x44,0x38}, // 98 b
    {0x38,0x44,0x44,0x44,0x20}, // 99 c
    {0x38,0x44,0x44,0x48,0x7F}, // 100 d
    {0x38,0x54,0x54,0x54,0x18}, // 101 e
    {0x08,0x7E,0x09,0x01,0x02}, // 102 f
    {0x08,0x14,0x54,0x54,0x3C}, // 103 g
    {0x7F,0x08,0x04,0x04,0x78}, // 104 h
    {0x00,0x44,0x7D,0x40,0x00}, // 105 i
    {0x20,0x40,0x44,0x3D,0x00}, // 106 j
    {0x00,0x7F,0x10,0x28,0x44}, // 107 k
    {0x00,0x41,0x7F,0x40,0x00}, // 108 l
    {0x7C,0x04,0x18,0x04,0x78}, // 109 m
    {0x7C,0x08,0x04,0x04,0x78}, // 110 n
    {0x38,0x44,0x44,0x44,0x38}, // 111 o
    {0x7C,0x14,0x14,0x14,0x08}, // 112 p
    {0x08,0x14,0x14,0x18,0x7C}, // 113 q
    {0x7C,0x08,0x04,0x04,0x08}, // 114 r
    {0x48,0x54,0x54,0x54,0x20}, // 115 s
    {0x04,0x3F,0x44,0x40,0x20}, // 116 t
    {0x3C,0x40,0x40,0x20,0x7C}, // 117 u
    {0x1C,0x20,0x40,0x20,0x1C}, // 118 v
    {0x3C,0x40,0x30,0x40,0x3C}, // 119 w
    {0x44,0x28,0x10,0x28,0x44}, // 120 x
    {0x0C,0x50,0x50,0x50,0x3C}, // 121 y
    {0x44,0x64,0x54,0x4C,0x44}, // 122 z
    {0x00,0x08,0x36,0x41,0x00}, // 123 {
    {0x00,0x00,0x7F,0x00,0x00}, // 124 |
    {0x00,0x41,0x36,0x08,0x00}, // 125 }
    {0x08,0x04,0x08,0x10,0x08}, // 126 ~
};

static constexpr int FONT_W = 6; // 5px char + 1px spacing
static constexpr int FONT_H = 8; // 7px char + 1px spacing
static constexpr int MARGIN = 4;
static constexpr uint32_t FPS_UPDATE_MS = 1000;

// ============================================================================
// Framebuffer drawing primitives
// ============================================================================

void SystemApp::clearFramebuffer(uint16_t color) {
    int total = _w * _h;
    for (int i = 0; i < total; i++) {
        _framebuf[i] = color;
    }
}

void SystemApp::drawChar(int x, int y, char c, uint16_t color) {
    if (c < 32 || c > 126) return;
    int idx = c - 32;
    const uint8_t* glyph = font5x7[idx];
    for (int col = 0; col < 5; col++) {
        uint8_t colBits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (colBits & (1 << row)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < _w && py >= 0 && py < _h) {
                    _framebuf[py * _w + px] = color;
                }
            }
        }
    }
}

void SystemApp::drawString(int x, int y, const char* str, uint16_t color) {
    while (*str) {
        drawChar(x, y, *str, color);
        x += FONT_W;
        str++;
    }
}

void SystemApp::fillRect(int x, int y, int w, int h, uint16_t color) {
    // Clip
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > _w) ? _w : (x + w);
    int y1 = (y + h > _h) ? _h : (y + h);
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            _framebuf[py * _w + px] = color;
        }
    }
}

void SystemApp::drawRect(int x, int y, int w, int h, uint16_t color) {
    drawHLine(x, y, w, color);
    drawHLine(x, y + h - 1, w, color);
    for (int py = y; py < y + h; py++) {
        if (py >= 0 && py < _h) {
            if (x >= 0 && x < _w) _framebuf[py * _w + x] = color;
            int rx = x + w - 1;
            if (rx >= 0 && rx < _w) _framebuf[py * _w + rx] = color;
        }
    }
}

void SystemApp::drawHLine(int x, int y, int w, uint16_t color) {
    if (y < 0 || y >= _h) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w > _w) ? _w : (x + w);
    for (int px = x0; px < x1; px++) {
        _framebuf[y * _w + px] = color;
    }
}

void SystemApp::drawProgressBar(int x, int y, int w, int h, float value,
                                 uint16_t fg, uint16_t bg) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    fillRect(x, y, w, h, bg);
    int filled = (int)(w * value);
    if (filled > 0) {
        fillRect(x, y, filled, h, fg);
    }
    drawRect(x, y, w, h, COL_TEXT_DIM);
}

void SystemApp::pushFramebuffer() {
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

void SystemApp::setup(esp_lcd_panel_handle_t panel, int width, int height) {
    _panel = panel;
    _w = width;
    _h = height;

    Serial.printf("[system] Powered by Tritium\n");
    Serial.printf("[system] Display: %dx%d\n", _w, _h);

    // Initialize color palette (byte-swapped RGB565)
    COL_BG        = rgb565(0x00, 0x00, 0x00);
    COL_HEADER_BG = rgb565(0x1A, 0x1A, 0x2E);
    COL_TEXT      = rgb565(0xDD, 0xDD, 0xDD);
    COL_TEXT_DIM  = rgb565(0x88, 0x88, 0x88);
    COL_ACCENT    = rgb565(0x00, 0x99, 0xFF);
    COL_GREEN     = rgb565(0x00, 0xCC, 0x66);
    COL_RED       = rgb565(0xFF, 0x44, 0x44);
    COL_YELLOW    = rgb565(0xFF, 0xAA, 0x00);
    COL_BAR_BG    = rgb565(0x22, 0x22, 0x22);
    COL_DIVIDER   = rgb565(0x44, 0x44, 0x44);

    // Allocate PSRAM framebuffer
    size_t fb_size = _w * _h * sizeof(uint16_t);
    _framebuf = (uint16_t*)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM);
    if (!_framebuf) {
        Serial.println("[system] FATAL: framebuffer alloc failed");
        while (1) delay(1000);
    }
    memset(_framebuf, 0, fb_size);

    // Allocate DMA transfer buffer
    size_t dma_size = _w * CHUNK_ROWS * sizeof(uint16_t);
    _dma_buf = (uint16_t*)heap_caps_malloc(dma_size, MALLOC_CAP_DMA);
    if (!_dma_buf) {
        Serial.println("[system] FATAL: DMA buffer alloc failed");
        while (1) delay(1000);
    }

    // Initialize I2C bus for sensors
    // HALs on boards with a sensor I2C bus use Wire (I2C_NUM_0)
#if defined(SENSOR_SDA) && defined(SENSOR_SCL)
    Wire.begin(SENSOR_SDA, SENSOR_SCL);
    Wire.setClock(400000);
    Serial.printf("[system] Sensor I2C: SDA=%d SCL=%d\n", SENSOR_SDA, SENSOR_SCL);
#elif defined(IMU_SDA) && defined(IMU_SCL)
    Wire.begin(IMU_SDA, IMU_SCL);
    Wire.setClock(400000);
    Serial.printf("[system] I2C: SDA=%d SCL=%d\n", IMU_SDA, IMU_SCL);
#endif

    // Initialize HALs via Arduino Wire
#if HAS_IMU
    _imu = new IMUHAL();
    if (_imu->init(Wire)) {
        Serial.printf("[system] IMU: OK (WHO_AM_I=0x%02X)\n", _imu->whoAmI());
    } else {
        Serial.println("[system] IMU: init failed");
    }
#endif

#if HAS_PMIC
    _power = new PowerHAL();
    if (_power->init(Wire)) {
        Serial.println("[system] Power: OK");
    } else {
        Serial.println("[system] Power: init failed");
    }
#endif

#if HAS_RTC
    _rtc = new RTCHAL();
    if (_rtc->init(Wire)) {
        Serial.println("[system] RTC: OK");
    } else {
        Serial.println("[system] RTC: init failed");
    }
#endif

#if HAS_AUDIO_CODEC
    _audio = new AudioHAL();
    if (_audio->init(Wire)) {
        Serial.printf("[system] Audio: OK (codec=%d, mic=%d, spk=%d)\n",
                      _audio->hasCodec(), _audio->hasMic(), _audio->hasSpeaker());
    } else {
        Serial.println("[system] Audio: init failed");
    }
#endif

    _fpsTimer = millis();
    _frameCount = 0;
    _lastHalUpdate = 0;

    Serial.println("[system] Setup complete");
}

// ============================================================================
// Loop
// ============================================================================

void SystemApp::loop() {
    uint32_t now = millis();

    // Throttle HAL reads
    if (now - _lastHalUpdate >= HAL_UPDATE_MS) {
        _lastHalUpdate = now;

#if HAS_IMU
        if (_imu && _imu->available()) {
            _imu->readAll(_ax, _ay, _az, _gx, _gy, _gz);
        }
#endif

#if HAS_PMIC
        if (_power && _power->available()) {
            _powerInfo = _power->getInfo();
        }
#endif

#if HAS_RTC
        if (_rtc && _rtc->available()) {
            _rtcTime = _rtc->getTime();
        }
#endif

#if HAS_AUDIO_CODEC
        if (_audio && _audio->available() && _audio->hasMic()) {
            _micLevel = _audio->getMicLevel();
        }
#endif
    }

    // Render dashboard
    clearFramebuffer(COL_BG);

    int y = 0;
    y = drawHeader(y);
    y = drawBoardInfo(y);
    y = drawMemoryInfo(y);
    y = drawIMUInfo(y);
    y = drawPowerInfo(y);
    y = drawRTCInfo(y);
    y = drawAudioInfo(y);
    y = drawFPSInfo(y);

    pushFramebuffer();

    // FPS tracking
    _frameCount++;
    if (now - _fpsTimer >= FPS_UPDATE_MS) {
        _fps = _frameCount * 1000.0f / (now - _fpsTimer);
        _frameCount = 0;
        _fpsTimer = now;
    }
}

// ============================================================================
// Dashboard Sections
// ============================================================================

int SystemApp::drawHeader(int y) {
    int headerH = FONT_H + 4;
    fillRect(0, y, _w, headerH, COL_HEADER_BG);
    drawString(MARGIN, y + 2, "SYSTEM DASHBOARD", COL_ACCENT);
    return y + headerH + 2;
}

int SystemApp::drawBoardInfo(int y) {
    char buf[80];
    int lineH = FONT_H + 2;

    drawString(MARGIN, y, "Board", COL_ACCENT);
    y += lineH;

    snprintf(buf, sizeof(buf), " Driver: %s  %s", DISPLAY_DRIVER, DISPLAY_IF);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    snprintf(buf, sizeof(buf), " Res: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    // Uptime
    uint32_t uptimeSec = millis() / 1000;
    uint32_t hours = uptimeSec / 3600;
    uint32_t mins = (uptimeSec % 3600) / 60;
    uint32_t secs = uptimeSec % 60;
    snprintf(buf, sizeof(buf), " Up: %luh%02lum%02lus",
             (unsigned long)hours, (unsigned long)mins, (unsigned long)secs);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    drawHLine(MARGIN, y + 1, _w - 2 * MARGIN, COL_DIVIDER);
    return y + 4;
}

int SystemApp::drawMemoryInfo(int y) {
    char buf[80];
    int lineH = FONT_H + 2;
    int barW = _w - 2 * MARGIN - 4;

    drawString(MARGIN, y, "Memory", COL_ACCENT);
    y += lineH;

    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t totalHeap = ESP.getHeapSize();
    snprintf(buf, sizeof(buf), " Heap: %luK / %luK",
             (unsigned long)(freeHeap / 1024), (unsigned long)(totalHeap / 1024));
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    float heapUsed = 1.0f - (float)freeHeap / (float)totalHeap;
    uint16_t heapColor = heapUsed > 0.85f ? COL_RED : (heapUsed > 0.7f ? COL_YELLOW : COL_GREEN);
    drawProgressBar(MARGIN + 2, y, barW, 6, heapUsed, heapColor, COL_BAR_BG);
    y += 8;

    if (ESP.getPsramSize() > 0) {
        uint32_t freePsram = ESP.getFreePsram();
        uint32_t totalPsram = ESP.getPsramSize();
        snprintf(buf, sizeof(buf), " PSRAM: %luK / %luK",
                 (unsigned long)(freePsram / 1024), (unsigned long)(totalPsram / 1024));
        drawString(MARGIN, y, buf, COL_TEXT);
        y += lineH;

        float psramUsed = 1.0f - (float)freePsram / (float)totalPsram;
        drawProgressBar(MARGIN + 2, y, barW, 6, psramUsed, COL_ACCENT, COL_BAR_BG);
        y += 8;
    }

    drawHLine(MARGIN, y + 1, _w - 2 * MARGIN, COL_DIVIDER);
    return y + 4;
}

int SystemApp::drawIMUInfo(int y) {
#if HAS_IMU
    if (!_imu || !_imu->available()) return y;

    char buf[60];
    int lineH = FONT_H + 2;

    drawString(MARGIN, y, "IMU", COL_ACCENT);
    y += lineH;

    snprintf(buf, sizeof(buf), " Acc: %+5.2f %+5.2f %+5.2f g", _ax, _ay, _az);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    snprintf(buf, sizeof(buf), " Gyr: %+6.1f %+6.1f %+6.1f", _gx, _gy, _gz);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    // Accel magnitude bar (0-2g range normalized)
    float mag = sqrtf(_ax * _ax + _ay * _ay + _az * _az);
    int barW = _w - 2 * MARGIN - 4;
    drawProgressBar(MARGIN + 2, y, barW, 6, mag / 2.0f, COL_GREEN, COL_BAR_BG);
    y += 8;

    drawHLine(MARGIN, y + 1, _w - 2 * MARGIN, COL_DIVIDER);
    return y + 4;
#else
    return y;
#endif
}

int SystemApp::drawPowerInfo(int y) {
#if HAS_PMIC
    if (!_power || !_power->available()) return y;

    char buf[60];
    int lineH = FONT_H + 2;
    int barW = _w - 2 * MARGIN - 4;

    drawString(MARGIN, y, "Power", COL_ACCENT);
    y += lineH;

    // Voltage
    snprintf(buf, sizeof(buf), " Batt: %.2fV  %d%%",
             _powerInfo.voltage, _powerInfo.percentage);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    // Battery bar
    float pct = (_powerInfo.percentage >= 0) ? _powerInfo.percentage / 100.0f : 0.0f;
    uint16_t barColor = COL_GREEN;
    if (_powerInfo.percentage < 20) barColor = COL_RED;
    else if (_powerInfo.percentage < 50) barColor = COL_YELLOW;
    drawProgressBar(MARGIN + 2, y, barW, 6, pct, barColor, COL_BAR_BG);
    y += 8;

    // Charging status
    snprintf(buf, sizeof(buf), " %s  Src: %s",
             _powerInfo.is_charging ? "Charging" : "Discharging",
             _powerInfo.source == PowerSource::USB ? "USB" :
             _powerInfo.source == PowerSource::BATTERY ? "Battery" : "Unknown");
    uint16_t chgColor = _powerInfo.is_charging ? COL_GREEN : COL_TEXT_DIM;
    drawString(MARGIN, y, buf, chgColor);
    y += lineH;

    drawHLine(MARGIN, y + 1, _w - 2 * MARGIN, COL_DIVIDER);
    return y + 4;
#else
    return y;
#endif
}

int SystemApp::drawRTCInfo(int y) {
#if HAS_RTC
    if (!_rtc || !_rtc->available()) return y;

    char buf[60];
    int lineH = FONT_H + 2;

    drawString(MARGIN, y, "RTC", COL_ACCENT);
    y += lineH;

    snprintf(buf, sizeof(buf), " %04d-%02d-%02d %02d:%02d:%02d",
             _rtcTime.year, _rtcTime.month, _rtcTime.day,
             _rtcTime.hour, _rtcTime.minute, _rtcTime.second);
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    drawHLine(MARGIN, y + 1, _w - 2 * MARGIN, COL_DIVIDER);
    return y + 4;
#else
    return y;
#endif
}

int SystemApp::drawAudioInfo(int y) {
#if HAS_AUDIO_CODEC
    if (!_audio || !_audio->available()) return y;

    char buf[60];
    int lineH = FONT_H + 2;
    int barW = _w - 2 * MARGIN - 4;

    drawString(MARGIN, y, "Audio", COL_ACCENT);
    y += lineH;

    snprintf(buf, sizeof(buf), " Codec:%s Mic:%s Spk:%s",
             _audio->hasCodec() ? "Y" : "N",
             _audio->hasMic() ? "Y" : "N",
             _audio->hasSpeaker() ? "Y" : "N");
    drawString(MARGIN, y, buf, COL_TEXT);
    y += lineH;

    if (_audio->hasMic()) {
        snprintf(buf, sizeof(buf), " Mic: %.0f%%", _micLevel * 100.0f);
        drawString(MARGIN, y, buf, COL_TEXT);
        y += lineH;

        drawProgressBar(MARGIN + 2, y, barW, 6, _micLevel, COL_YELLOW, COL_BAR_BG);
        y += 8;
    }

    drawHLine(MARGIN, y + 1, _w - 2 * MARGIN, COL_DIVIDER);
    return y + 4;
#else
    return y;
#endif
}

int SystemApp::drawFPSInfo(int y) {
    char buf[40];
    int lineH = FONT_H + 2;

    snprintf(buf, sizeof(buf), "FPS: %.1f", _fps);
    drawString(MARGIN, y, buf, COL_GREEN);
    y += lineH;

    return y;
}
