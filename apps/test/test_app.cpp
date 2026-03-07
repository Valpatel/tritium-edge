#include "test_app.h"
#ifdef SIMULATOR
#include "sim_hal.h"
#else
#include <Arduino.h>
#include <WiFi.h>
#endif
#include "debug_log.h"

// Always-available HALs
#include "hal_fs.h"
#include "hal_watchdog.h"
#include "hal_sleep.h"
#include "hal_provision.h"
#include "hal_ota.h"
#include "hal_ntp.h"
#include "hal_webserver.h"
#include "hal_espnow.h"

// Board-specific HALs
#if HAS_SDCARD
#include "hal_sdcard.h"
#endif
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
#if HAS_CAMERA
#include "hal_camera.h"
#endif

// Neon dark theme colors (RGB565)
static constexpr uint16_t COL_BG      = 0x0000;  // Black
static constexpr uint16_t COL_CYAN    = 0x07FF;  // Neon cyan for headers
static constexpr uint16_t COL_PASS    = 0x07E0;  // Neon green for pass
static constexpr uint16_t COL_FAIL    = 0xF800;  // Red for fail
static constexpr uint16_t COL_DETAIL  = 0x7BEF;  // Gray for detail text
static constexpr uint16_t COL_YELLOW  = 0xFFE0;  // Yellow for running indicator
static constexpr uint16_t COL_WHITE   = 0xFFFF;  // White for summary

static constexpr int LINE_HEIGHT = 10;
static constexpr int HEADER_HEIGHT = 16;
static constexpr int MARGIN = 4;

// ============================================================================
// Test sequence — defines execution order
// ============================================================================

// Each entry is an index into the test dispatch table. We build the sequence
// at setup time so conditional HALs are included only when available.

typedef void (TestApp::*TestFn)();

struct TestDescriptor {
    const char* label;
    TestFn fn;
};

// Master list of tests in execution order
static const TestDescriptor ALL_TESTS[] = {
    { "LittleFS",     &TestApp::testFilesystem },
    { "Watchdog",     &TestApp::testWatchdog },
    { "Sleep Cfg",    &TestApp::testSleep },
    { "Provision",    &TestApp::testProvision },
#if HAS_SDCARD
    { "SD Card",      &TestApp::testSDCard },
#endif
    { "WiFi Scan",    &TestApp::testWiFi },
    { "ESP-NOW",      &TestApp::testEspNow },
    { "NTP",          &TestApp::testNTP },
    { "OTA",          &TestApp::testOTA },
    { "WebServer",    &TestApp::testWebServer },
#if HAS_IMU
    { "IMU",          &TestApp::testIMU },
#endif
#if HAS_PMIC
    { "Power",        &TestApp::testPower },
#endif
#if HAS_RTC
    { "RTC",          &TestApp::testRTC },
#endif
#if HAS_AUDIO_CODEC
    { "Audio",        &TestApp::testAudio },
#endif
#if HAS_CAMERA
    { "Camera",       &TestApp::testCamera },
#endif
};

static constexpr int TOTAL_TESTS = sizeof(ALL_TESTS) / sizeof(ALL_TESTS[0]);

// ============================================================================
// Setup
// ============================================================================

void TestApp::setup(LGFX& display) {
    DBG_INFO("test", "App: %s", name());

    _w = display.width();
    _h = display.height();

    _canvas = new LGFX_Sprite(&display);
    _canvas->setPsram(true);
    _canvas->setColorDepth(16);

    if (!_canvas->createSprite(_w, _h)) {
        DBG_ERROR("test", "Failed to create sprite %dx%d", _w, _h);
        // Fallback to smaller sprite
        _canvas->createSprite(_w, _h / 2);
    }

    // Reset state
    _testCount = 0;
    _currentTest = 0;
    _passCount = 0;
    _failCount = 0;
    _scrollOffset = 0;
    _phase = TestPhase::RUNNING;
    _startTime = millis();

    memset(_tests, 0, sizeof(_tests));

    // Draw initial screen
    drawResults();
}

// ============================================================================
// Loop — run one test per frame
// ============================================================================

