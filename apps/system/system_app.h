#pragma once
#include "app.h"
#include "debug_log.h"

// Conditional HAL includes based on board capabilities
#if HAS_IMU
#include "hal_imu.h"
#endif

#if HAS_PMIC
#include "hal_power.h"
#endif

#if HAS_SDCARD
#include "hal_sdcard.h"
#endif

#if HAS_CAMERA
#include "hal_camera.h"
#endif

#if HAS_RTC
#include "hal_rtc.h"
#endif

#if HAS_AUDIO_CODEC
#include "hal_audio.h"
#include "hal_voice.h"
#endif

#include "wifi_manager.h"

enum class SystemScreen : uint8_t {
    HOME,
    IMU,
    BATTERY,
    NETWORK,
    STORAGE,
    CAMERA_PREVIEW,
    AUDIO_TEST,
    VOICE_COMMANDS,
    SETTINGS
};

struct MenuItem {
    const char* label;
    SystemScreen screen;
    bool available;
};

class SystemApp : public App {
public:
    const char* name() override { return "System"; }
    void setup(LGFX& display) override;
    void loop(LGFX& display) override;

private:
    // Screen rendering
    void drawHome(LGFX_Sprite& spr);
    void drawIMU(LGFX_Sprite& spr);
    void drawBattery(LGFX_Sprite& spr);
    void drawNetwork(LGFX_Sprite& spr);
    void drawStorage(LGFX_Sprite& spr);
    void drawCameraPreview(LGFX& display);
    void drawAudioTest(LGFX_Sprite& spr);
    void drawVoiceCommands(LGFX_Sprite& spr);
    void drawSettings(LGFX_Sprite& spr);

    // Navigation
    void drawNavBar(LGFX_Sprite& spr, const char* title);
    void handleTouch(LGFX& display);
    void navigateTo(SystemScreen screen);

    // Helpers
    void buildMenuItems();
    void drawProgressBar(LGFX_Sprite& spr, int x, int y, int w, int h,
                         float value, uint32_t fg, uint32_t bg);
    const char* screenTitle(SystemScreen s);

    // Sprite for double-buffered rendering
    LGFX_Sprite* _canvas = nullptr;

    // Navigation state
    SystemScreen _currentScreen = SystemScreen::HOME;
    int _menuCount = 0;
    MenuItem _menuItems[9];

    // Display metrics
    int _w = 0;
    int _h = 0;

    // FPS tracking
    uint32_t _frameCount = 0;
    uint32_t _fpsTimer = 0;
    float _fps = 0.0f;

    // Touch debounce
    bool _touchDown = false;
    uint32_t _lastTouchTime = 0;

    // Scroll offset for long lists
    int _scrollOffset = 0;

    // Settings state
    int _brightness = 128;
    int _rotation = 0;
    int _sleepTimeout = 0; // 0=off, seconds

    // Network scan state
    bool _scanRequested = false;
    uint32_t _lastScanTime = 0;
    ScanResult _scanResults[WIFI_MAX_SCAN_RESULTS];
    int _scanResultCount = 0;

    // HAL object references
#if HAS_IMU
    IMUHAL* _imu = nullptr;
    bool _imuOwned = false;
    float _ax = 0, _ay = 0, _az = 0;
    float _gx = 0, _gy = 0, _gz = 0;
#endif

#if HAS_PMIC
    PowerHAL* _power = nullptr;
    bool _powerOwned = false;
#endif

#if HAS_SDCARD
    SDCardHAL* _sd = nullptr;
    bool _sdOwned = false;
#endif

#if HAS_CAMERA
    CameraHAL* _camera = nullptr;
    bool _cameraOwned = false;
    uint32_t _camFrameCount = 0;
    uint32_t _camFpsTimer = 0;
    float _camFps = 0.0f;
#endif

#if HAS_RTC
    RTCHAL* _rtc = nullptr;
    bool _rtcOwned = false;
#endif

#if HAS_AUDIO_CODEC
    AudioHAL* _audio = nullptr;
    bool _audioOwned = false;
    float _micLevel = 0.0f;
    bool _toneRequested = false;
    uint16_t _toneFreq = 440;
    bool _micMonitoring = false;
    float _spectrumBins[16] = {};

    // Voice command state
    VoiceHAL* _voice = nullptr;
    bool _voiceOwned = false;
    int _voiceTrainCmd = -1;       // Command ID being trained (-1 = none)
    int _voiceSelectedCmd = 0;     // Currently selected command in list
    VoiceDetection _lastVoiceDet = {};
    bool _voiceHasResult = false;
    uint32_t _voiceResultTime = 0; // When last result was shown
#endif

    WifiManager* _wifi = nullptr;
};
