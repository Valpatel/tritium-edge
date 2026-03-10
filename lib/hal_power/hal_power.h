#pragma once
// Battery and Power Management HAL
// Supports AXP2101 PMIC, ADC battery voltage, and charge status pins
//
// Usage:
//   #include "hal_power.h"
//   PowerHAL power;
//   power.init();               // ESP32 (uses global i2c0)
//   power.initLgfx(0, 0x34);   // Legacy API (now uses i2c0 internally)
//   power.init();               // simulator

#include <cstdint>
#include <cstddef>

enum class PowerSource : uint8_t {
    UNKNOWN,
    USB,
    BATTERY
};

struct PowerInfo {
    float voltage;
    int percentage;       // 0-100, -1 if unknown
    bool is_charging;
    bool is_usb_powered;
    bool has_battery;
    PowerSource source;
};

typedef void (*LowBatteryCallback)(int percentage);

class PowerHAL {
public:
#ifdef SIMULATOR
    bool init();
#else
    bool init();
    bool initLgfx(uint8_t i2c_port, uint8_t addr = 0x34);
#endif
    PowerInfo getInfo();
    float getBatteryVoltage();
    int getBatteryLevel();
    bool isCharging();
    bool setChargeCurrent(uint16_t mA);
    bool hasPMIC() const { return _has_pmic; }
    bool hasADC() const { return _has_adc; }
    bool available() const { return _initialized; }

    void setLowBatteryThreshold(int percent) { _low_threshold = percent; }
    void onLowBattery(LowBatteryCallback cb) { _low_cb = cb; }
    void poll();

private:
    bool _initialized = false;
    bool _has_pmic = false;
    bool _has_adc = false;
    int _low_threshold = 15;
    bool _low_warned = false;
    LowBatteryCallback _low_cb = nullptr;

#ifndef SIMULATOR
    uint8_t _addr = 0;

    bool initAXP2101();
    float axp_getBatteryVoltage();
    bool axp_isCharging();
    void writeReg(uint8_t reg, uint8_t val);
    uint8_t readReg(uint8_t reg);
    float adc_getBatteryVoltage();
#endif
};
