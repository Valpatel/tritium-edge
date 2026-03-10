#pragma once
// Unified Touch HAL abstracting FT3168, FT6336, GT911, and AXS15231B touch
// Uses ESP-IDF I2C master API (no Arduino Wire dependency).
//
// Usage:
//   #include "hal_touch.h"
//   TouchHAL touch;
//   touch.init();

#include <cstdint>
#include <cstddef>

struct TouchPoint {
    uint16_t x;
    uint16_t y;
};

class TouchHAL {
public:
    enum TouchDriver { NONE, FT3168, FT6336, GT911, AXS15231B_TOUCH };

    bool init();
    bool isTouched();
    bool read(uint16_t &x, uint16_t &y);
    uint8_t getPoints(TouchPoint *points, uint8_t maxPoints);
    TouchDriver getDriver() const { return _driver; }
    bool available() const { return _driver != NONE; }

private:
    TouchDriver _driver = NONE;
    int8_t _int_pin = -1;

#ifndef SIMULATOR
    void *_i2c_dev = nullptr; // i2c_master_dev_handle_t
    uint8_t _addr = 0;

    bool i2c_read_reg(uint8_t reg, uint8_t *buf, size_t len);
    bool i2c_write_reg(uint8_t reg, uint8_t val);
    bool i2c_read_reg16(uint16_t reg, uint8_t *buf, size_t len);
    bool i2c_write_reg16(uint16_t reg, uint8_t val);
    bool ft_read(uint16_t &x, uint16_t &y);
    bool gt911_read(uint16_t &x, uint16_t &y);
    bool axs_read(uint16_t &x, uint16_t &y);
#endif
};
