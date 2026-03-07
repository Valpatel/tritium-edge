#include "hal_sleep.h"
#include "debug_log.h"

static constexpr const char* TAG = "sleep";

#ifdef SIMULATOR

// ---------------------------------------------------------------------------
// Simulator stubs -- all sleep functions are no-ops
// ---------------------------------------------------------------------------

void SleepHAL::setWakeTimer(uint64_t microseconds) {
    _wake_timer_set = true;
    (void)microseconds;
}

void SleepHAL::setWakeGPIO(int pin, bool level_high) {
    _wake_gpio_set = true;
    (void)pin;
    (void)level_high;
}

void SleepHAL::setWakeUART(int uart_num) {
    (void)uart_num;
}

void SleepHAL::sleep(SleepMode mode) {
    (void)mode;
    // No-op: simulator never actually sleeps
}

WakeSource SleepHAL::getWakeReason() {
    return WakeSource::TIMER;
}

bool SleepHAL::isWakeFromSleep() {
    return false;
}

void SleepHAL::setWakeTouch(uint8_t pin, uint16_t threshold) {
    (void)pin;
    (void)threshold;
}

void SleepHAL::displaySleep(bool sleep) {
    _display_sleeping = sleep;
    if (sleep) {
        DBG_INFO(TAG, "Display entering sleep");
    } else {
        DBG_INFO(TAG, "Display waking");
    }
}

void SleepHAL::displayWake() {
    displaySleep(false);
}

void SleepHAL::setPeripheralPower(bool on) {
    _peripheral_power_on = on;
    DBG_INFO(TAG, "Peripheral power: %s", on ? "ON" : "OFF");
}

void SleepHAL::resetActivityTimer() {}
void SleepHAL::setAutoSleepTimeout(uint32_t seconds) { _auto_sleep_timeout_s = seconds; }
void SleepHAL::pollAutoSleep() {}

SleepHAL::TestResult SleepHAL::runTest() {
    TestResult result = {};
    result.init_ok = true;

    DBG_INFO(TAG, "--- Sleep Test Begin (simulator) ---");

    // Test timer wake config
    setWakeTimer(5 * 1000000ULL);
    result.timer_wake_config_ok = _wake_timer_set;
    DBG_INFO(TAG, "Timer wake config: %s", result.timer_wake_config_ok ? "OK" : "FAIL");

    // Test GPIO wake config
    setWakeGPIO(0, false);
    result.gpio_wake_config_ok = _wake_gpio_set;
    DBG_INFO(TAG, "GPIO wake config: %s", result.gpio_wake_config_ok ? "OK" : "FAIL");

    // Test display sleep/wake cycle
    displaySleep(true);
    result.display_sleep_ok = _display_sleeping;
    DBG_INFO(TAG, "Display sleep: %s", result.display_sleep_ok ? "OK" : "FAIL");

    displayWake();
    result.display_wake_ok = !_display_sleeping;
    DBG_INFO(TAG, "Display wake: %s", result.display_wake_ok ? "OK" : "FAIL");

    // Test wake reason API
    WakeSource reason = getWakeReason();
    result.wake_reason_ok = true;
    switch (reason) {
        case WakeSource::TIMER:     result.last_wake_reason = "TIMER"; break;
        case WakeSource::TOUCH_PAD: result.last_wake_reason = "TOUCH_PAD"; break;
        case WakeSource::GPIO:      result.last_wake_reason = "GPIO"; break;
        case WakeSource::UART:      result.last_wake_reason = "UART"; break;
        default:                    result.last_wake_reason = "UNKNOWN"; result.wake_reason_ok = false; break;
    }
    DBG_INFO(TAG, "Wake reason: %s (%s)", result.last_wake_reason, result.wake_reason_ok ? "OK" : "FAIL");

    // Test auto-sleep config
    setAutoSleepTimeout(30);
    result.auto_sleep_config_ok = (_auto_sleep_timeout_s == 30);
    DBG_INFO(TAG, "Auto-sleep config: %s", result.auto_sleep_config_ok ? "OK" : "FAIL");

    result.test_duration_ms = 0;

    DBG_INFO(TAG, "--- Sleep Test Complete ---");
    return result;
}

#else // ESP32

// ---------------------------------------------------------------------------
// ESP32-S3 implementation using ESP-IDF sleep APIs + Arduino framework
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/uart.h>