void TestApp::loop(LGFX& display) {
    if (_phase == TestPhase::RUNNING) {
        if (_currentTest < TOTAL_TESTS) {
            runNextTest();
            _currentTest++;
        } else {
            // All tests complete
            _totalTime = millis() - _startTime;
            _phase = TestPhase::COMPLETE;
            DBG_INFO("test", "Complete: %d/%d passed in %lums",
                     _passCount, _testCount, _totalTime);
        }
        drawResults();
    }
    // In COMPLETE phase, just idle (results remain on screen)
}

// ============================================================================
// Test dispatch
// ============================================================================

void TestApp::runNextTest() {
    if (_currentTest < 0 || _currentTest >= TOTAL_TESTS) return;
    const auto& desc = ALL_TESTS[_currentTest];
    DBG_INFO("test", "Running: %s", desc.label);
    (this->*desc.fn)();
}

// ============================================================================
// Result recording
// ============================================================================

void TestApp::addResult(const char* resultName, bool passed, const char* detail, uint32_t ms) {
    if (_testCount >= MAX_TESTS) return;
    TestEntry& e = _tests[_testCount];
    e.name = resultName;
    e.passed = passed;
    e.ran = true;
    e.duration_ms = ms;
    strncpy(e.detail, detail, sizeof(e.detail) - 1);
    e.detail[sizeof(e.detail) - 1] = '\0';
    if (passed) _passCount++; else _failCount++;
    _testCount++;
}

// ============================================================================
// Display rendering — neon dark theme
// ============================================================================

void TestApp::drawResults() {
    if (!_canvas) return;
    LGFX_Sprite& spr = *_canvas;

    spr.fillSprite(COL_BG);

    // Title bar
    spr.setTextColor(COL_CYAN);
    spr.setTextSize(1);
    spr.setTextDatum(textdatum_t::top_center);
    spr.drawString("Hardware Tests", _w / 2, 2);
    spr.drawFastHLine(0, HEADER_HEIGHT - 2, _w, COL_CYAN);

    // Running indicator
    if (_phase == TestPhase::RUNNING && _currentTest < TOTAL_TESTS) {
        spr.setTextColor(COL_YELLOW);
        spr.setTextDatum(textdatum_t::top_right);
        char running[32];
        snprintf(running, sizeof(running), "[%d/%d]", _currentTest + 1, TOTAL_TESTS);
        spr.drawString(running, _w - MARGIN, 2);
    }

    // Test results list
    spr.setTextDatum(textdatum_t::top_left);
    int y = HEADER_HEIGHT + 2;
    int maxVisible = (_h - HEADER_HEIGHT - 20) / LINE_HEIGHT;

    // Auto-scroll to keep latest test visible
    if (_testCount > maxVisible) {
        _scrollOffset = _testCount - maxVisible;
    }

    for (int i = _scrollOffset; i < _testCount && y + LINE_HEIGHT <= _h - 18; i++) {
        const TestEntry& e = _tests[i];

        // Status tag
        if (e.passed) {
            spr.setTextColor(COL_PASS);
            spr.drawString("[PASS]", MARGIN, y);
        } else {
            spr.setTextColor(COL_FAIL);
            spr.drawString("[FAIL]", MARGIN, y);
        }

        // Test name
        spr.setTextColor(COL_WHITE);
        spr.drawString(e.name, MARGIN + 38, y);

        // Duration
        if (e.duration_ms > 0) {
            char dur[16];
            snprintf(dur, sizeof(dur), "%lums", (unsigned long)e.duration_ms);
            spr.setTextColor(COL_CYAN);
            int durX = _w / 2 + 10;
            spr.drawString(dur, durX, y);
        }

        // Detail text (truncated to fit)
        spr.setTextColor(COL_DETAIL);
        int detailX = MARGIN + 2;
        y += LINE_HEIGHT;
        if (y + LINE_HEIGHT <= _h - 18) {
            // Indent detail line slightly
            spr.drawString(e.detail, detailX + 4, y);
            y += LINE_HEIGHT;
        }
    }

    // Summary bar at bottom
    if (_phase == TestPhase::COMPLETE) {
        spr.drawFastHLine(0, _h - 16, _w, COL_CYAN);
        char summary[64];
        snprintf(summary, sizeof(summary), "%d/%d passed in %lums",
                 _passCount, _testCount, (unsigned long)_totalTime);
        spr.setTextColor(_failCount == 0 ? COL_PASS : COL_FAIL);
        spr.setTextDatum(textdatum_t::top_center);
        spr.drawString(summary, _w / 2, _h - 14);
    } else if (_phase == TestPhase::RUNNING) {
        spr.drawFastHLine(0, _h - 16, _w, COL_DETAIL);
        spr.setTextColor(COL_YELLOW);
        spr.setTextDatum(textdatum_t::top_center);
        spr.drawString("Running tests...", _w / 2, _h - 14);
    }

    spr.pushSprite(0, 0);
}

