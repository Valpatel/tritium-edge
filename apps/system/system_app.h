#pragma once
#include "app.h"
#include "esp_lcd_panel_ops.h"
#include <cstdint>

// Conditional HAL includes based on board capabilities
#if HAS_IMU
#include "hal_imu.h"
#endif

#if HAS_PMIC
#include "hal_power.h"
#endif

#if HAS_RTC
#include "hal_rtc.h"
#endif

#if HAS_AUDIO_CODEC
#include "hal_audio.h"
#endif

class SystemApp : public App {
public:
    const char* name() override { return "System"; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;

private:
    // Software framebuffer rendering
    void clearFramebuffer(uint16_t color);
    void drawChar(int x, int y, char c, uint16_t color);
    void drawString(int x, int y, const char* str, uint16_t color);
    void drawRect(int x, int y, int w, int h, uint16_t color);
    void fillRect(int x, int y, int w, int h, uint16_t color);
    void drawHLine(int x, int y, int w, uint16_t color);
    void drawProgressBar(int x, int y, int w, int h, float value,
                         uint16_t fg, uint16_t bg);
    void pushFramebuffer();

    // Dashboard sections (return y position after drawing)
    int drawHeader(int y);
    int drawBoardInfo(int y);
    int drawMemoryInfo(int y);
    int drawIMUInfo(int y);
    int drawPowerInfo(int y);
    int drawRTCInfo(int y);
    int drawAudioInfo(int y);
    int drawFPSInfo(int y);

    // RGB565 with byte-swap for QSPI
    static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
        uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        return (c >> 8) | (c << 8);
    }

    // Color palette (pre-swapped RGB565)
    uint16_t COL_BG;
    uint16_t COL_HEADER_BG;
    uint16_t COL_TEXT;
    uint16_t COL_TEXT_DIM;
    uint16_t COL_ACCENT;
    uint16_t COL_GREEN;
    uint16_t COL_RED;
    uint16_t COL_YELLOW;
    uint16_t COL_BAR_BG;
    uint16_t COL_DIVIDER;

    // Display state
    esp_lcd_panel_handle_t _panel = nullptr;
    int _w = 0;
    int _h = 0;
    uint16_t* _framebuf = nullptr;
    uint16_t* _dma_buf = nullptr;
    static constexpr int CHUNK_ROWS = 64;

    // FPS tracking
    uint32_t _frameCount = 0;
    uint32_t _fpsTimer = 0;
    float _fps = 0.0f;

    // HAL update throttle
    uint32_t _lastHalUpdate = 0;
    static constexpr uint32_t HAL_UPDATE_MS = 200;

    // HAL instances
#if HAS_IMU
    IMUHAL* _imu = nullptr;
    float _ax = 0, _ay = 0, _az = 0;
    float _gx = 0, _gy = 0, _gz = 0;
#endif

#if HAS_PMIC
    PowerHAL* _power = nullptr;
    PowerInfo _powerInfo = {};
#endif

#if HAS_RTC
    RTCHAL* _rtc = nullptr;
    RTCTime _rtcTime = {};
#endif

#if HAS_AUDIO_CODEC
    AudioHAL* _audio = nullptr;
    float _micLevel = 0.0f;
#endif
};
