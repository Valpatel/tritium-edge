// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Environment Sensor HAL
//
// Abstraction layer for environmental sensors (BME280, BMP280, SHT31,
// BME680, etc.). Publishes readings to MQTT as JSON on topic:
//   tritium/{device_id}/environment
//
// Currently stub-only since Waveshare ESP32-S3 boards do not include
// onboard environment sensors. The data model is ready for external
// I2C sensors connected to the expansion header.
//
// Supported sensors (when connected):
//   BME280  — temperature, humidity, pressure
//   BMP280  — temperature, pressure
//   SHT31   — temperature, humidity
//   BME680  — temperature, humidity, pressure, gas resistance
//   BH1750  — ambient light (lux)
//   VEML7700 — ambient light (lux)
//
// Usage:
//   #include "hal_environment.h"
//   hal_environment::init();
//   // In loop:
//   hal_environment::tick();   // reads sensors, publishes if interval elapsed
//   float t = hal_environment::get_temperature_c();

#include <cstdint>

namespace hal_environment {

// Sensor types that can be auto-detected on I2C
enum class SensorChip : uint8_t {
    NONE = 0,
    BME280,
    BMP280,
    SHT31,
    BME680,
    BH1750,
    VEML7700,
};

struct EnvironmentConfig {
    uint32_t read_interval_ms = 30000;   // Default 30s between reads
    bool publish_mqtt = true;            // Publish to MQTT on each read
    bool auto_detect = true;             // Auto-detect sensors on I2C bus
    uint8_t i2c_sda = 0;                // SDA pin (0 = use board default)
    uint8_t i2c_scl = 0;                // SCL pin (0 = use board default)
};

struct EnvironmentReading {
    float temperature_c;      // Celsius (NaN if unavailable)
    float humidity_pct;       // 0-100% (NaN if unavailable)
    float pressure_hpa;       // hPa (NaN if unavailable)
    float gas_resistance_ohm; // Ohms, BME680 only (NaN if unavailable)
    float light_lux;          // Lux (NaN if unavailable)
    uint32_t timestamp;       // millis() when read
    SensorChip source;        // Which sensor provided the reading
    bool valid;               // Whether any sensor was read
};

// Initialize the environment sensor subsystem.
// Scans I2C for known sensors and configures them.
// Returns true if at least one sensor was found.
bool init(const EnvironmentConfig& config = {});

// Call from loop(). Reads sensors and publishes to MQTT if interval elapsed.
// Non-blocking — returns immediately if not time yet.
// Returns true if a reading was taken.
bool tick();

// Force an immediate reading (ignores interval timer).
bool read_now();

// Get the latest reading.
const EnvironmentReading& get_latest();

// Individual getters (return NaN if unavailable).
float get_temperature_c();
float get_humidity_pct();
float get_pressure_hpa();
float get_light_lux();

// Check if any environment sensor is available.
bool is_available();

// Get detected sensor chip type.
SensorChip get_sensor_chip();

}  // namespace hal_environment
