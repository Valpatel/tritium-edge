// BLE Device Type Classifier
//
// Classifies BLE devices by combining OUI manufacturer prefix,
// advertised device name patterns, and address type heuristics.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <cstdint>

namespace ble_classifier {

enum class DeviceType : uint8_t {
    UNKNOWN     = 0,
    PHONE       = 1,
    TABLET      = 2,
    LAPTOP      = 3,
    WATCH       = 4,
    TRACKER     = 5,   // AirTag, Tile, SmartTag
    HEADPHONES  = 6,
    SPEAKER     = 7,
    TV          = 8,
    IOT         = 9,   // Smart home devices
    GAME        = 10,  // Game controllers
    VEHICLE     = 11,
    MEDICAL     = 12,
    CAMERA      = 13,
    PRINTER     = 14,
    ROUTER      = 15,  // Network equipment
};

/// Classify a BLE device from its MAC address, name, and address type.
/// Returns the best-guess DeviceType.
DeviceType classify(const uint8_t mac[6], const char* name, uint8_t addr_type);

/// Get a short human-readable label for a DeviceType.
const char* type_name(DeviceType t);

/// Get a compact icon string for UI display (1-2 chars).
const char* type_icon(DeviceType t);

}  // namespace ble_classifier
