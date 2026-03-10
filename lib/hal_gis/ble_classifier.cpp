// BLE Device Type Classifier — pattern-based classification.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ble_classifier.h"
#include <cstring>
#include <cctype>

namespace ble_classifier {

// OUI prefix to manufacturer mapping (first 3 bytes of MAC)
struct OuiHint {
    uint8_t prefix[3];
    const char* manufacturer;
    DeviceType default_type;  // Fallback if name doesn't refine it
};

// Common manufacturer OUI prefixes and their typical device types.
// These are the most frequently seen in BLE scans.
static const OuiHint s_oui_hints[] = {
    // Apple — most BLE devices are phones/watches/AirPods
    {{0x4C, 0x1D, 0xBE}, "Apple",   DeviceType::PHONE},
    {{0xDC, 0xCF, 0x96}, "Apple",   DeviceType::PHONE},
    {{0x3C, 0xE0, 0x72}, "Apple",   DeviceType::PHONE},
    {{0x28, 0x6C, 0x07}, "Apple",   DeviceType::PHONE},
    {{0xA4, 0xD1, 0x8C}, "Apple",   DeviceType::PHONE},
    {{0x68, 0xFE, 0xF7}, "Apple",   DeviceType::PHONE},
    {{0xF0, 0x18, 0x98}, "Apple",   DeviceType::PHONE},
    // Samsung
    {{0xEC, 0x1F, 0x72}, "Samsung", DeviceType::PHONE},
    {{0x84, 0x25, 0xDB}, "Samsung", DeviceType::PHONE},
    {{0x00, 0x26, 0x37}, "Samsung", DeviceType::PHONE},
    {{0xC0, 0xBD, 0xC8}, "Samsung", DeviceType::PHONE},
    // Google
    {{0xF4, 0xF5, 0xD8}, "Google",  DeviceType::PHONE},
    {{0xF4, 0xF5, 0xE8}, "Google",  DeviceType::PHONE},
    {{0x54, 0x60, 0x09}, "Google",  DeviceType::SPEAKER},
    // Amazon
    {{0x74, 0xC2, 0x46}, "Amazon",  DeviceType::SPEAKER},
    {{0xFC, 0x65, 0xDE}, "Amazon",  DeviceType::SPEAKER},
    {{0xA0, 0x02, 0xDC}, "Amazon",  DeviceType::SPEAKER},
    // Tile tracker
    {{0xF0, 0x13, 0xC3}, "Tile",    DeviceType::TRACKER},
    // Fitbit
    {{0xC8, 0xFF, 0x28}, "Fitbit",  DeviceType::WATCH},
    // Bose
    {{0x04, 0x52, 0xC7}, "Bose",    DeviceType::HEADPHONES},
    {{0x2C, 0x41, 0xA1}, "Bose",    DeviceType::HEADPHONES},
    // Sony
    {{0xAC, 0x9B, 0x0A}, "Sony",    DeviceType::HEADPHONES},
    {{0x04, 0x5D, 0x4B}, "Sony",    DeviceType::HEADPHONES},
    // JBL / Harman
    {{0x00, 0x1D, 0xDF}, "Harman",  DeviceType::SPEAKER},
    // Intel (laptops)
    {{0x3C, 0x58, 0xC2}, "Intel",   DeviceType::LAPTOP},
    {{0x34, 0x13, 0xE8}, "Intel",   DeviceType::LAPTOP},
    {{0x7C, 0xB2, 0x7D}, "Intel",   DeviceType::LAPTOP},
    // Microsoft
    {{0x28, 0x18, 0x78}, "Microsoft", DeviceType::LAPTOP},
    {{0x7C, 0x1E, 0x52}, "Microsoft", DeviceType::GAME},
    // Espressif (IoT)
    {{0x24, 0x0A, 0xC4}, "Espressif", DeviceType::IOT},
    {{0x24, 0x62, 0xAB}, "Espressif", DeviceType::IOT},
    {{0x30, 0xAE, 0xA4}, "Espressif", DeviceType::IOT},
    // Raspberry Pi
    {{0xB8, 0x27, 0xEB}, "RPi",     DeviceType::IOT},
    {{0xD8, 0x3A, 0xDD}, "RPi",     DeviceType::IOT},
    // Philips Hue
    {{0x00, 0x17, 0x88}, "Philips",  DeviceType::IOT},
    // Ring
    {{0x34, 0x3D, 0xC4}, "Ring",     DeviceType::IOT},
};

static constexpr int NUM_OUI_HINTS = sizeof(s_oui_hints) / sizeof(s_oui_hints[0]);

// Case-insensitive substring search
static bool contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// Classify by device name pattern (highest priority)
static DeviceType classify_by_name(const char* name) {
    if (!name || name[0] == '\0') return DeviceType::UNKNOWN;

    // Phones
    if (contains_ci(name, "iPhone") || contains_ci(name, "Pixel") ||
        contains_ci(name, "Galaxy S") || contains_ci(name, "Galaxy Z") ||
        contains_ci(name, "OnePlus") || contains_ci(name, "Xiaomi") ||
        contains_ci(name, "Huawei") || contains_ci(name, "Oppo") ||
        contains_ci(name, "Motorola"))
        return DeviceType::PHONE;

    // Tablets
    if (contains_ci(name, "iPad") || contains_ci(name, "Galaxy Tab") ||
        contains_ci(name, "Kindle") || contains_ci(name, "Fire HD"))
        return DeviceType::TABLET;

    // Laptops
    if (contains_ci(name, "MacBook") || contains_ci(name, "ThinkPad") ||
        contains_ci(name, "Surface") || contains_ci(name, "Dell") ||
        contains_ci(name, "Chromebook") || contains_ci(name, "HP "))
        return DeviceType::LAPTOP;

    // Watches
    if (contains_ci(name, "Watch") || contains_ci(name, "Fitbit") ||
        contains_ci(name, "Garmin") || contains_ci(name, "Mi Band") ||
        contains_ci(name, "Galaxy Fit") || contains_ci(name, "Amazfit"))
        return DeviceType::WATCH;

    // Trackers
    if (contains_ci(name, "AirTag") || contains_ci(name, "Tile") ||
        contains_ci(name, "SmartTag") || contains_ci(name, "Chipolo") ||
        contains_ci(name, "FindMy"))
        return DeviceType::TRACKER;

    // Headphones / Earbuds
    if (contains_ci(name, "AirPods") || contains_ci(name, "Buds") ||
        contains_ci(name, "WH-1000") || contains_ci(name, "WF-1000") ||
        contains_ci(name, "Beats") || contains_ci(name, "JBL") ||
        contains_ci(name, "Bose") || contains_ci(name, "Headphone") ||
        contains_ci(name, "Earbuds") || contains_ci(name, "QC35") ||
        contains_ci(name, "QC45") || contains_ci(name, "EarFun"))
        return DeviceType::HEADPHONES;

    // Speakers
    if (contains_ci(name, "Echo") || contains_ci(name, "HomePod") ||
        contains_ci(name, "Sonos") || contains_ci(name, "Google Home") ||
        contains_ci(name, "Nest") || contains_ci(name, "Speaker") ||
        contains_ci(name, "SoundLink") || contains_ci(name, "Charge ") ||
        contains_ci(name, "Flip ") || contains_ci(name, "UE "))
        return DeviceType::SPEAKER;

    // TVs
    if (contains_ci(name, "[TV]") || contains_ci(name, "Roku") ||
        contains_ci(name, "Chromecast") || contains_ci(name, "Fire TV") ||
        contains_ci(name, "Apple TV") || contains_ci(name, "Samsung TV") ||
        contains_ci(name, "LG TV"))
        return DeviceType::TV;

    // Game controllers
    if (contains_ci(name, "Xbox") || contains_ci(name, "DualSense") ||
        contains_ci(name, "DualShock") || contains_ci(name, "Joy-Con") ||
        contains_ci(name, "Pro Controller") || contains_ci(name, "Switch"))
        return DeviceType::GAME;

    // IoT
    if (contains_ci(name, "Hue") || contains_ci(name, "LIFX") ||
        contains_ci(name, "Ring") || contains_ci(name, "Wyze") ||
        contains_ci(name, "ESP32") || contains_ci(name, "Shelly") ||
        contains_ci(name, "Tuya") || contains_ci(name, "Tasmota"))
        return DeviceType::IOT;

    // Printers
    if (contains_ci(name, "Printer") || contains_ci(name, "ENVY") ||
        contains_ci(name, "OfficeJet") || contains_ci(name, "LaserJet"))
        return DeviceType::PRINTER;

    return DeviceType::UNKNOWN;
}

DeviceType classify(const uint8_t mac[6], const char* name, uint8_t addr_type) {
    // Name-based classification has highest priority
    DeviceType by_name = classify_by_name(name);
    if (by_name != DeviceType::UNKNOWN) return by_name;

    // OUI-based classification
    for (int i = 0; i < NUM_OUI_HINTS; i++) {
        if (mac[0] == s_oui_hints[i].prefix[0] &&
            mac[1] == s_oui_hints[i].prefix[1] &&
            mac[2] == s_oui_hints[i].prefix[2]) {
            return s_oui_hints[i].default_type;
        }
    }

    // Heuristic: random address (bit 1 of first byte set) with no name
    // is often a modern smartphone or Apple device
    if (addr_type == 1 || (mac[0] & 0x02)) {
        return DeviceType::PHONE;  // Best guess for random-address BLE
    }

    return DeviceType::UNKNOWN;
}

const char* type_name(DeviceType t) {
    switch (t) {
        case DeviceType::PHONE:      return "Phone";
        case DeviceType::TABLET:     return "Tablet";
        case DeviceType::LAPTOP:     return "Laptop";
        case DeviceType::WATCH:      return "Watch";
        case DeviceType::TRACKER:    return "Tracker";
        case DeviceType::HEADPHONES: return "Headphones";
        case DeviceType::SPEAKER:    return "Speaker";
        case DeviceType::TV:         return "TV";
        case DeviceType::IOT:        return "IoT";
        case DeviceType::GAME:       return "Game";
        case DeviceType::VEHICLE:    return "Vehicle";
        case DeviceType::MEDICAL:    return "Medical";
        case DeviceType::CAMERA:     return "Camera";
        case DeviceType::PRINTER:    return "Printer";
        case DeviceType::ROUTER:     return "Router";
        default:                     return "Unknown";
    }
}

const char* type_icon(DeviceType t) {
    // Short text icons (no LVGL dependency)
    switch (t) {
        case DeviceType::PHONE:      return "Ph";
        case DeviceType::TABLET:     return "Tb";
        case DeviceType::LAPTOP:     return "Lp";
        case DeviceType::WATCH:      return "Wt";
        case DeviceType::TRACKER:    return "Tr";
        case DeviceType::HEADPHONES: return "Hp";
        case DeviceType::SPEAKER:    return "Sp";
        case DeviceType::TV:         return "TV";
        case DeviceType::IOT:        return "Io";
        case DeviceType::GAME:       return "Gm";
        case DeviceType::VEHICLE:    return "Vh";
        case DeviceType::MEDICAL:    return "Md";
        case DeviceType::CAMERA:     return "Cm";
        case DeviceType::PRINTER:    return "Pr";
        case DeviceType::ROUTER:     return "Rt";
        default:                     return "??";
    }
}

}  // namespace ble_classifier
