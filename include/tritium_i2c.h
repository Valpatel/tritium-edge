/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * tritium_i2c.h — Native ESP-IDF I2C master, replacing Arduino Wire.
 *
 * Uses the new i2c_master driver (ESP-IDF 5.x). No conflicts with LovyanGFX
 * or other drivers since there's a single bus owner.
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_log.h"
#include <cstdint>
#include <cstring>

class TritiumI2C {
public:
    /**
     * Initialize the I2C master bus.
     * @param sda  GPIO number for SDA
     * @param scl  GPIO number for SCL
     * @param freq Clock frequency in Hz (default 400kHz)
     * @return true on success
     */
    bool begin(int sda, int scl, uint32_t freq = 400000) {
        if (_bus_handle) return true;  // Already initialized

        _sda = sda;
        _scl = scl;

        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.i2c_port = I2C_NUM_0;
        bus_cfg.sda_io_num = (gpio_num_t)sda;
        bus_cfg.scl_io_num = (gpio_num_t)scl;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&bus_cfg, &_bus_handle);
        if (err != ESP_OK) {
            ESP_LOGE("i2c", "Failed to create I2C bus: %s", esp_err_to_name(err));
            _bus_handle = nullptr;
            return false;
        }

        _freq = freq;
        return true;
    }

    void end() {
        // Remove all cached device handles
        for (int i = 0; i < _dev_count; i++) {
            if (_devs[i].handle) {
                i2c_master_bus_rm_device(_devs[i].handle);
            }
        }
        _dev_count = 0;

        if (_bus_handle) {
            i2c_del_master_bus(_bus_handle);
            _bus_handle = nullptr;
        }
    }

    bool isInitialized() const { return _bus_handle != nullptr; }

    /**
     * Write data to an I2C device.
     * @param addr  7-bit I2C address
     * @param data  Data buffer
     * @param len   Number of bytes
     * @param timeout_ms  Timeout in milliseconds
     * @return ESP_OK on success
     */
    esp_err_t write(uint8_t addr, const uint8_t* data, size_t len, int timeout_ms = 100) {
        i2c_master_dev_handle_t dev = getDevice(addr);
        if (!dev) return ESP_ERR_INVALID_STATE;
        return i2c_master_transmit(dev, data, len, timeout_ms);
    }

    /**
     * Read data from an I2C device.
     * @param addr  7-bit I2C address
     * @param data  Buffer to read into
     * @param len   Number of bytes to read
     * @param timeout_ms  Timeout in milliseconds
     * @return ESP_OK on success
     */
    esp_err_t read(uint8_t addr, uint8_t* data, size_t len, int timeout_ms = 100) {
        i2c_master_dev_handle_t dev = getDevice(addr);
        if (!dev) return ESP_ERR_INVALID_STATE;
        return i2c_master_receive(dev, data, len, timeout_ms);
    }

    /**
     * Write then read (combined transaction).
     */
    esp_err_t writeRead(uint8_t addr,
                        const uint8_t* write_data, size_t write_len,
                        uint8_t* read_data, size_t read_len,
                        int timeout_ms = 100) {
        i2c_master_dev_handle_t dev = getDevice(addr);
        if (!dev) return ESP_ERR_INVALID_STATE;
        return i2c_master_transmit_receive(dev, write_data, write_len,
                                           read_data, read_len, timeout_ms);
    }

    /**
     * Write a single register value.
     */
    esp_err_t writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
        uint8_t buf[2] = { reg, value };
        return write(addr, buf, 2);
    }

    /**
     * Read a single register.
     */
    esp_err_t readReg(uint8_t addr, uint8_t reg, uint8_t* value) {
        return writeRead(addr, &reg, 1, value, 1);
    }

    /**
     * Probe an address to check if a device is present.
     */
    bool probe(uint8_t addr) {
        if (!_bus_handle) return false;
        return i2c_master_probe(_bus_handle, addr, 50) == ESP_OK;
    }

    /**
     * Arduino Wire compatibility: beginTransmission / write / endTransmission.
     * For gradual migration. Prefer write() or writeReg() for new code.
     */
    void beginTransmission(uint8_t addr) {
        _tx_addr = addr;
        _tx_len = 0;
    }

    size_t wireWrite(uint8_t data) {
        if (_tx_len < sizeof(_tx_buf)) {
            _tx_buf[_tx_len++] = data;
            return 1;
        }
        return 0;
    }

    uint8_t endTransmission(bool sendStop = true) {
        (void)sendStop;
        esp_err_t err = write(_tx_addr, _tx_buf, _tx_len);
        _tx_len = 0;
        return (err == ESP_OK) ? 0 : 4;  // 0=success, 4=other error (Wire convention)
    }

    uint8_t requestFrom(uint8_t addr, uint8_t len) {
        _rx_len = 0;
        _rx_idx = 0;
        if (len > sizeof(_rx_buf)) len = sizeof(_rx_buf);
        esp_err_t err = read(addr, _rx_buf, len);
        if (err == ESP_OK) {
            _rx_len = len;
            return len;
        }
        return 0;
    }

    int wireAvailable() const { return _rx_len - _rx_idx; }

    int wireRead() {
        if (_rx_idx < _rx_len) return _rx_buf[_rx_idx++];
        return -1;
    }

    // Access the bus handle for advanced operations
    i2c_master_bus_handle_t getBusHandle() const { return _bus_handle; }

private:
    static constexpr int MAX_DEVICES = 16;

    struct DevEntry {
        uint8_t addr;
        i2c_master_dev_handle_t handle;
    };

    i2c_master_dev_handle_t getDevice(uint8_t addr) {
        if (!_bus_handle) return nullptr;

        // Check cache
        for (int i = 0; i < _dev_count; i++) {
            if (_devs[i].addr == addr) return _devs[i].handle;
        }

        // Create new device handle
        if (_dev_count >= MAX_DEVICES) {
            ESP_LOGE("i2c", "Too many I2C devices (max %d)", MAX_DEVICES);
            return nullptr;
        }

        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = addr;
        dev_cfg.scl_speed_hz = _freq;

        i2c_master_dev_handle_t handle = nullptr;
        esp_err_t err = i2c_master_bus_add_device(_bus_handle, &dev_cfg, &handle);
        if (err != ESP_OK) {
            ESP_LOGE("i2c", "Failed to add device 0x%02X: %s", addr, esp_err_to_name(err));
            return nullptr;
        }

        _devs[_dev_count++] = { addr, handle };
        return handle;
    }

    i2c_master_bus_handle_t _bus_handle = nullptr;
    int _sda = -1;
    int _scl = -1;
    uint32_t _freq = 400000;

    DevEntry _devs[MAX_DEVICES] = {};
    int _dev_count = 0;

    // Wire compat buffers
    uint8_t _tx_addr = 0;
    uint8_t _tx_buf[64] = {};
    size_t _tx_len = 0;
    uint8_t _rx_buf[64] = {};
    size_t _rx_len = 0;
    size_t _rx_idx = 0;
};

// Global I2C bus instance
extern TritiumI2C i2c0;
