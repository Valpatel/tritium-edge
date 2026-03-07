#include "hal_power.h"

#ifdef SIMULATOR

// --- Simulator stub (fake 75% battery on USB) ---

bool PowerHAL::init() {
    _initialized = true;
    _has_adc = true;
    return true;
}

PowerInfo PowerHAL::getInfo() {
    PowerInfo info = {};
    info.voltage = 3.85f;
    info.percentage = 72;
    info.is_charging = false;
    info.is_usb_powered = true;
    info.has_battery = true;
    info.source = PowerSource::USB;
    return info;
}

float PowerHAL::getBatteryVoltage() { return 3.85f; }
int PowerHAL::getBatteryLevel() { return 72; }
bool PowerHAL::isCharging() { return false; }
bool PowerHAL::setChargeCurrent(uint16_t mA) { return false; }
void PowerHAL::poll() {}

#else // ESP32

#include <Arduino.h>
#include <Wire.h>
#include <lgfx/v1/platforms/common.hpp>

#define AXP2101_STATUS1        0x00
#define AXP2101_STATUS2        0x01
#define AXP2101_CHIP_ID        0x03
#define AXP2101_VBAT_H         0x34
#define AXP2101_VBAT_L         0x35
#define AXP2101_ICC_SET        0x62
#define AXP2101_ADC_CTRL       0x30

#define ADC_VDIV_RATIO         2.0f

bool PowerHAL::init(TwoWire &wire) {
    _wire = &wire;
    _use_lgfx = false;

#if defined(HAS_PMIC) && HAS_PMIC
    if (initAXP2101()) {
        _has_pmic = true;
        _initialized = true;
        return true;
    }
#endif

#if defined(BAT_ADC_PIN) && BAT_ADC_PIN >= 0
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    _has_adc = true;
    _initialized = true;
    return true;
#endif

#if defined(BAT_PWR_EN) && BAT_PWR_EN >= 0
    ::pinMode(BAT_PWR_EN, OUTPUT);
    ::digitalWrite(BAT_PWR_EN, HIGH);
    _initialized = true;
    return true;
#endif

    _initialized = true;
    return true;
}

bool PowerHAL::initLgfx(uint8_t i2c_port, uint8_t addr) {
    _use_lgfx = true;
    _lgfx_port = i2c_port;
    _addr = addr;
    _wire = nullptr;

#if defined(HAS_PMIC) && HAS_PMIC
    if (initAXP2101()) {
        _has_pmic = true;
        _initialized = true;
        return true;
    }
#endif

#if defined(BAT_ADC_PIN) && BAT_ADC_PIN >= 0
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    _has_adc = true;
    _initialized = true;
    return true;
#endif

    _initialized = true;
    return true;
}

bool PowerHAL::initAXP2101() {
    if (!_use_lgfx) {
#if defined(PMIC_I2C_ADDR)
        _addr = PMIC_I2C_ADDR;
#else
        _addr = 0x34;
#endif
        _wire->beginTransmission(_addr);
        if (_wire->endTransmission() != 0) return false;
    }
    uint8_t id = readReg(AXP2101_CHIP_ID);
    if ((id & 0xCF) != 0x47) return false;
    writeReg(AXP2101_ADC_CTRL, 0x03);
    return true;
}

PowerInfo PowerHAL::getInfo() {
    PowerInfo info = {};
    info.source = PowerSource::UNKNOWN;
    info.percentage = -1;

    if (_has_pmic) {
        info.voltage = axp_getBatteryVoltage();
        info.is_charging = axp_isCharging();
        info.has_battery = (info.voltage > 2.5f);
        info.is_usb_powered = true;
        info.source = info.has_battery ? PowerSource::BATTERY : PowerSource::USB;
        if (info.has_battery) info.percentage = getBatteryLevel();
    } else if (_has_adc) {
        info.voltage = adc_getBatteryVoltage();
        info.has_battery = (info.voltage > 2.5f);
        info.source = info.has_battery ? PowerSource::BATTERY : PowerSource::USB;
        if (info.has_battery) info.percentage = getBatteryLevel();
#if defined(BAT_STAT_PIN) && BAT_STAT_PIN >= 0
        info.is_charging = (::digitalRead(BAT_STAT_PIN) == LOW);
#endif
    } else {
        info.is_usb_powered = true;
        info.source = PowerSource::USB;
#if defined(BAT_STAT_PIN) && BAT_STAT_PIN >= 0
        info.is_charging = (::digitalRead(BAT_STAT_PIN) == LOW);
        info.has_battery = true;
#endif
    }
    return info;
}

