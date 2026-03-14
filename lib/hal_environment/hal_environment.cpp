// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

// Environment Sensor HAL — stub implementation
//
// Currently no Waveshare ESP32-S3 boards have onboard environment sensors.
// This stub returns invalid readings and logs a message on init.
// When a BME280/BMP280/SHT31 is connected via I2C, this file will be
// replaced with real driver code.

#include "hal_environment.h"
#include <Arduino.h>
#include <cmath>

static const char* TAG = "hal_env";

static hal_environment::EnvironmentConfig _config;
static hal_environment::EnvironmentReading _latest;
static uint32_t _last_read_ms = 0;
static bool _initialized = false;
static hal_environment::SensorChip _detected = hal_environment::SensorChip::NONE;

// I2C addresses to scan for known sensors
// BME280/BMP280: 0x76 or 0x77
// SHT31: 0x44 or 0x45
// BH1750: 0x23 or 0x5C
// BME680: 0x76 or 0x77 (same as BME280, differentiated by chip ID)
static bool _scan_i2c() {
    // TODO: Implement I2C scan when hardware is available
    // Wire.beginTransmission(addr); if (Wire.endTransmission() == 0) found
    return false;
}

static void _invalidate_reading() {
    _latest.temperature_c = NAN;
    _latest.humidity_pct = NAN;
    _latest.pressure_hpa = NAN;
    _latest.gas_resistance_ohm = NAN;
    _latest.light_lux = NAN;
    _latest.timestamp = 0;
    _latest.source = hal_environment::SensorChip::NONE;
    _latest.valid = false;
}

static void _publish_mqtt() {
    if (!_config.publish_mqtt || !_latest.valid) return;

    // Build JSON payload matching tritium_lib EnvironmentReading model
    // Published on tritium/{device_id}/environment by hal_mqtt
    // TODO: Wire to hal_mqtt when sensor hardware is available
    //
    // JSON format:
    // {
    //   "temperature_c": 22.5,
    //   "humidity_pct": 45.0,
    //   "pressure_hpa": 1013.25,
    //   "gas_resistance_ohm": null,
    //   "light_lux": null,
    //   "sensor": "BME280",
    //   "timestamp": 1234567890
    // }
}

namespace hal_environment {

bool init(const EnvironmentConfig& config) {
    _config = config;
    _invalidate_reading();

    if (_config.auto_detect) {
        if (_scan_i2c()) {
            Serial.printf("[%s] Environment sensor detected: chip=%d\n",
                          TAG, static_cast<int>(_detected));
            _initialized = true;
            return true;
        }
    }

    Serial.printf("[%s] No environment sensors detected on I2C bus. "
                  "Running in stub mode.\n", TAG);
    _initialized = true;
    return false;
}

bool tick() {
    if (!_initialized) return false;
    if (_detected == SensorChip::NONE) return false;

    uint32_t now = millis();
    if (now - _last_read_ms < _config.read_interval_ms) return false;

    return read_now();
}

bool read_now() {
    if (!_initialized || _detected == SensorChip::NONE) {
        _invalidate_reading();
        return false;
    }

    // TODO: Read actual sensor when hardware is available
    // For now, return invalid
    _invalidate_reading();
    _last_read_ms = millis();
    return false;
}

const EnvironmentReading& get_latest() {
    return _latest;
}

float get_temperature_c() { return _latest.temperature_c; }
float get_humidity_pct() { return _latest.humidity_pct; }
float get_pressure_hpa() { return _latest.pressure_hpa; }
float get_light_lux() { return _latest.light_lux; }

bool is_available() {
    return _detected != SensorChip::NONE;
}

SensorChip get_sensor_chip() {
    return _detected;
}

}  // namespace hal_environment