// ============================================================================
// Individual test implementations
// ============================================================================

void TestApp::testFilesystem() {
    uint32_t t0 = millis();
    FsHAL fs;
    if (!fs.init()) {
        addResult("LittleFS", false, "init failed", millis() - t0);
        return;
    }
    auto r = fs.runTest(5, 1024);
    char detail[64];
    snprintf(detail, sizeof(detail), "%d cyc W:%luus R:%luus %s",
             r.cycles_completed,
             (unsigned long)r.write_us,
             (unsigned long)r.read_us,
             r.verify_ok ? "verified" : "CORRUPT");
    addResult("LittleFS", r.write_ok && r.read_ok && r.verify_ok,
              detail, (r.write_us + r.read_us) / 1000);
    fs.deinit();
}

void TestApp::testWatchdog() {
    uint32_t t0 = millis();
    WatchdogHAL wd;
    auto r = wd.runTest();
    char detail[64];
    snprintf(detail, sizeof(detail), "heap:%luK frag:%.0f%% tasks:%lu",
             (unsigned long)(r.health.free_heap / 1024),
             r.health.heap_fragmentation * 100.0f,
             (unsigned long)r.health.task_count);
    bool ok = r.init_ok && r.feed_ok && r.health_ok;
    addResult("Watchdog", ok, detail, r.test_duration_ms);
}

void TestApp::testSleep() {
    uint32_t t0 = millis();
    SleepHAL sleep;
    auto r = sleep.runTest();
    char detail[64];
    snprintf(detail, sizeof(detail), "tmr:%s gpio:%s disp:%s wake:%s",
             r.timer_wake_config_ok ? "ok" : "X",
             r.gpio_wake_config_ok ? "ok" : "X",
             r.display_sleep_ok ? "ok" : "X",
             r.last_wake_reason ? r.last_wake_reason : "?");
    bool ok = r.init_ok && r.timer_wake_config_ok && r.gpio_wake_config_ok;
    addResult("Sleep Cfg", ok, detail, r.test_duration_ms);
}

void TestApp::testProvision() {
    uint32_t t0 = millis();
    ProvisionHAL prov;
    auto r = prov.runTest();
    char detail[64];
    const char* stateStr = "?";
    switch (r.state) {
        case ProvisionState::UNPROVISIONED: stateStr = "unprov"; break;
        case ProvisionState::PROVISIONED:   stateStr = "prov";   break;
        case ProvisionState::ERROR:         stateStr = "err";    break;
    }
    snprintf(detail, sizeof(detail), "%s fs:%s cert:%s id:%s",
             stateStr,
             r.fs_ok ? "ok" : "X",
             r.cert_verify_ok ? "ok" : "X",
             r.identity_ok ? "ok" : "X");
    bool ok = r.init_ok && r.fs_ok && r.cert_write_ok && r.cert_read_ok;
    addResult("Provision", ok, detail, r.test_duration_ms);
}

void TestApp::testOTA() {
    uint32_t t0 = millis();
    OtaHAL ota;
    auto r = ota.runTest();
    char detail[64];
    snprintf(detail, sizeof(detail), "part:%s max:%luK srv:%s rb:%s",
             r.running_partition ? r.running_partition : "?",
             (unsigned long)(r.max_firmware_size / 1024),
             r.server_start_ok ? "ok" : "X",
             r.rollback_check_ok ? "ok" : "X");
    bool ok = r.partition_ok && r.server_start_ok && r.server_stop_ok;
    addResult("OTA", ok, detail, r.test_duration_ms);
}