float PowerHAL::getBatteryVoltage() {
    if (_has_pmic) return axp_getBatteryVoltage();
    if (_has_adc) return adc_getBatteryVoltage();
    return 0.0f;
}

int PowerHAL::getBatteryLevel() {
    float v = getBatteryVoltage();
    if (v <= 0.0f) return -1;
    if (v >= 4.15f) return 100;
    if (v >= 4.05f) return 90 + (int)((v - 4.05f) / 0.10f * 10.0f);
    if (v >= 3.80f) return 50 + (int)((v - 3.80f) / 0.25f * 40.0f);
    if (v >= 3.50f) return 15 + (int)((v - 3.50f) / 0.30f * 35.0f);
    if (v >= 3.00f) return (int)((v - 3.00f) / 0.50f * 15.0f);
    return 0;
}

bool PowerHAL::isCharging() {
    if (_has_pmic) return axp_isCharging();
#if defined(BAT_STAT_PIN) && BAT_STAT_PIN >= 0
    return ::digitalRead(BAT_STAT_PIN) == LOW;
#else
    return false;
#endif
}

bool PowerHAL::setChargeCurrent(uint16_t mA) {
    if (!_has_pmic) return false;
    uint8_t setting;
    if (mA <= 200) setting = mA / 25;
    else setting = 8 + (mA - 200) / 50;
    if (setting > 15) setting = 15;
    writeReg(AXP2101_ICC_SET, setting);
    return true;
}

void PowerHAL::poll() {
    if (!_low_cb) return;
    int level = getBatteryLevel();
    if (level < 0) return;
    if (level <= _low_threshold && !_low_warned) {
        _low_warned = true;
        _low_cb(level);
    } else if (level > _low_threshold + 5) {
        _low_warned = false;
    }
}

float PowerHAL::axp_getBatteryVoltage() {
    uint8_t h = readReg(AXP2101_VBAT_H);
    uint8_t l = readReg(AXP2101_VBAT_L);
    return ((uint16_t)(h << 8) | l) * 0.001f;
}

bool PowerHAL::axp_isCharging() {
    uint8_t status = readReg(AXP2101_STATUS2);
    return ((status >> 5) & 0x03) == 0x01;
}

void PowerHAL::writeReg(uint8_t reg, uint8_t val) {
    if (_use_lgfx) {
        uint8_t buf[2] = { reg, val };
        lgfx::i2c::transactionWrite(_lgfx_port, _addr, buf, 2, 400000);
    } else {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->write(val);
        _wire->endTransmission();
    }
}

uint8_t PowerHAL::readReg(uint8_t reg) {
    uint8_t val = 0;
    if (_use_lgfx) {
        lgfx::i2c::transactionWriteRead(_lgfx_port, _addr,
            &reg, 1, &val, 1, 400000);
    } else {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission(false);
        _wire->requestFrom(_addr, (uint8_t)1);
        if (_wire->available()) val = _wire->read();
    }
    return val;
}

float PowerHAL::adc_getBatteryVoltage() {
#if defined(BAT_ADC_PIN) && BAT_ADC_PIN >= 0
    uint32_t raw = 0;
    for (int i = 0; i < 16; i++) raw += analogRead(BAT_ADC_PIN);
    raw /= 16;
    float adc_voltage = (raw / 4095.0f) * 3.3f;
    return adc_voltage * ADC_VDIV_RATIO;
#else
    return 0.0f;
#endif
}

#endif // SIMULATOR
