# Device Fingerprinting — Comprehensive Identification Guide

Tritium identifies every detected wireless device using layered fingerprinting from publicly available databases. This document covers all identification methods, data sources, and classification logic.

## Overview

Every BLE and WiFi device emits identifying information. Tritium fuses multiple signal sources to classify each target with a device type, manufacturer, model hint, and confidence score. The classifier lives in `tritium-lib/src/tritium_lib/classifier/` and loads JSON lookup databases from `tritium-lib/src/tritium_lib/data/`.

## BLE Identification Layers

### 1. GAP Appearance Value (Highest Priority — 0.95 confidence)

The BLE Generic Access Profile (GAP) includes an Appearance characteristic (UUID 0x2A01) that explicitly declares the device type. This is the most reliable signal because the device manufacturer chose it intentionally.

**Data file:** `ble_appearance_values.json`
**Source:** Bluetooth SIG Assigned Numbers (bluetooth.com/specifications/assigned-numbers/)

| Appearance | Category | Device Type |
|-----------|----------|-------------|
| 64 | Phone | phone |
| 128 | Computer | computer |
| 131 | Computer | laptop |
| 192 | Watch | watch |
| 961 | HID | keyboard |
| 962 | HID | mouse |
| 1344 | Sensor | sensor |
| 1408 | Light Fixture | smart_light |
| 1537 | HVAC | thermostat |
| 1800 | Access Control | smart_lock |
| 2113 | Audio Sink | speaker |
| 5184 | Outdoor Sports | fitness_tracker |

Full table contains 150+ entries covering phones, computers, watches, sensors, lights, locks, audio devices, health monitors, and more.

### 2. Apple Continuity Protocol (0.85-0.95 confidence)

Apple devices broadcast Continuity messages in BLE advertising data under company ID 0x004C. These messages reveal device type and activity state.

**Data file:** `apple_continuity_types.json`
**Sources:** furiousMAC/continuity (GitHub), Celosia & Cunche 2019, Teplov 2020

**Message Types:**
- **0x10 Nearby Info** — Contains device type byte and activity state
- **0x0F Nearby Action** — Setup/pairing actions (Watch Setup, Speaker Setup, WiFi Password, etc.)
- **0x05 Proximity Pairing** — AirPods/Beats model identification
- **0x12 Find My** — AirTag and Find My network beacons
- **0x0C Handoff** — Cross-device continuity

**Device Type Codes (from Nearby Info):**
| Code | Device |
|------|--------|
| 0x01 | iPhone |
| 0x02 | iPad |
| 0x03 | MacBook |
| 0x04 | Mac Desktop |
| 0x05 | Apple Watch |
| 0x06 | AirPods |
| 0x07 | Apple TV |
| 0x08 | HomePod |
| 0x09 | AirTag |

**Proximity Pairing Model IDs:**
| Model ID | Device |
|----------|--------|
| 0x0220 | AirPods (1st gen) |
| 0x0F20 | AirPods 2 |
| 0x1320 | AirPods 3 |
| 0x0E20 | AirPods Pro |
| 0x1420 | AirPods Pro 2 |
| 0x0A20 | AirPods Max |

### 3. Device Name Pattern Matching (0.70-0.95 confidence)

BLE devices advertise a human-readable name. Tritium matches these against 150+ regex patterns covering major manufacturers.

**Data file:** `ble_name_patterns.json`

**Examples:**
- `^iPhone` → phone, Apple (0.95)
- `^Galaxy Watch` → watch, Samsung (0.95)
- `^WH-1000XM` → headphones, Sony (0.95)
- `^Fitbit Charge` → fitness_tracker, Fitbit (0.95)
- `^Meshtastic` → mesh_radio, Meshtastic (0.95)
- `^ESP32` → iot, Espressif (0.90)
- `^DJI ` → drone, DJI (0.90)

### 4. Google Fast Pair Model IDs (0.90 confidence)

