#pragma once
#include <cstdint>

// Task watchdog for crash recovery and system health monitoring.
class WatchdogHAL {
public:
    // Initialize watchdog with timeout in seconds (default 30s)
    bool init(uint32_t timeout_s = 30);
    void deinit();

    // Feed the watchdog — call regularly from main loop
    void feed();

    // Temporarily disable during long operations
    void pause();
    void resume();

    // Check if last reboot was from watchdog
    bool wasWatchdogReset() const;

    // System health metrics
    struct Health {
        uint32_t uptime_s;
        uint32_t free_heap;
        uint32_t min_free_heap;     // Lowest ever
        uint32_t free_psram;
        uint32_t largest_free_block;
        float heap_fragmentation;   // 0.0 = perfect, 1.0 = fully fragmented
        uint32_t loop_time_us;      // Last main loop iteration time
        uint32_t max_loop_time_us;  // Worst case loop time
        uint32_t feed_count;        // Total feeds since init
        uint32_t task_count;        // FreeRTOS task count
    };
    Health getHealth() const;

    // Record loop timing (call at start and end of loop)
    void loopStart();
    void loopEnd();

    // Test harness
    struct TestResult {
        bool init_ok;
        bool feed_ok;
        bool pause_resume_ok;
        bool health_ok;
        bool reset_reason_ok;
        Health health;
        uint32_t test_duration_ms;
    };
    TestResult runTest();

private:
    bool _active = false;
    bool _paused = false;
    uint32_t _loopStart = 0;
    uint32_t _maxLoopUs = 0;
    uint32_t _lastLoopUs = 0;
    uint32_t _feedCount = 0;
    uint32_t _timeout = 30;
};