void TestApp::testWiFi() {
    uint32_t t0 = millis();
#ifdef SIMULATOR
    addResult("WiFi Scan", true, "sim: skipped", 0);
#else
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, false, false, 3000);
    uint32_t elapsed = millis() - t0;
    char detail[64];
    snprintf(detail, sizeof(detail), "Found %d networks", n);
    addResult("WiFi Scan", n >= 0, detail, elapsed);
    WiFi.mode(WIFI_OFF);
#endif
}

void TestApp::testNTP() {
    uint32_t t0 = millis();
#ifdef SIMULATOR
    addResult("NTP", true, "sim: skipped", 0);
    return;
#else
    // NTP requires WiFi — check if connected
    if (WiFi.status() != WL_CONNECTED) {
        addResult("NTP", false, "needs WiFi connection", millis() - t0);
        return;
    }
    NtpHAL ntp;
    auto r = ntp.runTest();
    char detail[64];
    snprintf(detail, sizeof(detail), "sync:%s t:%s %lums",
             r.sync_ok ? "ok" : "X",
             r.time_valid ? r.time_str : "invalid",
             (unsigned long)r.sync_time_ms);
    addResult("NTP", r.init_ok && r.sync_ok && r.time_valid,
              detail, r.test_duration_ms);
#endif
}

void TestApp::testWebServer() {
    uint32_t t0 = millis();
    WebServerHAL web;
    auto r = web.runTest();
    char detail[64];
    snprintf(detail, sizeof(detail), "port:%u mdns:%s dash:%s api:%s",
             r.port,
             r.mdns_ok ? "ok" : "X",
             r.dashboard_ok ? "ok" : "X",
             r.api_ok ? "ok" : "X");
    addResult("WebServer", r.init_ok, detail, r.test_duration_ms);
    web.stop();
}

void TestApp::testEspNow() {
    uint32_t t0 = millis();
    EspNowHAL espnow;
    // Short discovery — 3 seconds to avoid blocking too long
    auto r = espnow.runTest(3);
    char detail[64];
    snprintf(detail, sizeof(detail), "mac:%s bcast:%s peers:%d tx:%lu rx:%lu",
             r.mac_ok ? "ok" : "X",
             r.broadcast_ok ? "ok" : "X",
             r.peers_found,
             (unsigned long)r.stats.tx_count,
             (unsigned long)r.stats.rx_count);
    bool ok = r.init_ok && r.mac_ok;
    addResult("ESP-NOW", ok, detail, r.test_duration_ms);
    espnow.deinit();
}

// ============================================================================
// Board-specific HAL tests
// ============================================================================

void TestApp::testSDCard() {
#if HAS_SDCARD
    uint32_t t0 = millis();
    SDCardHAL sd;
    if (!sd.init()) {
        addResult("SD Card", false, "init failed (no card?)", millis() - t0);
        return;
    }
    auto r = sd.runTest(5, 4096);
    char detail[64];
    snprintf(detail, sizeof(detail), "%d cyc W:%luKB/s R:%luKB/s %s",
             r.cycles_completed,
             (unsigned long)r.write_speed_kbps,
             (unsigned long)r.read_speed_kbps,
             r.verify_ok ? "verified" : "CORRUPT");
    bool ok = r.mount_ok && r.write_ok && r.read_ok && r.verify_ok;
    addResult("SD Card", ok, detail, r.test_duration_ms);
    sd.deinit();
#else
    addResult("SD Card", false, "not available on this board", 0);
#endif
}

void TestApp::testIMU() {
#if HAS_IMU
    uint32_t t0 = millis();
    IMUHAL imu;
#ifdef SIMULATOR
    bool initOk = imu.init();
#else
    bool initOk = imu.initLgfx(0);
#endif
    if (!initOk) {
        addResult("IMU", false, "init failed", millis() - t0);
        return;
    }
    // Read a burst of samples and measure timing
    float ax, ay, az, gx, gy, gz;
    uint32_t readStart = millis();
    int successCount = 0;
    static constexpr int READ_CYCLES = 100;
    for (int i = 0; i < READ_CYCLES; i++) {
        if (imu.readAll(ax, ay, az, gx, gy, gz)) successCount++;
    }
    uint32_t readTime = millis() - readStart;
    uint8_t whoami = imu.whoAmI();
    char detail[64];
    snprintf(detail, sizeof(detail), "id:0x%02X %d/%d reads %lums a:%.1f,%.1f,%.1f",
             whoami, successCount, READ_CYCLES, (unsigned long)readTime,
             ax, ay, az);
    addResult("IMU", successCount > 0, detail, millis() - t0);
#else
    addResult("IMU", false, "not available on this board", 0);
#endif
}

