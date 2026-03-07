#include "hal_sleep.h"

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

void SleepHAL::displaySleep(bool sleep) {
    _display_sleeping = sleep;
}

void SleepHAL::setPeripheralPower(bool on) {
    _peripheral_power_on = on;
}

void SleepHAL::resetActivityTimer() {}
void SleepHAL::setAutoSleepTimeout(uint32_t seconds) { _auto_sleep_timeout_s = seconds; }
void SleepHAL::pollAutoSleep() {}

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
    // Placeholder: connect to your display driver's sleep/wake commands.
    // Example for boards with a backlight pin:
    //   #if defined(LCD_BL_PIN) && LCD_BL_PIN >= 0
    //       digitalWrite(LCD_BL_PIN, sleep ? LOW : HIGH);
    //   #endif
    // For AMOLED displays, send the display driver's sleep-in / sleep-out
    // command through your display HAL.
}

// ---- Peripheral power control --------------------------------------------

void SleepHAL::setPeripheralPower(bool on) {
    _peripheral_power_on = on;
    // Stub: for boards with AXP2101 PMIC, this could toggle LDO rails
    // to power down sensors, touch controller, etc.
    // Example:
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

#endif // SIMULATOR