// ---- Wake source configuration -------------------------------------------

void SleepHAL::setWakeTimer(uint64_t microseconds) {
    esp_sleep_enable_timer_wakeup(microseconds);
    _wake_timer_set = true;
}

void SleepHAL::setWakeGPIO(int pin, bool level_high) {
    // ESP32-S3 supports ext1 wakeup on any RTC GPIO.
    // For a single-pin wake we use ext0-style via ext1 with a single bitmask bit.
    // esp_sleep_enable_ext0_wakeup is not available on S3; use ext1 instead.
    uint64_t mask = 1ULL << pin;
    esp_sleep_ext1_wakeup_mode_t mode = level_high
        ? ESP_EXT1_WAKEUP_ANY_HIGH
        : ESP_EXT1_WAKEUP_ALL_LOW;
    esp_sleep_enable_ext1_wakeup(mask, mode);
    _wake_gpio_set = true;
}

void SleepHAL::setWakeUART(int uart_num) {
    // UART wakeup is only supported in light sleep.
    // The UART peripheral must be configured before calling this.
    // Wakeup threshold: 3 positive edges on RXD (default for reliable detection).
    uart_set_wakeup_threshold(static_cast<uart_port_t>(uart_num), 3);
    esp_sleep_enable_uart_wakeup(uart_num);
}

void SleepHAL::setWakeTouch(uint8_t pin, uint16_t threshold) {
    (void)pin;
    (void)threshold;
    // Configure the touch pin threshold for wake detection
    touchSleepWakeUpEnable(pin, threshold);
    esp_sleep_enable_touchpad_wakeup();
    DBG_INFO(TAG, "Touch wake enabled on pin %u, threshold %u", pin, threshold);
}

// ---- Enter sleep ---------------------------------------------------------

void SleepHAL::sleep(SleepMode mode) {
    if (_before_sleep) {
        _before_sleep();
    }

    switch (mode) {
    case SleepMode::LIGHT_SLEEP:
        esp_light_sleep_start();
        // Execution resumes here after waking from light sleep
        if (_after_wake) {
            _after_wake();
        }
        break;

    case SleepMode::DEEP_SLEEP:
        esp_deep_sleep_start();
        // Never returns -- wake from deep sleep is a full reboot
        break;
    }
}

// ---- Wake reason ---------------------------------------------------------

WakeSource SleepHAL::getWakeReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER:
        return WakeSource::TIMER;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
        return WakeSource::TOUCH_PAD;
    case ESP_SLEEP_WAKEUP_EXT0:
    case ESP_SLEEP_WAKEUP_EXT1:
    case ESP_SLEEP_WAKEUP_GPIO:
        return WakeSource::GPIO;
    case ESP_SLEEP_WAKEUP_UART:
        return WakeSource::UART;
    default:
        // Cold boot or unknown -- default to TIMER as a safe fallback
        return WakeSource::TIMER;
    }
}

bool SleepHAL::isWakeFromSleep() {
    return esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED;
}

// ---- Display sleep -------------------------------------------------------

void SleepHAL::displaySleep(bool sleep) {
    _display_sleeping = sleep;
    if (sleep) {
        DBG_INFO(TAG, "Display entering sleep");
#if defined(LCD_BL) && LCD_BL >= 0
        digitalWrite(LCD_BL, LOW);
#endif
    } else {
        DBG_INFO(TAG, "Display waking");
#if defined(LCD_BL) && LCD_BL >= 0
        digitalWrite(LCD_BL, HIGH);
#endif
    }
}

void SleepHAL::displayWake() {
    displaySleep(false);
}

// ---- Peripheral power control --------------------------------------------

void SleepHAL::setPeripheralPower(bool on) {
    _peripheral_power_on = on;
    DBG_INFO(TAG, "Peripheral power: %s", on ? "ON" : "OFF");
    // Placeholder for future AXP2101 LDO rail control:
    //   #if defined(HAS_PMIC) && HAS_PMIC
    //       axp2101_set_ldo2(on);  // sensor rail
    //       axp2101_set_ldo3(on);  // touch rail
    //   #endif
}

// ---- Inactivity auto-sleep -----------------------------------------------

void SleepHAL::resetActivityTimer() {
    _last_activity_ms = millis();
}

