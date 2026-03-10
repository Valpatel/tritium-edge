#pragma once
// Unified IO Expander HAL for CH422G and TCA9554PWR
//
// Usage:
//   #include "hal_io_expander.h"
//   IOExpander io;
//   io.init();  // simulator
//   io.init();  // ESP32 (uses global i2c0)

#include <cstdint>
#include <cstddef>

class IOExpander {
public:
    enum ExpanderType { NONE, CH422G, TCA9554 };

    static constexpr uint8_t PIN_INPUT  = 0;
    static constexpr uint8_t PIN_OUTPUT = 1;

#ifdef SIMULATOR
    bool init();
#else
    bool init();
#endif
    void setPinMode(uint8_t pin, uint8_t mode);
    void writePin(uint8_t pin, uint8_t value);
    uint8_t readPin(uint8_t pin);
    ExpanderType getType() const { return _type; }
    bool available() const { return _type != NONE; }

private:
    ExpanderType _type = NONE;
    uint8_t _output_state = 0;
    uint8_t _dir_state = 0xFF;

#ifndef SIMULATOR
    uint8_t _addr = 0;

    static constexpr uint8_t CH422G_WR_IO_ADDR = 0x38;
    static constexpr uint8_t CH422G_RD_IO_ADDR = 0x38;
    static constexpr uint8_t CH422G_SET_ADDR   = 0x24;

    void ch422g_write_output(uint8_t val);
    uint8_t ch422g_read_input();
    void ch422g_set_config(uint8_t cfg);
    void tca9554_write_reg(uint8_t reg, uint8_t val);
    uint8_t tca9554_read_reg(uint8_t reg);
#endif
};