void TestApp::testPower() {
#if HAS_PMIC
    uint32_t t0 = millis();
    PowerHAL pwr;
#ifdef SIMULATOR
    bool initOk = pwr.init();
#else
    bool initOk = pwr.initLgfx(0);
#endif
    if (!initOk) {
        addResult("Power", false, "init failed", millis() - t0);
        return;
    }
    PowerInfo info = pwr.getInfo();
    char detail[64];
    snprintf(detail, sizeof(detail), "%.2fV %d%% %s %s",
             info.voltage, info.percentage,
             info.is_charging ? "chrg" : "dis",
             info.is_usb_powered ? "USB" : "BAT");
    addResult("Power", pwr.available(), detail, millis() - t0);
#else
    addResult("Power", false, "not available on this board", 0);
#endif
}

void TestApp::testRTC() {
#if HAS_RTC
    uint32_t t0 = millis();
    RTCHAL rtc;
#ifdef SIMULATOR
    bool initOk = rtc.init();
#else
    bool initOk = rtc.initLgfx(0);
#endif
    if (!initOk) {
        addResult("RTC", false, "init failed", millis() - t0);
        return;
    }
    RTCTime now = rtc.getTime();
    char detail[64];
    snprintf(detail, sizeof(detail), "%04u-%02u-%02u %02u:%02u:%02u",
             now.year, now.month, now.day,
             now.hour, now.minute, now.second);
    // Sanity: year should be > 2020
    bool ok = rtc.available() && now.year > 2020;
    addResult("RTC", ok, detail, millis() - t0);
#else
    addResult("RTC", false, "not available on this board", 0);
#endif
}

void TestApp::testAudio() {
#if HAS_AUDIO_CODEC
    uint32_t t0 = millis();
    AudioHAL audio;
#ifdef SIMULATOR
    bool initOk = audio.init();
#else
    bool initOk = audio.initLgfx(0);
#endif
    if (!initOk) {
        addResult("Audio", false, "init failed", millis() - t0);
        return;
    }
    bool volOk = audio.setVolume(50);
    float micLevel = audio.getMicLevel();
    char detail[64];
    snprintf(detail, sizeof(detail), "codec:%s mic:%s spk:%s vol:%s lvl:%.2f",
             audio.hasCodec() ? "ok" : "X",
             audio.hasMic() ? "ok" : "X",
             audio.hasSpeaker() ? "ok" : "X",
             volOk ? "ok" : "X",
             micLevel);
    addResult("Audio", audio.available(), detail, millis() - t0);
#else
    addResult("Audio", false, "not available on this board", 0);
#endif
}

void TestApp::testCamera() {
#if HAS_CAMERA
    uint32_t t0 = millis();
    CameraHAL cam;
    if (!cam.init(CamResolution::QQVGA_160x120, CamPixelFormat::RGB565)) {
        addResult("Camera", false, "init failed", millis() - t0);
        return;
    }
    // Capture a few frames and measure throughput
    uint32_t capStart = millis();
    int frameCount = 0;
    size_t totalBytes = 0;
    static constexpr int CAP_CYCLES = 10;
    for (int i = 0; i < CAP_CYCLES; i++) {
        CameraFrame* f = cam.capture();
        if (f && f->data) {
            frameCount++;
            totalBytes += f->len;
            cam.releaseFrame();
        }
    }
    uint32_t capTime = millis() - capStart;
    float fps = capTime > 0 ? (frameCount * 1000.0f / capTime) : 0;
    char detail[64];
    snprintf(detail, sizeof(detail), "%dx%d %d/%d frames %.1ffps %luKB",
             cam.getWidth(), cam.getHeight(),
             frameCount, CAP_CYCLES, fps,
             (unsigned long)(totalBytes / 1024));
    addResult("Camera", frameCount > 0, detail, millis() - t0);
    cam.deinit();
#else
    addResult("Camera", false, "not available on this board", 0);
#endif
}
