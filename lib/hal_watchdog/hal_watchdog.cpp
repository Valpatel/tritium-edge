#include "hal_watchdog.h"
#include "debug_log.h"

#ifdef SIMULATOR

// ---------------------------------------------------------------------------
// Simulator stubs — health returns fake data, init returns true
// ---------------------------------------------------------------------------

#include <ctime>

static uint32_t sim_start_time = 0;

bool WatchdogHAL::init(uint32_t timeout_s) {
    _timeout = timeout_s;
    _active = true;
    _paused = false;
    _feedCount = 0;
    _maxLoopUs = 0;
    _lastLoopUs = 0;
    sim_start_time = (uint32_t)time(nullptr);
    DBG_INFO("wdt", "Simulator watchdog init (timeout=%us)", timeout_s);
    return true;
}

void WatchdogHAL::deinit() {
    _active = false;
    DBG_INFO("wdt", "Simulator watchdog deinit");
}

void WatchdogHAL::feed() {
    if (_active && !_paused) {
        _feedCount++;
    }
}

void WatchdogHAL::pause() { _paused = true; }
void WatchdogHAL::resume() { _paused = false; }

bool WatchdogHAL::wasWatchdogReset() const { return false; }

WatchdogHAL::Health WatchdogHAL::getHealth() const {
    Health h = {};
    h.uptime_s = (uint32_t)time(nullptr) - sim_start_time;
    h.free_heap = 256000;
    h.min_free_heap = 200000;
    h.free_psram = 4000000;
    h.largest_free_block = 128000;
    h.heap_fragmentation = 1.0f - (128000.0f / 256000.0f);
    h.loop_time_us = _lastLoopUs;
    h.max_loop_time_us = _maxLoopUs;
    h.feed_count = _feedCount;
    h.task_count = 1;
    return h;
}

void WatchdogHAL::loopStart() {
    _loopStart = (uint32_t)(clock() / (CLOCKS_PER_SEC / 1000000));
}

void WatchdogHAL::loopEnd() {
    uint32_t now = (uint32_t)(clock() / (CLOCKS_PER_SEC / 1000000));
    _lastLoopUs = now - _loopStart;
    if (_lastLoopUs > _maxLoopUs) _maxLoopUs = _lastLoopUs;
}

WatchdogHAL::TestResult WatchdogHAL::runTest() {
    TestResult r = {};
    uint32_t start = (uint32_t)(clock() / (CLOCKS_PER_SEC / 1000));

    r.init_ok = init(30);
    for (int i = 0; i < 10; i++) feed();
    r.feed_ok = (_feedCount >= 10);
    pause();
    r.pause_resume_ok = _paused;
    resume();
    r.pause_resume_ok = r.pause_resume_ok && !_paused;
    r.health = getHealth();
    r.health_ok = (r.health.free_heap > 0);
    r.reset_reason_ok = true; // Simulator never has watchdog reset
    r.test_duration_ms = (uint32_t)(clock() / (CLOCKS_PER_SEC / 1000)) - start;
    deinit();
    return r;
}

#else // ESP32

// ---------------------------------------------------------------------------
// ESP32-S3 implementation using ESP-IDF task watchdog
// ---------------------------------------------------------------------------

#include "tritium_compat.h"
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

bool WatchdogHAL::init(uint32_t timeout_s) {
    if (_active) {
        DBG_WARN("wdt", "Already initialized");
        return true;
    }

    _timeout = timeout_s;

    // ESP-IDF 5.x watchdog API — uses config struct
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = timeout_s * 1000,
        .idle_core_mask = 0,           // Don't watch idle tasks
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_init(&wdt_config);
    if (err == ESP_ERR_INVALID_STATE) {
        // Already initialized — deinit and reinit with new timeout
        esp_task_wdt_deinit();
        err = esp_task_wdt_init(&wdt_config);
    }
    if (err != ESP_OK) {
        DBG_ERROR("wdt", "WDT init failed: %d", err);
        return false;
    }

    err = esp_task_wdt_add(nullptr);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        DBG_ERROR("wdt", "WDT add task failed: %d", err);
        return false;
    }

    _active = true;
    _paused = false;
    _feedCount = 0;
    _maxLoopUs = 0;
    _lastLoopUs = 0;

    DBG_INFO("wdt", "Watchdog initialized (timeout=%us)", timeout_s);
    return true;
}

void WatchdogHAL::deinit() {
    if (!_active) return;

    esp_task_wdt_delete(nullptr);
    _active = false;
    DBG_INFO("wdt", "Watchdog deinitialized");
}

void WatchdogHAL::feed() {
    if (_active && !_paused) {
        esp_task_wdt_reset();
        _feedCount++;
    }
}

void WatchdogHAL::pause() {
    if (_active && !_paused) {
        esp_task_wdt_delete(nullptr);
        _paused = true;
        DBG_DEBUG("wdt", "Watchdog paused");
    }
}

void WatchdogHAL::resume() {
    if (_active && _paused) {
        esp_task_wdt_add(nullptr);
        esp_task_wdt_reset();
        _paused = false;
        DBG_DEBUG("wdt", "Watchdog resumed");
    }
}

bool WatchdogHAL::wasWatchdogReset() const {
    esp_reset_reason_t reason = esp_reset_reason();
    return (reason == ESP_RST_TASK_WDT ||
            reason == ESP_RST_INT_WDT  ||
            reason == ESP_RST_WDT);
}

WatchdogHAL::Health WatchdogHAL::getHealth() const {
    Health h = {};

    h.uptime_s = millis() / 1000;
    h.free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    h.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    h.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    h.largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    h.heap_fragmentation = (h.free_heap > 0)
        ? 1.0f - ((float)h.largest_free_block / (float)h.free_heap)
        : 1.0f;
    h.loop_time_us = _lastLoopUs;
    h.max_loop_time_us = _maxLoopUs;
    h.feed_count = _feedCount;
    h.task_count = uxTaskGetNumberOfTasks();

    return h;
}

void WatchdogHAL::loopStart() {
    _loopStart = micros();
}

void WatchdogHAL::loopEnd() {
    uint32_t now = micros();
    _lastLoopUs = now - _loopStart;
    if (_lastLoopUs > _maxLoopUs) {
        _maxLoopUs = _lastLoopUs;
    }
}

WatchdogHAL::TestResult WatchdogHAL::runTest() {
    TestResult r = {};
    uint32_t start = millis();

    // Test init
    r.init_ok = init(30);

    // Test feed (10 times)
    if (r.init_ok) {
        uint32_t before = _feedCount;
        for (int i = 0; i < 10; i++) {
            feed();
        }
        r.feed_ok = (_feedCount >= before + 10);
    }

    // Test pause/resume
    if (r.init_ok) {
        pause();
        bool was_paused = _paused;
        resume();
        r.pause_resume_ok = was_paused && !_paused;
    }

    // Test health metrics
    r.health = getHealth();
    r.health_ok = (r.health.free_heap > 0) &&
                  (r.health.uptime_s >= 0) &&
                  (r.health.task_count > 0);

    // Test reset reason check (just verify it doesn't crash)
    r.reset_reason_ok = true;
    (void)wasWatchdogReset();

    r.test_duration_ms = millis() - start;

    DBG_INFO("wdt", "Test complete: init=%d feed=%d pause=%d health=%d reset=%d (%ums)",
             r.init_ok, r.feed_ok, r.pause_resume_ok, r.health_ok,
             r.reset_reason_ok, r.test_duration_ms);

    return r;
}

#endif // SIMULATOR
