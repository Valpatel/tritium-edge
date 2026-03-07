#include "hal_imu.h"
#include <cmath>

#ifdef SIMULATOR

// --- Simulator stubs (returns fake gravity vector) ---

bool IMUHAL::init() {
    _initialized = true;
    return true;
}

uint8_t IMUHAL::whoAmI() { return 0x05; }

bool IMUHAL::readAccel(float &x, float &y, float &z) {
    if (!_initialized) return false;
    x = 0.0f; y = 0.0f; z = 1.0f; // simulate gravity
    return true;
}

bool IMUHAL::readGyro(float &x, float &y, float &z) {
    if (!_initialized) return false;
    x = 0.0f; y = 0.0f; z = 0.0f;
    return true;
}

bool IMUHAL::readAll(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
    if (!_initialized) return false;
    ax = 0.0f; ay = 0.0f; az = 1.0f;
    gx = 0.0f; gy = 0.0f; gz = 0.0f;
    return true;
}

bool IMUHAL::detectMotion(float threshold) {
    return false;
}

#else // ESP32

#include <Arduino.h>
#include <Wire.h>
#include <lgfx/v1/platforms/common.hpp>

#ifndef HAS_IMU
#define HAS_IMU 0
#endif

#define QMI8658_WHO_AM_I      0x00
#define QMI8658_CTRL1         0x02
#define QMI8658_CTRL2         0x03
#define QMI8658_CTRL3         0x04
#define QMI8658_CTRL5         0x06
#define QMI8658_CTRL7         0x08
#define QMI8658_AX_L          0x35
#define QMI8658_GX_L          0x3B
#define QMI8658_RESET         0x60

bool IMUHAL::init(TwoWire &wire) {
#if !HAS_IMU
    return false;
#else
    _wire = &wire;
    _use_lgfx = false;

#if defined(IMU_I2C_ADDR)
    _addr = IMU_I2C_ADDR;
#else
    _addr = 0x6B;
#endif

    return initDevice();
#endif
}

bool IMUHAL::initLgfx(uint8_t i2c_port, uint8_t addr) {
#if !HAS_IMU
    return false;
#else
    _use_lgfx = true;
    _lgfx_port = i2c_port;
    _addr = addr;
    _wire = nullptr;

    return initDevice();
#endif
}

bool IMUHAL::initDevice() {
    uint8_t id = readReg(QMI8658_WHO_AM_I);
    if (id != 0x05) return false;

    writeReg(QMI8658_RESET, 0xB0);
    delay(20);

    id = readReg(QMI8658_WHO_AM_I);
    if (id != 0x05) return false;

    writeReg(QMI8658_CTRL1, 0x40);
    writeReg(QMI8658_CTRL2, 0x35);
    _accel_scale = 8.0f / 32768.0f;
    writeReg(QMI8658_CTRL3, 0x55);
    _gyro_scale = 512.0f / 32768.0f;
    writeReg(QMI8658_CTRL5, 0x11);
    writeReg(QMI8658_CTRL7, 0x03);

    delay(10);
    _initialized = true;
    return true;
}

uint8_t IMUHAL::whoAmI() {
    return readReg(QMI8658_WHO_AM_I);
}

bool IMUHAL::readAccel(float &x, float &y, float &z) {
    if (!_initialized) return false;
    uint8_t buf[6];
    readRegs(QMI8658_AX_L, buf, 6);
    x = (int16_t)(buf[1] << 8 | buf[0]) * _accel_scale;
    y = (int16_t)(buf[3] << 8 | buf[2]) * _accel_scale;
    z = (int16_t)(buf[5] << 8 | buf[4]) * _accel_scale;
    return true;
}

bool IMUHAL::readGyro(float &x, float &y, float &z) {
    if (!_initialized) return false;
    uint8_t buf[6];
    readRegs(QMI8658_GX_L, buf, 6);
    x = (int16_t)(buf[1] << 8 | buf[0]) * _gyro_scale;
    y = (int16_t)(buf[3] << 8 | buf[2]) * _gyro_scale;
    z = (int16_t)(buf[5] << 8 | buf[4]) * _gyro_scale;
    return true;
}

bool IMUHAL::readAll(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
    if (!_initialized) return false;
    uint8_t buf[12];
    readRegs(QMI8658_AX_L, buf, 12);
    ax = (int16_t)(buf[1]  << 8 | buf[0])  * _accel_scale;
    ay = (int16_t)(buf[3]  << 8 | buf[2])  * _accel_scale;
    az = (int16_t)(buf[5]  << 8 | buf[4])  * _accel_scale;
    gx = (int16_t)(buf[7]  << 8 | buf[6])  * _gyro_scale;
    gy = (int16_t)(buf[9]  << 8 | buf[8])  * _gyro_scale;
    gz = (int16_t)(buf[11] << 8 | buf[10]) * _gyro_scale;
    return true;
}

bool IMUHAL::detectMotion(float threshold) {
    float ax, ay, az;
    if (!readAccel(ax, ay, az)) return false;
    float magnitude = sqrtf(ax * ax + ay * ay + az * az);
    return fabsf(magnitude - 1.0f) > (threshold - 1.0f);
}

void IMUHAL::writeReg(uint8_t reg, uint8_t val) {
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

uint8_t IMUHAL::readReg(uint8_t reg) {
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

void IMUHAL::readRegs(uint8_t reg, uint8_t *buf, uint8_t len) {
    if (_use_lgfx) {
        lgfx::i2c::transactionWriteRead(_lgfx_port, _addr,
            &reg, 1, buf, len, 400000);
    } else {
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
