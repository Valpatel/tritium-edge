#include "hal_power.h"
#include "debug_log.h"

#include <cstdio>

#if __has_include("hal_diag.h")
#include "hal_diag.h"
#define HAS_HAL_DIAG 1
#else
#define HAS_HAL_DIAG 0
#endif

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
    info.charge_state = ChargeState::NOT_CHARGING;
    info.charge_current_ma = 0.0f;
    info.pmic_temp_c = -999.0f;
    return info;
}

float PowerHAL::getBatteryVoltage() { return 3.85f; }
int PowerHAL::getBatteryLevel() { return 72; }
bool PowerHAL::isCharging() { return false; }
bool PowerHAL::setChargeCurrent(uint16_t mA) { return false; }
ChargeState PowerHAL::getChargeState() { return ChargeState::NOT_CHARGING; }
void PowerHAL::poll() {}

#else // ESP32

#include <Arduino.h>
#include <Wire.h>
#if __has_include(<lgfx/v1/platforms/common.hpp>)
#include <lgfx/v1/platforms/common.hpp>
#define HAS_LGFX_I2C 1
#else
#define HAS_LGFX_I2C 0
#endif

#define AXP2101_STATUS1        0x00
#define AXP2101_STATUS2        0x01
#define AXP2101_CHIP_ID        0x03
#define AXP2101_ADC_CTRL       0x30
#define AXP2101_VBAT_H         0x34
#define AXP2101_VBAT_L         0x35
#define AXP2101_TS_H           0x36
#define AXP2101_TS_L           0x37
#define AXP2101_ICC_H          0x38
#define AXP2101_ICC_L          0x39
#define AXP2101_ICC_SET        0x62

// Board can override voltage divider ratio (e.g. 3.49 board uses 3x divider)
#ifndef BAT_ADC_DIVIDER
#define BAT_ADC_DIVIDER        2.0f
#endif
#define ADC_VDIV_RATIO         BAT_ADC_DIVIDER

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
#if !HAS_LGFX_I2C
    return false;
#else
    _use_lgfx = true;
    _lgfx_port = i2c_port;
    _addr = addr;
    _wire = nullptr;

    DBG_INFO("power", "initLgfx port=%d addr=0x%02X", i2c_port, addr);

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
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    _has_adc = true;
    _initialized = true;
    DBG_INFO("power", "Using ADC pin %d for battery", BAT_ADC_PIN);
    return true;
#endif

    DBG_WARN("power", "No battery source available");
    _initialized = true;
    return true;
#endif // HAS_LGFX_I2C
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

    // Try a few times — lgfx I2C bus may need a moment after touch init
    uint8_t id = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        id = readReg(AXP2101_CHIP_ID);
        if ((id & 0xCF) == 0x47) break;
        delay(10);
    }
    DBG_INFO("power", "AXP2101 chip ID: 0x%02X (addr=0x%02X, lgfx=%d, port=%d)",
             id, _addr, _use_lgfx, _lgfx_port);
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
    info.charge_state = ChargeState::UNKNOWN;
    info.charge_current_ma = 0.0f;
    info.pmic_temp_c = -999.0f;

    if (_has_pmic) {
        info.voltage = axp_getBatteryVoltage();
        info.is_charging = axp_isCharging();
        info.has_battery = (info.voltage > 2.5f);
        info.is_usb_powered = true;
        info.source = info.has_battery ? PowerSource::BATTERY : PowerSource::USB;
        if (info.has_battery) info.percentage = getBatteryLevel();
        info.charge_current_ma = axp_getChargeCurrent();
        info.pmic_temp_c = axp_getPMICTemp();
        info.charge_state = getChargeState();
    } else if (_has_adc) {
        info.voltage = adc_getBatteryVoltage();
        info.has_battery = (info.voltage > 2.5f);
        info.source = info.has_battery ? PowerSource::BATTERY : PowerSource::USB;
        if (info.has_battery) info.percentage = getBatteryLevel();
#if defined(BAT_STAT_PIN) && BAT_STAT_PIN >= 0
        info.is_charging = (::digitalRead(BAT_STAT_PIN) == LOW);
        info.charge_state = info.is_charging ? ChargeState::CONSTANT_CURRENT : ChargeState::NOT_CHARGING;
#else
        info.charge_state = ChargeState::NOT_CHARGING;
#endif
    } else {
        info.is_usb_powered = true;
        info.source = PowerSource::USB;
        info.charge_state = ChargeState::NOT_CHARGING;
#if defined(BAT_STAT_PIN) && BAT_STAT_PIN >= 0
        info.is_charging = (::digitalRead(BAT_STAT_PIN) == LOW);
        info.has_battery = true;
        info.charge_state = info.is_charging ? ChargeState::CONSTANT_CURRENT : ChargeState::NOT_CHARGING;
#endif
    }
    return info;
}

