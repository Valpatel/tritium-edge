#pragma once
// QMI8658 6-axis IMU HAL
//
// Usage:
//   #include "hal_imu.h"
//   IMUHAL imu;
//   imu.init(Wire);           // ESP32 via Arduino Wire
//   imu.initLgfx(0, 0x6B);   // ESP32 via lgfx::i2c (shared bus with LovyanGFX)
//   imu.init();               // simulator

#include <cstdint>
#include <cstddef>

#ifndef SIMULATOR
class TwoWire;
#endif

class IMUHAL {
public:
#ifdef SIMULATOR
    bool init();
#else
    bool init(TwoWire &wire);
    bool initLgfx(uint8_t i2c_port, uint8_t addr = 0x6B);
#endif
    bool readAccel(float &x, float &y, float &z);
    bool readGyro(float &x, float &y, float &z);
    bool readAll(float &ax, float &ay, float &az, float &gx, float &gy, float &gz);
    bool detectMotion(float threshold = 1.5f);
    bool available() const { return _initialized; }
    uint8_t whoAmI();

private:
    bool _initialized = false;
    float _accel_scale = 8.0f / 32768.0f;
    float _gyro_scale = 512.0f / 32768.0f;

#ifndef SIMULATOR
    // Wire-based I2C
    TwoWire *_wire = nullptr;
    // lgfx-based I2C
    bool _use_lgfx = false;
    uint8_t _lgfx_port = 0;

    uint8_t _addr = 0;

    void writeReg(uint8_t reg, uint8_t val);
    uint8_t readReg(uint8_t reg);
    void readRegs(uint8_t reg, uint8_t *buf, uint8_t len);

    bool initDevice();
#endif
};
