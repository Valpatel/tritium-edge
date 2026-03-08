#include "hal_rtc.h"

#ifdef SIMULATOR

#include <ctime>

// --- Simulator stub (uses system clock) ---

bool RTCHAL::init() {
    _initialized = true;
    return true;
}

RTCTime RTCHAL::getTime() {
    RTCTime t = {};
    if (!_initialized) return t;
    time_t now = time(nullptr);
    struct tm *tm = localtime(&now);
    t.year    = 1900 + tm->tm_year;
    t.month   = 1 + tm->tm_mon;
    t.day     = tm->tm_mday;
    t.hour    = tm->tm_hour;
    t.minute  = tm->tm_min;
    t.second  = tm->tm_sec;
    t.weekday = tm->tm_wday;
    return t;
}

bool RTCHAL::setTime(const RTCTime &t) { return _initialized; }
bool RTCHAL::setAlarm(uint8_t hour, uint8_t minute) { return _initialized; }
bool RTCHAL::clearAlarm() { return _initialized; }

#else // ESP32

#include <Arduino.h>
#include <Wire.h>
#if __has_include(<lgfx/v1/platforms/common.hpp>)
#include <lgfx/v1/platforms/common.hpp>
#define HAS_LGFX_I2C 1
#else
#define HAS_LGFX_I2C 0
#endif

#ifndef HAS_RTC
#define HAS_RTC 0
#endif

#define PCF85063_CTRL1     0x00
#define PCF85063_CTRL2     0x01
#define PCF85063_SECONDS   0x04
#define PCF85063_MINUTES   0x05
#define PCF85063_HOURS     0x06
#define PCF85063_DAYS      0x07
#define PCF85063_WEEKDAYS  0x08
#define PCF85063_MONTHS    0x09
#define PCF85063_YEARS     0x0A
#define PCF85063_SEC_ALARM 0x0B
#define PCF85063_MIN_ALARM 0x0C
#define PCF85063_HR_ALARM  0x0D
#define PCF85063_DAY_ALARM 0x0E
#define PCF85063_WD_ALARM  0x0F

bool RTCHAL::init(TwoWire &wire) {
#if !HAS_RTC
    return false;
#else
    _wire = &wire;
    _use_lgfx = false;
#if defined(RTC_I2C_ADDR)
    _addr = RTC_I2C_ADDR;
#else
    _addr = 0x51;
#endif
    return initDevice();
#endif
}

bool RTCHAL::initLgfx(uint8_t i2c_port, uint8_t addr) {
#if !HAS_RTC || !HAS_LGFX_I2C
    return false;
#else
    _use_lgfx = true;
    _lgfx_port = i2c_port;
    _addr = addr;
    _wire = nullptr;
    return initDevice();
#endif
}

bool RTCHAL::initDevice() {
    writeReg(PCF85063_CTRL1, 0x00);
    _initialized = true;
    return true;
}

RTCTime RTCHAL::getTime() {
    RTCTime t = {};
    if (!_initialized) return t;

    uint8_t buf[7];
    readRegs(PCF85063_SECONDS, buf, 7);
    t.second  = bcd2dec(buf[0] & 0x7F);
    t.minute  = bcd2dec(buf[1] & 0x7F);
    t.hour    = bcd2dec(buf[2] & 0x3F);
    t.day     = bcd2dec(buf[3] & 0x3F);
    t.weekday = buf[4] & 0x07;
    t.month   = bcd2dec(buf[5] & 0x1F);
    t.year    = 2000 + bcd2dec(buf[6]);
    return t;
}

bool RTCHAL::setTime(const RTCTime &t) {
    if (!_initialized) return false;
    writeReg(PCF85063_CTRL1, 0x20);
    writeReg(PCF85063_SECONDS,  dec2bcd(t.second));
    writeReg(PCF85063_MINUTES,  dec2bcd(t.minute));
    writeReg(PCF85063_HOURS,    dec2bcd(t.hour));
    writeReg(PCF85063_DAYS,     dec2bcd(t.day));
    writeReg(PCF85063_WEEKDAYS, t.weekday & 0x07);
    writeReg(PCF85063_MONTHS,   dec2bcd(t.month));
    writeReg(PCF85063_YEARS,    dec2bcd(t.year - 2000));
    writeReg(PCF85063_CTRL1, 0x00);
    return true;
}

bool RTCHAL::setAlarm(uint8_t hour, uint8_t minute) {
    if (!_initialized) return false;
    writeReg(PCF85063_SEC_ALARM, 0x80);
    writeReg(PCF85063_MIN_ALARM, dec2bcd(minute));
    writeReg(PCF85063_HR_ALARM,  dec2bcd(hour));
    writeReg(PCF85063_DAY_ALARM, 0x80);
    writeReg(PCF85063_WD_ALARM,  0x80);
    uint8_t ctrl2 = readReg(PCF85063_CTRL2);
    writeReg(PCF85063_CTRL2, ctrl2 | 0x80);
    return true;
}

bool RTCHAL::clearAlarm() {
    if (!_initialized) return false;
    uint8_t ctrl2 = readReg(PCF85063_CTRL2);
    writeReg(PCF85063_CTRL2, ctrl2 & ~0xC0);
    return true;
}

void RTCHAL::writeReg(uint8_t reg, uint8_t val) {
#if HAS_LGFX_I2C
    if (_use_lgfx) {
        uint8_t buf[2] = { reg, val };
        lgfx::i2c::transactionWrite(_lgfx_port, _addr, buf, 2, 400000);
    } else
#endif
    {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->write(val);
        _wire->endTransmission();
    }
}

uint8_t RTCHAL::readReg(uint8_t reg) {
    uint8_t val = 0;
#if HAS_LGFX_I2C
    if (_use_lgfx) {
        lgfx::i2c::transactionWriteRead(_lgfx_port, _addr,
            &reg, 1, &val, 1, 400000);
    } else
#endif
    {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission(false);
        _wire->requestFrom(_addr, (uint8_t)1);
        if (_wire->available()) val = _wire->read();
    }
    return val;
}

void RTCHAL::readRegs(uint8_t reg, uint8_t *buf, uint8_t len) {
#if HAS_LGFX_I2C
    if (_use_lgfx) {
        lgfx::i2c::transactionWriteRead(_lgfx_port, _addr,
            &reg, 1, buf, len, 400000);
    } else
#endif
    {
        _wire->beginTransmission(_addr);
        _wire->write(reg);
        _wire->endTransmission(false);
        _wire->requestFrom(_addr, len);
        for (uint8_t i = 0; i < len && _wire->available(); i++) {
            buf[i] = _wire->read();
        }
    }
}

#endif // SIMULATOR
