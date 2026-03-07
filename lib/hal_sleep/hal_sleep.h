#pragma once
// Sleep and Power Management HAL
// Manages deep sleep, light sleep, and wake sources for ESP32-S3 boards.
// Supports timer, GPIO, and UART wake sources with pre/post-sleep callbacks
// for peripheral preparation (display off, PMIC config, etc.).
//
// Usage:
//   #include "hal_sleep.h"
//   SleepHAL sleepHal;
//   sleepHal.setWakeTimer(10 * 1000000ULL);  // 10 seconds
//   sleepHal.onBeforeSleep([]{ /* turn off display */ });
//   sleepHal.sleep(SleepMode::LIGHT_SLEEP);

#include <cstdint>

enum class SleepMode : uint8_t {
    LIGHT_SLEEP,    // CPU paused, RAM retained, fast wake ~1ms
    DEEP_SLEEP      // CPU off, RTC RAM only, wake = reboot
};

enum class WakeSource : uint8_t {
    TIMER       = 0x01,
    TOUCH_PAD   = 0x02,
    GPIO        = 0x04,
    UART        = 0x08
};

// Allow bitwise OR of WakeSource
inline WakeSource operator|(WakeSource a, WakeSource b) {
    return static_cast<WakeSource>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline bool operator&(WakeSource a, WakeSource b) {
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

class SleepHAL {
public:
    // Configure wake sources before sleeping
    void setWakeTimer(uint64_t microseconds);
    void setWakeGPIO(int pin, bool level_high = false);
    void setWakeUART(int uart_num = 0);
    void setWakeTouch(uint8_t pin, uint16_t threshold);

    // Pre-sleep callbacks for peripherals to prepare
    typedef void (*SleepCallback)(void);
    void onBeforeSleep(SleepCallback cb) { _before_sleep = cb; }
    void onAfterWake(SleepCallback cb) { _after_wake = cb; }

    // Enter sleep mode
    void sleep(SleepMode mode);

    // Get wake reason after deep sleep reboot
    static WakeSource getWakeReason();
    static bool isWakeFromSleep();

    // Display sleep (backlight off, display sleep command)
    void displaySleep(bool sleep);

    // Display wake (backlight on)
    void displayWake();

    // Peripheral power control
    void setPeripheralPower(bool on);

    // Inactivity auto-sleep
    void resetActivityTimer();
    void setAutoSleepTimeout(uint32_t seconds);
    void pollAutoSleep();

    // Test harness for sleep configuration
    struct TestResult {
        bool init_ok;
        bool timer_wake_config_ok;
        bool gpio_wake_config_ok;
        bool display_sleep_ok;
        bool display_wake_ok;
        bool auto_sleep_config_ok;
        bool wake_reason_ok;
        const char* last_wake_reason;
        uint32_t test_duration_ms;
    };
    // Test sleep configuration WITHOUT actually entering sleep (that would halt the test!)
    // Tests: configure wake sources, verify they're set, display sleep/wake cycle, check wake reason API
    TestResult runTest();

private:
    SleepCallback _before_sleep = nullptr;
    SleepCallback _after_wake = nullptr;
    uint32_t _auto_sleep_timeout_s = 0;
    uint32_t _last_activity_ms = 0;
    bool _wake_timer_set = false;
    bool _wake_gpio_set = false;
    bool _peripheral_power_on = true;
    bool _display_sleeping = false;
};
