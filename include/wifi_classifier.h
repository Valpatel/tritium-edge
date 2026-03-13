// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
#pragma once

// WiFi SSID Pattern Classifier — identifies network types from SSID patterns,
// auth mode, and signal characteristics. Provides situational awareness for
// Tritium edge devices by categorizing nearby WiFi networks.

#include <cstdint>
#include <cstring>

namespace wifi_classifier {

enum class NetworkType : uint8_t {
    UNKNOWN        = 0,
    CORPORATE      = 1,   // Enterprise networks (eduroam, CORP-*, etc.)
    HOME_ROUTER    = 2,   // Consumer routers (NETGEAR, TP-Link, ASUS, etc.)
    MOBILE_HOTSPOT = 3,   // Phone hotspots (iPhone, AndroidAP, Galaxy, etc.)
    IOT_DEVICE     = 4,   // IoT setup networks (Ring, Nest, SmartLife, etc.)
    GUEST          = 5,   // Guest networks (*-Guest, *_guest, *visitor*)
    PUBLIC_OPEN    = 6,   // Open public WiFi (Starbucks, airport, hotel, etc.)
    MESH_EXTENDER  = 7,   // Mesh/repeater (EXT, RPT, _Mesh, etc.)
    PRINTER        = 8,   // Printer setup (DIRECT-, HP-*, EPSON*, etc.)
    VEHICLE        = 9,   // Car WiFi (myChevrolet, FordPass, Tesla, etc.)
    HIDDEN         = 10,  // Hidden SSID (empty name)
    SURVEILLANCE   = 11,  // Security cameras (Hikvision, Dahua, Reolink, etc.)
};

struct Classification {
    NetworkType type;
    uint8_t     confidence;   // 0-100
    const char* type_name;    // Human-readable label
};

// Classify a network by SSID, auth mode (0=OPEN,3=WPA2,5=WPA3), and RSSI
inline Classification classify(const char* ssid, uint8_t auth_type, int8_t rssi) {
    if (!ssid || ssid[0] == '\0') {
        return {NetworkType::HIDDEN, 95, "Hidden"};
    }

    // Work with lowercase copy for pattern matching
    char lower[33];
    int len = 0;
    for (int i = 0; ssid[i] && i < 32; i++) {
        lower[i] = (ssid[i] >= 'A' && ssid[i] <= 'Z') ? ssid[i] + 32 : ssid[i];
        len++;
    }
    lower[len] = '\0';

    // --- Printer (high confidence — very specific patterns) ---
    if (strncmp(ssid, "DIRECT-", 7) == 0 ||
        strncmp(lower, "hp-", 3) == 0 ||
        strncmp(lower, "epson", 5) == 0 ||
        strstr(lower, "canon_") ||
        strstr(lower, "brother_"))
        return {NetworkType::PRINTER, 90, "Printer"};

    // --- IoT device setup networks ---
    if (strstr(lower, "ring-") ||
        strstr(lower, "nest-") ||
        strstr(lower, "smartlife") ||
        strstr(lower, "tuya_") ||
        strstr(lower, "esp_") ||
        strstr(lower, "esp32") ||
        strstr(lower, "tasmota") ||
        strstr(lower, "sonoff") ||
        strstr(lower, "wled") ||
        strstr(lower, "shelly") ||
        strncmp(lower, "iot-", 4) == 0 ||
        strstr(lower, "smartthings") ||
        strstr(lower, "ecobee") ||
        strstr(lower, "wyze_") ||
        strstr(lower, "gosund"))
        return {NetworkType::IOT_DEVICE, 85, "IoT Device"};

    // --- Surveillance cameras ---
    if (strstr(lower, "hikvision") ||
        strstr(lower, "dahua") ||
        strstr(lower, "reolink") ||
        strncmp(lower, "ipc_", 4) == 0 ||
        strstr(lower, "amcrest") ||
        strstr(lower, "cam_setup"))
        return {NetworkType::SURVEILLANCE, 85, "Camera"};

    // --- Vehicle WiFi ---
    if (strstr(lower, "mychevrolet") ||
        strstr(lower, "fordpass") ||
        strstr(lower, "tesla") ||
        strstr(lower, "myford") ||
        strstr(lower, "onstar") ||
        strstr(lower, "subaru_") ||
        strncmp(lower, "honda_", 6) == 0 ||
        strstr(lower, "toyota_"))
        return {NetworkType::VEHICLE, 80, "Vehicle"};

    // --- Mobile hotspot ---
    if (strstr(lower, "iphone") ||
        strstr(lower, "ipad") ||
        strstr(lower, "androidap") ||
        strstr(lower, "galaxy") ||
        strstr(lower, "pixel") ||
        strstr(lower, "oneplus") ||
        strstr(lower, "hotspot") ||
        strstr(lower, "mi phone") ||
        strstr(lower, "redmi") ||
        strncmp(lower, "lg_", 3) == 0)
        return {NetworkType::MOBILE_HOTSPOT, 80, "Hotspot"};

    // --- Guest network ---
    if (strstr(lower, "guest") ||
        strstr(lower, "visitor") ||
        strstr(lower, "_gst") ||
        strstr(lower, "-gst"))
        return {NetworkType::GUEST, 85, "Guest"};

    // --- Mesh / extender ---
    if (strstr(lower, "_ext") ||
        strstr(lower, "-ext") ||
        strstr(lower, "_rpt") ||
        strstr(lower, "-rpt") ||
        strstr(lower, "_mesh") ||
        strstr(lower, "eero") ||
        strstr(lower, "velop") ||
        strstr(lower, "orbi") ||
        strstr(lower, "deco_"))
        return {NetworkType::MESH_EXTENDER, 75, "Mesh/Extender"};

    // --- Corporate / enterprise ---
    if (strstr(lower, "eduroam") ||
        strstr(lower, "corp") ||
        strstr(lower, "enterprise") ||
        strstr(lower, "-wpa2") ||
        strstr(lower, "secure") ||
        auth_type >= 5)  // WPA3 is typically enterprise
        return {NetworkType::CORPORATE, 70, "Corporate"};

    // --- Home router (ISP/manufacturer default SSIDs) ---
    if (strncmp(lower, "netgear", 7) == 0 ||
        strncmp(lower, "linksys", 7) == 0 ||
        strncmp(lower, "tp-link", 7) == 0 ||
        strncmp(lower, "asus", 4) == 0 ||
        strncmp(lower, "arris", 5) == 0 ||
        strncmp(lower, "att", 3) == 0 ||
        strstr(lower, "spectrum") ||
        strstr(lower, "xfinity") ||
        strstr(lower, "comcast") ||
        strncmp(lower, "sky", 3) == 0 ||
        strncmp(lower, "bt-", 3) == 0 ||
        strstr(lower, "verizon") ||
        strstr(lower, "optimum") ||
        strstr(lower, "cox") ||
        strstr(lower, "frontier") ||
        strncmp(lower, "dlink", 5) == 0 ||
        strncmp(lower, "ubnt", 4) == 0)
        return {NetworkType::HOME_ROUTER, 75, "Home Router"};

    // --- Public open networks ---
    if (auth_type == 0) {  // OPEN auth
        if (strstr(lower, "starbucks") ||
            strstr(lower, "mcdonalds") ||
            strstr(lower, "airport") ||
            strstr(lower, "hotel") ||
            strstr(lower, "lobby") ||
            strstr(lower, "library") ||
            strstr(lower, "cafe") ||
            strstr(lower, "free") ||
            strstr(lower, "public"))
            return {NetworkType::PUBLIC_OPEN, 80, "Public"};

        // Generic open network
        return {NetworkType::PUBLIC_OPEN, 50, "Public"};
    }

    // --- Fallback: try to guess from RSSI and auth ---
    // Strong signal + WPA2 is probably home router
    if (rssi > -50 && (auth_type == 3 || auth_type == 4)) {
        return {NetworkType::HOME_ROUTER, 40, "Home Router"};
    }

    return {NetworkType::UNKNOWN, 0, "Unknown"};
}

// Get human-readable name for a network type
inline const char* typeName(NetworkType t) {
    switch (t) {
        case NetworkType::CORPORATE:      return "Corporate";
        case NetworkType::HOME_ROUTER:    return "Home Router";
        case NetworkType::MOBILE_HOTSPOT: return "Hotspot";
        case NetworkType::IOT_DEVICE:     return "IoT Device";
        case NetworkType::GUEST:          return "Guest";
        case NetworkType::PUBLIC_OPEN:    return "Public";
        case NetworkType::MESH_EXTENDER:  return "Mesh/Extender";
        case NetworkType::PRINTER:        return "Printer";
        case NetworkType::VEHICLE:        return "Vehicle";
        case NetworkType::HIDDEN:         return "Hidden";
        case NetworkType::SURVEILLANCE:   return "Camera";
        default:                          return "Unknown";
    }
}

}  // namespace wifi_classifier