Android devices with Fast Pair support advertise a 24-bit model ID under service UUID 0xFE2C. Tritium maps these to specific device names.

**Data file:** `apple_continuity_types.json` (google_fast_pair_models section)
**Source:** DiamondRoPlayz/FastPair-Models (GitHub), community reverse engineering

**Examples:**
| Model ID | Device |
|----------|--------|
| 0x060000 | Google Pixel Buds |
| 0xF00000 | Bose QC 35 II |
| 0x01EEB4 | Sony WH-1000XM4 |
| 0x0577B1 | Galaxy S23 Ultra |

### 5. Service UUIDs (0.80 confidence)

BLE devices advertise service UUIDs that indicate their capabilities. 16-bit UUIDs are assigned by the Bluetooth SIG.

**Data file:** `ble_service_uuids.json`
**Source:** Bluetooth SIG Assigned Numbers

**Key UUIDs:**
| UUID | Service | Device Hint |
|------|---------|-------------|
| 0x180D | Heart Rate | fitness_tracker |
| 0x1812 | Human Interface Device | hid |
| 0x1826 | Fitness Machine | fitness_equipment |
| 0x180F | Battery Service | (any) |
| 0x1821 | Indoor Positioning | beacon |
| 0xFEAA | Eddystone | beacon |
| 0xFE2C | Google Fast Pair | (any) |

### 6. Company Identifier (0.70 confidence)

BLE advertising data includes a 16-bit Bluetooth SIG company identifier in the manufacturer-specific data section.

**Data file:** `ble_company_ids.json`
**Source:** Bluetooth SIG Assigned Numbers, NordicSemiconductor/bluetooth-numbers-database

**Top Companies:**
| ID | Company | Likely Devices |
|----|---------|---------------|
| 76 | Apple | phone, tablet, laptop, watch |
| 117 | Samsung | phone, tablet, watch, tv |
| 224 | Google | phone, smart_speaker |
| 157 | Bose | headphones, speaker |
| 135 | Garmin | watch, gps |
| 346 | Fitbit | fitness_tracker |

### 7. OUI Lookup (0.55 confidence — lowest priority)

The first 3 octets of a MAC address identify the manufacturer (OUI — Organizationally Unique Identifier).

**Data file:** `oui_device_types.json`
**Source:** IEEE MA-L registry (standards-oui.ieee.org)

**Important:** Modern phones and tablets randomize their MAC addresses, making OUI lookup unreliable. The classifier detects randomized MACs (locally-administered bit set) and skips OUI lookup for those devices.

## WiFi Identification Layers

### 8. SSID Pattern Matching (0.70-0.95 confidence)

WiFi networks broadcast SSIDs that often reveal the device or network type.

**Data file:** `wifi_ssid_patterns.json`

**Categories:**
- **Hotspots:** "iPhone", "AndroidAP", "Galaxy" → phone
- **IoT Setup:** "Ring-", "Nest-", "SmartThings-" → smart_home
- **Printers:** "HP-Print-", "EPSON" → printer
- **Mesh Routers:** "eero", "Orbi", "Velop" → router
- **Cameras:** "GoPro-", "DJI-" → camera/drone
- **Corporate:** "CORP-", "eduroam" → enterprise network

### 9. DHCP Fingerprinting (0.75-0.90 confidence)

DHCP Option 60 (Vendor Class Identifier) and Option 12 (Hostname) reveal device OS and type.

**Data file:** `wifi_vendor_fingerprints.json`
**Source:** RFC 2132, Fingerbank community data

**Vendor Class Examples:**
- `android-dhcp` → phone, Android
- `MSFT 5.0` → Windows
- `Cisco Systems` → router
- `AmazonFireTV` → streaming_device

**Hostname Examples:**
- `iPhone-*` → phone, iOS
- `DESKTOP-*` → desktop, Windows
- `raspberrypi` → single_board_computer, Linux

### 10. mDNS/Bonjour Service Discovery (0.70 confidence)

