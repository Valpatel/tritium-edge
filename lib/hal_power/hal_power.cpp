#include "hal_power.h"
#include "debug_log.h"

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

#include "tritium_compat.h"
#include "tritium_i2c.h"
#include "esp_adc/adc_oneshot.h"

#define AXP2101_STATUS1        0x00
#define AXP2101_STATUS2        0x01
#define AXP2101_CHIP_ID        0x03
#define AXP2101_VBAT_H         0x34
#define AXP2101_VBAT_L         0x35
#define AXP2101_ICC_SET        0x62
#define AXP2101_ADC_CTRL       0x30

// Board can override voltage divider ratio (e.g. 3.49 board uses 3x divider)
#ifndef BAT_ADC_DIVIDER
#define BAT_ADC_DIVIDER        2.0f
#endif
#define ADC_VDIV_RATIO         BAT_ADC_DIVIDER

bool PowerHAL::init() {
#if defined(HAS_PMIC) && HAS_PMIC
    if (initAXP2101()) {
        _has_pmic = true;
        _initialized = true;
        return true;
    }
#endif

#if defined(BAT_ADC_PIN) && BAT_ADC_PIN >= 0
    // ADC initialized on first read via ESP-IDF oneshot driver
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
    // Legacy API — lgfx I2C no longer used, all I2C goes through i2c0
    _addr = addr;

    DBG_INFO("power", "initLgfx (via i2c0) addr=0x%02X", addr);

#if defined(HAS_PMIC) && HAS_PMIC
    DBG_INFO("power", "Trying AXP2101 PMIC...");
    if (initAXP2101()) {
        _has_pmic = true;
        _initialized = true;
        DBG_INFO("power", "Using PMIC for battery");
        return true;
    }
    DBG_WARN("power", "AXP2101 init failed, falling through");
#endif

#if defined(BAT_ADC_PIN) && BAT_ADC_PIN >= 0
    // ADC initialized on first read via ESP-IDF oneshot driver
    _has_adc = true;
    _initialized = true;
    DBG_INFO("power", "Using ADC pin %d for battery", BAT_ADC_PIN);
    return true;
#endif

    DBG_WARN("power", "No battery source available");
    _initialized = true;
    return true;
}

bool PowerHAL::initAXP2101() {
#if defined(PMIC_I2C_ADDR)
    if (_addr == 0) _addr = PMIC_I2C_ADDR;
#else
    if (_addr == 0) _addr = 0x34;
#endif
    if (!i2c0.probe(_addr)) return false;

    // Try a few times — I2C bus may need a moment after touch init
    uint8_t id = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        id = readReg(AXP2101_CHIP_ID);
        if ((id & 0xCF) == 0x47) break;
        delay(10);
    }
    DBG_INFO("power", "AXP2101 chip ID: 0x%02X (addr=0x%02X)",
             id, _addr);
    // AXP2101 family IDs: 0x47, 0x4A, and other variants in the 0x4x range
    if ((id & 0xF0) != 0x40) {
        DBG_WARN("power", "AXP2101 not found (expected 0x4x, got 0x%02X)", id);
        return false;
    }

    // Enable VBAT and TS ADC channels
    writeReg(AXP2101_ADC_CTRL, 0x03);
    DBG_INFO("power", "AXP2101 initialized OK");
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
    // AXP2101 VBAT registers return millivolts as H8L8 (16-bit)
    return ((uint16_t)(h << 8) | l) * 0.001f;
}

bool PowerHAL::axp_isCharging() {
    uint8_t status = readReg(AXP2101_STATUS2);
    return ((status >> 5) & 0x03) == 0x01;
}

void PowerHAL::writeReg(uint8_t reg, uint8_t val) {
    i2c0.writeReg(_addr, reg, val);
}

uint8_t PowerHAL::readReg(uint8_t reg) {
    uint8_t val = 0;
    i2c0.readReg(_addr, reg, &val);
    return val;
}

float PowerHAL::adc_getBatteryVoltage() {
#if defined(BAT_ADC_PIN) && BAT_ADC_PIN >= 0
    // ESP-IDF ADC oneshot driver
    static adc_oneshot_unit_handle_t adc_handle = nullptr;
    if (!adc_handle) {
        adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
        if (adc_oneshot_new_unit(&init_cfg, &adc_handle) != ESP_OK) return 0.0f;
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        adc_channel_t channel = (adc_channel_t)(BAT_ADC_PIN - 1); // GPIO to channel mapping
        adc_oneshot_config_channel(adc_handle, channel, &chan_cfg);
    }
    uint32_t raw = 0;
    int val;
    adc_channel_t channel = (adc_channel_t)(BAT_ADC_PIN - 1);
    for (int i = 0; i < 16; i++) {
        if (adc_oneshot_read(adc_handle, channel, &val) == ESP_OK) raw += val;
    }
    raw /= 16;
    float adc_voltage = (raw / 4095.0f) * 3.3f;
    return adc_voltage * ADC_VDIV_RATIO;
#else
    return 0.0f;
#endif
}

#endif // SIMULATOR