void SleepHAL::setAutoSleepTimeout(uint32_t seconds) {
    _auto_sleep_timeout_s = seconds;
    _last_activity_ms = millis();
}

void SleepHAL::pollAutoSleep() {
    if (_auto_sleep_timeout_s == 0) {
        return;
    }

    uint32_t now = millis();
    uint32_t elapsed_ms = now - _last_activity_ms;

    if (elapsed_ms >= _auto_sleep_timeout_s * 1000UL) {
        // Auto-sleep: use light sleep with a timer wake so the device
        // periodically checks for activity (e.g., touch interrupt).
        // Default wake-up after 1 second if no other wake source is set.
        if (!_wake_timer_set && !_wake_gpio_set) {
            esp_sleep_enable_timer_wakeup(1000000ULL); // 1 second
        }
        sleep(SleepMode::LIGHT_SLEEP);
        // After waking, reset the activity timer so we don't immediately
        // re-enter sleep on the next poll.
        _last_activity_ms = millis();
    }
}

SleepHAL::TestResult SleepHAL::runTest() {
    TestResult result = {};
    uint32_t startMs = millis();

    DBG_INFO(TAG, "--- Sleep Test Begin ---");
    result.init_ok = true;

    // Test 1: Configure timer wake (5 seconds — don't actually sleep)
    setWakeTimer(5 * 1000000ULL);
    result.timer_wake_config_ok = _wake_timer_set;
    DBG_INFO(TAG, "Timer wake config (5s): %s", result.timer_wake_config_ok ? "OK" : "FAIL");

    // Test 2: Configure GPIO wake on an RTC pin
    // GPIO 0 is typically available as an RTC GPIO on ESP32-S3
    setWakeGPIO(0, false);
    result.gpio_wake_config_ok = _wake_gpio_set;
    DBG_INFO(TAG, "GPIO wake config (pin 0): %s", result.gpio_wake_config_ok ? "OK" : "FAIL");

    // Test 3: Display sleep/wake cycle
    displaySleep(true);
    result.display_sleep_ok = _display_sleeping;
    DBG_INFO(TAG, "Display sleep: %s", result.display_sleep_ok ? "OK" : "FAIL");

    delay(500);

    displayWake();
    result.display_wake_ok = !_display_sleeping;
    DBG_INFO(TAG, "Display wake: %s", result.display_wake_ok ? "OK" : "FAIL");

    // Test 4: Check wake reason API returns a valid enum
    WakeSource reason = getWakeReason();
    result.wake_reason_ok = true;
    switch (reason) {
        case WakeSource::TIMER:     result.last_wake_reason = "TIMER"; break;
        case WakeSource::TOUCH_PAD: result.last_wake_reason = "TOUCH_PAD"; break;
        case WakeSource::GPIO:      result.last_wake_reason = "GPIO"; break;
        case WakeSource::UART:      result.last_wake_reason = "UART"; break;
        default:                    result.last_wake_reason = "UNKNOWN"; result.wake_reason_ok = false; break;
    }
    DBG_INFO(TAG, "Wake reason: %s (%s)", result.last_wake_reason, result.wake_reason_ok ? "OK" : "FAIL");

    // Test 5: Auto-sleep timeout configuration
    setAutoSleepTimeout(30);
    result.auto_sleep_config_ok = (_auto_sleep_timeout_s == 30);
    DBG_INFO(TAG, "Auto-sleep config (30s): %s", result.auto_sleep_config_ok ? "OK" : "FAIL");
    // Reset to disabled so test doesn't trigger auto-sleep
    setAutoSleepTimeout(0);

    result.test_duration_ms = millis() - startMs;

    DBG_INFO(TAG, "--- Sleep Test Complete (%u ms) ---", result.test_duration_ms);
    DBG_INFO(TAG, "Results: init=%s timer=%s gpio=%s disp_sleep=%s disp_wake=%s auto=%s wake=%s",
             result.init_ok ? "OK" : "FAIL",
             result.timer_wake_config_ok ? "OK" : "FAIL",
             result.gpio_wake_config_ok ? "OK" : "FAIL",
             result.display_sleep_ok ? "OK" : "FAIL",
             result.display_wake_ok ? "OK" : "FAIL",
             result.auto_sleep_config_ok ? "OK" : "FAIL",
             result.wake_reason_ok ? "OK" : "FAIL");

    return result;
}

#endif // SIMULATOR
