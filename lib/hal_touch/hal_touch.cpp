#include "hal_touch.h"

#ifdef SIMULATOR

// --- Simulator stubs (touch via SDL mouse) ---

bool TouchHAL::init() {
    _driver = FT3168; // pretend we have a touch controller
    return true;
}

bool TouchHAL::isTouched() {
    return false; // SDL mouse integration handled by LVGL indev
}

bool TouchHAL::read(uint16_t &x, uint16_t &y) {
    return false;
}

uint8_t TouchHAL::getPoints(TouchPoint *points, uint8_t maxPoints) {
    return 0;
}

#else // ESP32

#include <Arduino.h>
#include <Wire.h>

#if __has_include("hal_diag.h")
#include "hal_diag.h"
#define TOUCH_HAS_DIAG 1
#else
#define TOUCH_HAS_DIAG 0
#endif

bool TouchHAL::init(TwoWire &wire) {
    _wire = &wire;

#if defined(TOUCH_INT) && TOUCH_INT >= 0
    _int_pin = TOUCH_INT;
    ::pinMode(_int_pin, INPUT);
#endif

#if defined(BOARD_TOUCH_LCD_43C_BOX)
    _addr = 0x5D;
    _wire->beginTransmission(_addr);
    if (_wire->endTransmission() != 0) {
        _addr = 0x14;
        _wire->beginTransmission(_addr);
        if (_wire->endTransmission() != 0) return false;
    }
    _driver = GT911;
#if TOUCH_HAS_DIAG
    hal_diag::log(hal_diag::Severity::INFO, "touch", "GT911 at 0x%02X", _addr);
#endif
    return true;

#elif defined(BOARD_TOUCH_LCD_35BC) || defined(BOARD_TOUCH_LCD_349)
#if defined(TOUCH_I2C_ADDR)
    _addr = TOUCH_I2C_ADDR;
#else
    _addr = 0x3B;
#endif
    _wire->beginTransmission(_addr);
    if (_wire->endTransmission() != 0) return false;
    _driver = AXS15231B_TOUCH;
#if TOUCH_HAS_DIAG
    hal_diag::log(hal_diag::Severity::INFO, "touch", "AXS15231B at 0x%02X", _addr);
#endif
    return true;

#elif defined(BOARD_TOUCH_AMOLED_241B)
#if defined(TOUCH_I2C_ADDR)
    _addr = TOUCH_I2C_ADDR;
#else
    _addr = 0x38;
#endif
    _wire->beginTransmission(_addr);
    if (_wire->endTransmission() != 0) return false;
    _driver = FT6336;
#if TOUCH_HAS_DIAG
    hal_diag::log(hal_diag::Severity::INFO, "touch", "FT6336 at 0x%02X", _addr);
#endif
    return true;

#elif defined(BOARD_AMOLED_191M) || defined(BOARD_TOUCH_AMOLED_18)
#if defined(TOUCH_I2C_ADDR)
    _addr = TOUCH_I2C_ADDR;
#else
    _addr = 0x38;
#endif
    _wire->beginTransmission(_addr);
    if (_wire->endTransmission() != 0) return false;
    _driver = FT3168;
#if TOUCH_HAS_DIAG
    hal_diag::log(hal_diag::Severity::INFO, "touch", "FT3168 at 0x%02X", _addr);
#endif
    return true;

#else
#if TOUCH_HAS_DIAG
    hal_diag::log(hal_diag::Severity::WARN, "touch", "No touch driver for this board");
#endif
    return false;
#endif
}

bool TouchHAL::isTouched() {
    if (_driver == NONE) return false;

    if (_int_pin >= 0) {
        return ::digitalRead(_int_pin) == LOW;
    }

    if (_driver == GT911) {
        uint8_t status = gt911_readReg(0x814E);
        return (status & 0x80) && (status & 0x0F) > 0;
    }

    uint8_t numPoints = readReg8(0x02);
    return (numPoints & 0x0F) > 0;
}