Devices on the local network advertise services via mDNS that reveal their capabilities and manufacturer.

**Data file:** `wifi_vendor_fingerprints.json` (mdns_service_types section)
**Source:** IANA service registry, jonathanmumm.com mDNS Bible

**Key Services:**
| Service Type | Identifies |
|-------------|------------|
| `_airplay._tcp` | Apple streaming device |
| `_googlecast._tcp` | Chromecast/Google device |
| `_hap._tcp` | HomeKit accessory |
| `_spotify-connect._tcp` | Spotify-capable speaker |
| `_ipp._tcp` | Network printer |
| `_ssh._tcp` | Computer with SSH |
| `_nvstream._tcp` | NVIDIA Shield |

## MAC Address Randomization

Modern devices (iOS 14+, Android 10+) randomize their WiFi and BLE MAC addresses for privacy. Tritium detects this by checking the locally-administered bit:

- **Bit 1 of first octet set** → locally administered (randomized)
- Second hex character is 2, 3, 6, 7, A, B, E, or F → randomized

When a MAC is randomized, the classifier skips OUI lookup and relies on other signals (name, appearance, services, advertising data).

## Beacon Detection

### iBeacon (Apple)
- Company ID: 0x004C
- Advertising indicator: 0x0215
- Fields: UUID (16 bytes), Major (2 bytes), Minor (2 bytes), TX Power (1 byte)

### Eddystone (Google)
- Service UUID: 0xFEAA
- Frame types: UID (0x00), URL (0x10), TLM (0x20), EID (0x30)

### AltBeacon
- Manufacturer ID: 0xBEAC
- Fields: Beacon ID (20 bytes), Reference RSSI, MFG Reserved

## Classification Priority

The classifier evaluates sources in this order, with higher-priority sources overriding lower ones:

1. **BLE Appearance** (0.95) — explicit device declaration
2. **Apple Continuity** (0.85-0.95) — protocol-level device type
3. **Device Name Pattern** (0.70-0.95) — name regex matching
4. **Google Fast Pair** (0.90) — model ID database
5. **Service UUIDs** (0.80) — capability-based classification
6. **Company ID** (0.70) — manufacturer identification
7. **OUI Lookup** (0.55) — MAC prefix manufacturer
8. **SSID Pattern** (0.70-0.95) — WiFi SSID matching
9. **DHCP Vendor Class** (0.80) — DHCP option 60
10. **DHCP Hostname** (0.75) — hostname pattern
11. **mDNS Services** (0.70) — service discovery

## Integration with Edge Firmware

The ESP32-S3 edge nodes can use these databases in two ways:

1. **On-device classification** — A subset of the JSON data compiled into C++ headers for real-time classification on the edge node itself.
2. **Server-side classification** — Raw BLE/WiFi scan data sent via MQTT to the command center, where the full Python classifier runs.

For memory-constrained edge devices, prioritize:
- OUI prefix lookup (compact, fast)
- Device name patterns (regex on strings)
- Appearance value lookup (simple integer map)

The full database including Apple Continuity parsing and mDNS analysis runs on the command center.

## Data Sources and Licensing

| Database | Source | License |
|----------|--------|---------|
| OUI prefixes | IEEE MA-L registry | Public data |
| Company IDs | Bluetooth SIG Assigned Numbers | Public specification |
| Appearance Values | Bluetooth SIG Assigned Numbers | Public specification |
| Service UUIDs | Bluetooth SIG Assigned Numbers | Public specification |
| Apple Continuity | furiousMAC/continuity, academic papers | MIT / academic |
| Google Fast Pair | DiamondRoPlayz/FastPair-Models | Community research |
| SSID patterns | Original compilation | Original |
| DHCP fingerprints | RFC 2132, Fingerbank | Public standards |
| mDNS services | IANA registry, community | Public standards |

All data is sourced from public specifications, open-source projects, and academic research. No proprietary databases or licensed data are used.
