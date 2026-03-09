#pragma once
// Unified Touch HAL abstracting FT3168, FT6336, GT911, and AXS15231B touch
//
// Usage:
//   #include "hal_touch.h"
//   TouchHAL touch;
//   touch.init(Wire);  // ESP32
//   touch.init();      // simulator

#include <cstdint>
#include <cstddef>

#ifndef SIMULATOR
class TwoWire;
#endif

struct TouchPoint {
    uint16_t x;
    uint16_t y;
};

class TouchHAL {
public:
    enum TouchDriver { NONE, FT3168, FT6336, GT911, AXS15231B_TOUCH };

#ifdef SIMULATOR
    bool init();
#else
    bool init(TwoWire &wire);
#endif
    bool isTouched();
    bool read(uint16_t &x, uint16_t &y);
    uint8_t getPoints(TouchPoint *points, uint8_t maxPoints);
    TouchDriver getDriver() const { return _driver; }
    bool available() const { return _driver != NONE; }
    uint8_t getAddr() const { return _addr; }

    /// Dump GT911 diagnostic registers into a JSON buffer. Returns bytes written.
    int dumpDiag(char* buf, size_t size);

private:
    TouchDriver _driver = NONE;
    int8_t _int_pin = -1;

#ifndef SIMULATOR
    TwoWire *_wire = nullptr;
    uint8_t _addr = 0;

    uint8_t readReg8(uint8_t reg);
    uint16_t readReg16(uint8_t regH, uint8_t regL);
    void writeReg8(uint8_t reg, uint8_t val);
    uint8_t gt911_readReg(uint16_t reg);
    void gt911_writeReg(uint16_t reg, uint8_t val);
    bool ft_read(uint16_t &x, uint16_t &y);
    bool gt911_read(uint16_t &x, uint16_t &y);
    bool axs_read(uint16_t &x, uint16_t &y);
#endif
};