bool TouchHAL::read(uint16_t &x, uint16_t &y) {
    switch (_driver) {
        case FT3168:
        case FT6336:    return ft_read(x, y);
        case GT911:     return gt911_read(x, y);
        case AXS15231B_TOUCH: return axs_read(x, y);
        default:        return false;
    }
}

uint8_t TouchHAL::getPoints(TouchPoint *points, uint8_t maxPoints) {
    if (_driver == NONE || maxPoints == 0) return 0;
    uint16_t x, y;
    if (read(x, y)) {
        points[0].x = x;
        points[0].y = y;
        return 1;
    }
    return 0;
}

bool TouchHAL::ft_read(uint16_t &x, uint16_t &y) {
    uint8_t numPoints = readReg8(0x02) & 0x0F;
    if (numPoints == 0) return false;
    uint8_t xH = readReg8(0x03) & 0x0F;
    uint8_t xL = readReg8(0x04);
    uint8_t yH = readReg8(0x05) & 0x0F;
    uint8_t yL = readReg8(0x06);
    x = (xH << 8) | xL;
    y = (yH << 8) | yL;
    return true;
}

bool TouchHAL::gt911_read(uint16_t &x, uint16_t &y) {
    uint8_t status = gt911_readReg(0x814E);
    if (!(status & 0x80)) return false;
    uint8_t numPoints = status & 0x0F;
    if (numPoints == 0) {
        gt911_writeReg(0x814E, 0);
        return false;
    }
    uint8_t buf[4];
    _wire->beginTransmission(_addr);
    _wire->write(0x81);
    _wire->write(0x50);
    _wire->endTransmission(false);
    _wire->requestFrom(_addr, (uint8_t)4);
    for (int i = 0; i < 4 && _wire->available(); i++) {
        buf[i] = _wire->read();
    }
    x = buf[0] | (buf[1] << 8);
    y = buf[2] | (buf[3] << 8);
    gt911_writeReg(0x814E, 0);
    return true;
}

bool TouchHAL::axs_read(uint16_t &x, uint16_t &y) {
    uint8_t buf[8];
    _wire->beginTransmission(_addr);
    _wire->write(0x00);
    _wire->endTransmission(false);
    _wire->requestFrom(_addr, (uint8_t)8);
    for (int i = 0; i < 8 && _wire->available(); i++) {
        buf[i] = _wire->read();
    }
    if (buf[0] == 0xFF || (buf[1] == 0xFF && buf[2] == 0xFF)) {
        return false;
    }
    x = ((buf[2] & 0x0F) << 8) | buf[3];
    y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

uint8_t TouchHAL::readReg8(uint8_t reg) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->endTransmission(false);
    _wire->requestFrom(_addr, (uint8_t)1);
    return _wire->available() ? _wire->read() : 0;
}

uint16_t TouchHAL::readReg16(uint8_t regH, uint8_t regL) {
    return ((uint16_t)readReg8(regH) << 8) | readReg8(regL);
}

void TouchHAL::writeReg8(uint8_t reg, uint8_t val) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write(val);
    _wire->endTransmission();
}

uint8_t TouchHAL::gt911_readReg(uint16_t reg) {
    _wire->beginTransmission(_addr);
    _wire->write((uint8_t)(reg >> 8));
    _wire->write((uint8_t)(reg & 0xFF));
    _wire->endTransmission(false);
    _wire->requestFrom(_addr, (uint8_t)1);
    return _wire->available() ? _wire->read() : 0;
}

void TouchHAL::gt911_writeReg(uint16_t reg, uint8_t val) {
    _wire->beginTransmission(_addr);
    _wire->write((uint8_t)(reg >> 8));
    _wire->write((uint8_t)(reg & 0xFF));
    _wire->write(val);
    _wire->endTransmission();
}

#endif // SIMULATOR
