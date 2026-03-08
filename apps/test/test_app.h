#pragma once
#include "app.h"

enum class TestPhase : uint8_t {
    IDLE,
    RUNNING,
    COMPLETE,
};

struct TestEntry {
    const char* name;
    bool passed;
    bool ran;
    char detail[64];    // Brief result text
    uint32_t duration_ms;
};

class TestApp : public App {
public:
    const char* name() override { return "HW Test"; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;

    // Test runners (public for function pointer table)
    void testFilesystem();
    void testWatchdog();
    void testSleep();
    void testProvision();
    void testOTA();
    void testWiFi();
    void testNTP();
    void testWebServer();
    void testEspNow();
    void testSDCard();
    void testIMU();
    void testPower();
    void testRTC();
    void testAudio();
    void testCamera();

private:
    static constexpr int MAX_TESTS = 20;
    TestEntry _tests[MAX_TESTS] = {};
    int _testCount = 0;
    int _currentTest = -1;
    TestPhase _phase = TestPhase::IDLE;
    uint32_t _startTime = 0;
    uint32_t _totalTime = 0;
    int _passCount = 0;
    int _failCount = 0;
    int _scrollOffset = 0;

    esp_lcd_panel_handle_t _panel = nullptr;
    int _w = 0, _h = 0;
    uint16_t* _framebuf = nullptr;
    uint16_t* _dma_buf = nullptr;
    static constexpr int CHUNK_ROWS = 40;

    void runNextTest();
    void drawResults();
    void pushFramebuffer();
    void addResult(const char* name, bool passed, const char* detail, uint32_t ms);
};
