#pragma once
// PCF85063 Real-Time Clock HAL
//
// Usage:
//   #include "hal_rtc.h"
//   RTCHAL rtc;
//   rtc.init(Wire);           // ESP32 via Arduino Wire
//   rtc.initLgfx(0, 0x51);   // ESP32 via lgfx::i2c
//   rtc.init();               // simulator

#include <cstdint>
#include <cstddef>

#ifndef SIMULATOR
class TwoWire;
#endif

struct RTCTime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday; // 0=Sunday
};

class RTCHAL {
public:
#ifdef SIMULATOR
    bool init();
#else
    bool init(TwoWire &wire);
    bool initLgfx(uint8_t i2c_port, uint8_t addr = 0x51);
#endif
    RTCTime getTime();
    bool setTime(const RTCTime &t);
    bool setAlarm(uint8_t hour, uint8_t minute);
    bool clearAlarm();
    bool available() const { return _initialized; }

private:
    bool _initialized = false;
    static uint8_t bcd2dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
    static uint8_t dec2bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

#ifndef SIMULATOR
    TwoWire *_wire = nullptr;
    bool _use_lgfx = false;
    uint8_t _lgfx_port = 0;
    uint8_t _addr = 0;

    bool initDevice();
    void writeReg(uint8_t reg, uint8_t val);
    uint8_t readReg(uint8_t reg);
    void readRegs(uint8_t reg, uint8_t *buf, uint8_t len);
#endif
};