ChargeState PowerHAL::getChargeState() {
    if (!_has_pmic) {
#if defined(BAT_STAT_PIN) && BAT_STAT_PIN >= 0
        return (::digitalRead(BAT_STAT_PIN) == LOW) ? ChargeState::CONSTANT_CURRENT : ChargeState::NOT_CHARGING;
#else
        return ChargeState::NOT_CHARGING;
#endif
    }
    // AXP2101 STATUS2 register bits [6:5] = charge status
    // 00 = not charging, 01 = pre-charge, 10 = CC, 11 = CV
    // Bit 3 = charge done
    uint8_t status2 = readReg(AXP2101_STATUS2);
    uint8_t charge_bits = (status2 >> 5) & 0x03;
    bool charge_done = (status2 >> 3) & 0x01;

    if (charge_done) return ChargeState::CHARGE_DONE;
    switch (charge_bits) {
        case 0x00: return ChargeState::NOT_CHARGING;
        case 0x01: return ChargeState::PRE_CHARGE;
        case 0x02: return ChargeState::CONSTANT_CURRENT;
        case 0x03: return ChargeState::CONSTANT_VOLTAGE;
        default:   return ChargeState::UNKNOWN;
    }
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
    PowerInfo info = getInfo();
    int level = info.percentage;

    // Record battery history for trend analysis
    _recordHistory(info);

    // Low-battery callback (existing behavior)
    if (_low_cb && level >= 0) {
        if (level <= _low_threshold && !_low_warned) {
            _low_warned = true;
            _low_cb(level);
        } else if (level > _low_threshold + 5) {
            _low_warned = false;
        }
    }

#if HAS_HAL_DIAG
    // ── Static trackers to avoid spamming logs every tick ───────────────
    static PowerSource s_prev_source = PowerSource::UNKNOWN;
    static bool s_warned_bat_low = false;
    static bool s_warned_bat_critical = false;
    static bool s_warned_pmic_temp_high = false;
    static bool s_warned_pmic_temp_critical = false;
    static bool s_warned_charge_anomaly = false;
    static bool s_first_poll = true;

    // ── 1. Power source change (USB <-> battery) ────────────────────────
    if (info.source != PowerSource::UNKNOWN) {
        if (!s_first_poll && info.source != s_prev_source) {
            const char* src_str = (info.source == PowerSource::USB) ? "USB" : "battery";
            hal_diag::log(hal_diag::Severity::INFO, "power",
                          "Power source changed to %s (%.2fV, %d%%)",
                          src_str, info.voltage, level);
        }
        s_prev_source = info.source;
    }

    // ── 2. Battery level warnings ───────────────────────────────────────
    if (level >= 0) {
        // Critical (< 5%)
        if (level < 5 && !s_warned_bat_critical) {
            s_warned_bat_critical = true;
            hal_diag::log(hal_diag::Severity::ERROR, "power",
                          "Battery CRITICAL: %d%% (%.2fV)", level, info.voltage);
        } else if (level >= 5) {
            s_warned_bat_critical = false;
        }

        // Low (< 20%)
        if (level < 20 && !s_warned_bat_low) {
            s_warned_bat_low = true;
            hal_diag::log(hal_diag::Severity::WARN, "power",
                          "Battery low: %d%% (%.2fV)", level, info.voltage);
        } else if (level >= 22) {
            // 2% hysteresis to avoid toggling at threshold
            s_warned_bat_low = false;
        }
    }

    // ── 3. PMIC temperature warnings ──────────────────────────────────
    if (_has_pmic) {
        float pmic_temp = axp_getPMICTemp();
        if (pmic_temp > -200.0f) {  // Valid reading
            // Critical (> 80°C)
            if (pmic_temp > 80.0f && !s_warned_pmic_temp_critical) {
                s_warned_pmic_temp_critical = true;
                hal_diag::log(hal_diag::Severity::ERROR, "power",
                              "PMIC temperature CRITICAL: %.1f C", pmic_temp);
            } else if (pmic_temp <= 78.0f) {
                s_warned_pmic_temp_critical = false;
            }

            // High (> 60°C)
            if (pmic_temp > 60.0f && !s_warned_pmic_temp_high) {
                s_warned_pmic_temp_high = true;
                hal_diag::log(hal_diag::Severity::WARN, "power",
                              "PMIC temperature high: %.1f C", pmic_temp);
            } else if (pmic_temp <= 58.0f) {
                s_warned_pmic_temp_high = false;
            }
        }

        // ── 4. Charge current anomaly (negative current on USB) ─────────
        if (info.source == PowerSource::USB) {
            float charge_ma = axp_getChargeCurrent();
            if (charge_ma < 0.0f && !s_warned_charge_anomaly) {
                s_warned_charge_anomaly = true;
                hal_diag::log(hal_diag::Severity::WARN, "power",
                              "Charge current anomaly: %.0f mA while on USB",
                              charge_ma);
            } else if (charge_ma >= 0.0f) {
                s_warned_charge_anomaly = false;
            }
        }
    }

    s_first_poll = false;
#endif // HAS_HAL_DIAG
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

float PowerHAL::axp_getPMICTemp() {
    uint8_t h = readReg(AXP2101_TS_H);
    uint8_t l = readReg(AXP2101_TS_L);
    // AXP2101 TS ADC: 14-bit value, 0.1°C per LSB, offset -273.15
    uint16_t raw = ((uint16_t)(h << 8) | l) & 0x3FFF;
    if (raw == 0 || raw == 0x3FFF) return -999.0f;  // No sensor / invalid
    return raw * 0.1f - 273.15f;
}

float PowerHAL::axp_getChargeCurrent() {
    uint8_t h = readReg(AXP2101_ICC_H);
    uint8_t l = readReg(AXP2101_ICC_L);
    // AXP2101 charge current: H8L8, milliamps
    return (float)((int16_t)((h << 8) | l));
}

void PowerHAL::writeReg(uint8_t reg, uint8_t val) {
    bool ok = false;
    uint32_t t0 = micros();
#if HAS_LGFX_I2C
    if (_use_lgfx) {
        uint8_t buf[2] = { reg, val };
        ok = lgfx::i2c::transactionWrite(_lgfx_port, _addr, buf, 2, 400000).has_value();
    } else
#endif
    {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->write(val);
        ok = (_wire->endTransmission() == 0);
    }
    int16_t latency = (int16_t)(micros() - t0);
#if HAS_HAL_DIAG
    hal_diag::report_i2c_result(_addr, ok, false, latency);
#endif
}

uint8_t PowerHAL::readReg(uint8_t reg) {
    uint8_t val = 0;
    bool ok = false;
    uint32_t t0 = micros();
#if HAS_LGFX_I2C
    if (_use_lgfx) {
        ok = lgfx::i2c::transactionWriteRead(_lgfx_port, _addr,
            &reg, 1, &val, 1, 400000).has_value();
    } else
#endif
    {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        uint8_t err = _wire->endTransmission(false);
        if (err == 0) {
            ok = (_wire->requestFrom(_addr, (uint8_t)1) == 1);
            if (_wire->available()) val = _wire->read();
        }
    }
    int16_t latency = (int16_t)(micros() - t0);
#if HAS_HAL_DIAG
    hal_diag::report_i2c_result(_addr, ok, false, latency);
#endif
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

// ---------------------------------------------------------------------------
// Platform-independent: battery history ring buffer
// ---------------------------------------------------------------------------

void PowerHAL::_recordHistory(const PowerInfo& info) {
    if (info.percentage < 0) return;  // No valid battery data
#ifdef ARDUINO
    uint32_t now = millis();
    if (_history_count > 0 && (now - _last_history_ms) < HISTORY_INTERVAL_MS) return;
    _last_history_ms = now;
#else
    uint32_t now = 0;
#endif

    BatteryReading& r = _history[_history_head];
    r.voltage = info.voltage;
    r.percentage = info.percentage;
    r.is_charging = info.is_charging;
    r.timestamp_ms = now;

    _history_head = (_history_head + 1) % BATTERY_HISTORY_SIZE;
    if (_history_count < BATTERY_HISTORY_SIZE) _history_count++;
}

int PowerHAL::getBatteryHistory(BatteryReading* out, int max_count) const {
    if (!out || max_count <= 0) return 0;
    int count = (_history_count < max_count) ? _history_count : max_count;

    // Read oldest-first from ring buffer
    int start = (_history_count < BATTERY_HISTORY_SIZE)
        ? 0
        : _history_head;

    for (int i = 0; i < count; i++) {
        int idx = (start + (_history_count - count) + i) % BATTERY_HISTORY_SIZE;
        out[i] = _history[idx];
    }
    return count;
}

int PowerHAL::getBatteryHistoryCount() const {
    return _history_count;
}

int PowerHAL::getBatteryHistoryJson(char* buf, size_t size) const {
    if (!buf || size < 3) return 0;
    int pos = snprintf(buf, size, "[");

    // Output oldest-first
    int start = (_history_count < BATTERY_HISTORY_SIZE)
        ? 0
        : _history_head;

    for (int i = 0; i < _history_count && pos < (int)size - 64; i++) {
        int idx = (start + i) % BATTERY_HISTORY_SIZE;
        const BatteryReading& r = _history[idx];
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, size - pos,
            "{\"v\":%.2f,\"p\":%d,\"c\":%s,\"t\":%lu}",
            r.voltage, r.percentage,
            r.is_charging ? "true" : "false",
            (unsigned long)r.timestamp_ms);
    }
    pos += snprintf(buf + pos, size - pos, "]");
    return pos;
}
