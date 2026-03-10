#include "hal_io_expander.h"

#ifdef SIMULATOR

// --- Simulator stubs ---

bool IOExpander::init() {
    _type = TCA9554;
    _output_state = 0;
    _dir_state = 0xFF;
    return true;
}

void IOExpander::setPinMode(uint8_t pin, uint8_t mode) {
    if (mode == PIN_OUTPUT) _dir_state &= ~(1 << pin);
    else _dir_state |= (1 << pin);
}

void IOExpander::writePin(uint8_t pin, uint8_t value) {
    if (value) _output_state |= (1 << pin);
    else _output_state &= ~(1 << pin);
}

uint8_t IOExpander::readPin(uint8_t pin) {
    return (_output_state >> pin) & 0x01;
}

#else // ESP32

#include "tritium_compat.h"
#include "tritium_i2c.h"

#ifndef HAS_IO_EXPANDER
#define HAS_IO_EXPANDER 0
#endif

bool IOExpander::init() {
#if !HAS_IO_EXPANDER
    return false;
#else

#if defined(IO_EXP_I2C_ADDR)
    _addr = IO_EXP_I2C_ADDR;
    if (i2c0.probe(_addr)) {
        _type = TCA9554;
        _dir_state = 0xFF;
        _output_state = 0x00;
        tca9554_write_reg(0x03, _dir_state);
        tca9554_write_reg(0x01, _output_state);
        return true;
    }
#else
    if (i2c0.probe(CH422G_SET_ADDR)) {
        _type = CH422G;
        ch422g_set_config(0x01);
        _output_state = 0x00;
        ch422g_write_output(_output_state);
        return true;
    }
#endif

    _type = NONE;
    return false;
#endif
}

void IOExpander::setPinMode(uint8_t pin, uint8_t mode) {
    if (_type == TCA9554) {
        if (mode == PIN_OUTPUT) _dir_state &= ~(1 << pin);
        else _dir_state |= (1 << pin);
        tca9554_write_reg(0x03, _dir_state);
    }
}

void IOExpander::writePin(uint8_t pin, uint8_t value) {
    if (value) _output_state |= (1 << pin);
    else _output_state &= ~(1 << pin);

    if (_type == TCA9554) tca9554_write_reg(0x01, _output_state);
    else if (_type == CH422G) ch422g_write_output(_output_state);
}

uint8_t IOExpander::readPin(uint8_t pin) {
    uint8_t val = 0;
    if (_type == TCA9554) val = tca9554_read_reg(0x00);
    else if (_type == CH422G) val = ch422g_read_input();
    return (val >> pin) & 0x01;
}

void IOExpander::ch422g_write_output(uint8_t val) {
    i2c0.write(CH422G_WR_IO_ADDR, &val, 1);
}

uint8_t IOExpander::ch422g_read_input() {
    uint8_t val = 0;
    i2c0.read(CH422G_RD_IO_ADDR, &val, 1);
    return val;
}

void IOExpander::ch422g_set_config(uint8_t cfg) {
    i2c0.write(CH422G_SET_ADDR, &cfg, 1);
}

void IOExpander::tca9554_write_reg(uint8_t reg, uint8_t val) {
    i2c0.writeReg(_addr, reg, val);
}

uint8_t IOExpander::tca9554_read_reg(uint8_t reg) {
    uint8_t val = 0;
    i2c0.readReg(_addr, reg, &val);
    return val;
}

#endif // SIMULATOR
