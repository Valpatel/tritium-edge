# BLE Device Fingerprinting Reference

Comprehensive lookup tables for identifying BLE devices by their advertisement data.
Used by tritium-edge BLE classifier to categorize detected targets.

## Table of Contents

1. [Apple Continuity Protocol](#apple-continuity-protocol)
2. [Android Fast Pair](#android-fast-pair)
3. [BLE GAP Appearance Values](#ble-gap-appearance-values)
4. [BLE Service UUIDs](#ble-service-uuids)
5. [Bluetooth Company Identifiers](#bluetooth-company-identifiers)
6. [OUI to Device Type Mapping](#oui-to-device-type-mapping)
7. [Integration Guide](#integration-guide)

---

## Apple Continuity Protocol

Apple devices broadcast BLE advertisements using manufacturer-specific data with company ID `0x004C`. The data follows a Type-Length-Value (TLV) format where the first byte after the company ID identifies the message type.

**Source**: [furiousMAC/continuity](https://github.com/furiousMAC/continuity) — reverse-engineered Wireshark dissector from the US Naval Academy.

### Continuity Message Types

| Type Code | Message Name | Description |
|-----------|-------------|-------------|
| 0x01 | Observed on iOS | General iOS broadcast |
| 0x02 | iBeacon | Location beacon |
| 0x03 | AirPrint | Printer discovery |
| 0x05 | AirDrop | File sharing discovery |
| 0x06 | HomeKit | Smart home accessory |
| 0x07 | Proximity Pairing | AirPods/Beats pairing |
| 0x08 | Hey Siri | Voice assistant activation |
| 0x09 | AirPlay Destination | AirPlay receiver |
| 0x0A | AirPlay Source | AirPlay sender |
| 0x0B | Magic Switch | Device switching |
| 0x0C | Handoff | Task continuity between devices |
| 0x0D | Tethering Target | Wi-Fi Settings / hotspot target |
| 0x0E | Tethering Source | Instant Hotspot source |
| 0x0F | Nearby Action | Device setup/pairing actions |
| 0x10 | Nearby Info | Device state broadcast |
| 0x12 | Find My | Lost device / AirTag tracking |

### Device Class Codes (from Nearby Info / Nearby Action)

Extracted from the `device_class_vals` table in the Wireshark dissector.

| Code | Device Class |
|------|-------------|
| 0x02 | iPhone |
| 0x04 | iPod |
| 0x06 | iPad |
| 0x08 | HomePod |
| 0x0A | Mac (MacBook/iMac/Mac Pro) |
| 0x0C | Apple TV |
| 0x0E | Apple Watch |

### Nearby Info Action Codes

Nearby Info messages (type 0x10) are broadcast continuously and reveal device usage state.

| Action Code | Device State |
|-------------|-------------|
| 0x00 | Activity unknown |
| 0x01 | Activity reporting disabled |
| 0x03 | Idle user (screen off) |
| 0x05 | Audio playing, screen locked |
| 0x07 | Active user (screen on) |
| 0x09 | Screen on with video |
| 0x0A | Watch on wrist, unlocked |
| 0x0B | Recent user interaction |
| 0x0D | User driving vehicle (CarPlay) |
| 0x0E | Phone/FaceTime call in progress |

### Nearby Info Status Flags (4-bit bitmask)

| Bit | Meaning |
|-----|---------|
| 0x01 | Primary iCloud account device |
| 0x04 | AirDrop receiving enabled |

### Nearby Info Data Flags (8-bit bitmask)

| Bit | Meaning |
|-----|---------|
| 0x01 | AirPods connected, screen on |
| 0x04 | WiFi enabled |
| 0x10 | Authentication tag present |
| 0x20 | Apple Watch locked |
| 0x40 | Watch auto-unlock enabled |
| 0x80 | Device auto-unlock enabled |

### Nearby Action Types

| Code | Action Type |
|------|------------|
| 0x01 | Apple TV Setup |
| 0x04 | Mobile Backup |
| 0x05 | Watch Setup |
| 0x06 | Apple TV Pair |
| 0x07 | Internet Relay |
| 0x08 | WiFi Password |
| 0x09 | iOS Setup |
| 0x0A | Repair |
| 0x0B | Speaker Setup |
| 0x0C | Apple Pay |
| 0x0D | Whole Home Audio Setup |
| 0x0E | Developer Tools Pairing Request |
| 0x0F | Answered Call |
| 0x10 | Ended Call |
| 0x11 | DD Ping |
| 0x12 | DD Pong |
| 0x13 | Remote Auto Fill |
| 0x14 | Companion Link Proximity |
| 0x15 | Remote Management |
| 0x16 | Remote Auto Fill Pong |
| 0x17 | Remote Display |

### Proximity Pairing Device Models (Type 0x07)

The proximity pairing message carries a device model byte at offset 7 in the advertisement payload (after `0x4C 0x00 0x07 0x19 0x07`). This identifies the specific Apple audio product.

**Source**: [ECTO-1A/AppleJuice](https://github.com/ECTO-1A/AppleJuice)

| Model Byte | Device |
|------------|--------|
| 0x02 | AirPods (Gen 1) |
| 0x03 | PowerBeats |
| 0x05 | BeatsX |
| 0x06 | Beats Solo3 |
| 0x09 | Beats Studio3 |
| 0x0A | AirPods Max |
| 0x0B | PowerBeats Pro |
| 0x0C | Beats Solo Pro |
| 0x0E | AirPods Pro |
| 0x0F | AirPods (Gen 2) |
| 0x10 | Beats Flex |
| 0x11 | Beats Studio Buds |
| 0x12 | Beats Fit Pro |
| 0x13 | AirPods (Gen 3) |
| 0x14 | AirPods Pro (Gen 2) |
| 0x16 | Beats Studio Buds+ |
| 0x17 | Beats Studio Pro |

**Known Proximity Pairing Full Model Words**:
- AirPods Pro: `0x0E20`

### Device Identification via BLE Flags

From the PETS 2019 paper "Handoff All Your Privacy":
- **iPhones/iPads/iPods/Watches**: BLE flags H=1, C=1, LE=0
- **MacBooks**: BLE flags H=0, C=0, LE=1
- **AirPods**: No BLE flags present (unique identifier)

---

## Android Fast Pair

Google's Fast Pair protocol uses BLE service UUID `0xFE2C` with a 24-bit encrypted Model ID in advertisements. Model IDs are registered with Google and matched against a cloud-synced database.

**Source**: [Google Fast Pair Specification](https://developers.google.com/nearby/fast-pair/specifications/introduction), [DiamondRoPlayz/FastPair-Models](https://github.com/DiamondRoPlayz/FastPair-Models)

### How It Works

1. Device advertises with service UUID `0xFE2C`
2. Advertisement contains encrypted 24-bit Model ID
3. Android device matches against cloud database
4. One-tap pairing prompt appears within ~1 meter

### Notable Fast Pair Model IDs

| Model ID | Device |
|----------|--------|
| 0x0000F0 | Bose QuietComfort 35 II |
| 0x000006 | Google Pixel Buds |
| 0x0582FD | Pixel Buds |
| 0x92BBBD | Pixel Buds |
| 0x038F16 | Beats Studio Buds |
| 0x01EEB4 | Sony WH-1000XM4 |
| 0x058D08 | Sony WH-1000XM4 |
| 0xD446A7 | Sony XM5 |
| 0x2D7A23 | Sony WF-1000XM4 |
| 0x00C95C | Sony WF-1000X |
| 0xCD8256 | Bose NC 700 |
| 0x0100F0 | Bose QuietComfort 35 II |
| 0x72EF8D | Razer Hammerhead TWS X |
| 0x0E30C3 | Razer Hammerhead TWS |
| 0x00AA48 | Jabra Elite 2 |
| 0x821F66 | JBL Flip 6 |
| 0xF52494 | JBL Buds Pro |
| 0x718FA4 | JBL Live 300TWS |
| 0x0577B1 | Galaxy S23 Ultra |
| 0x05A9BC | Galaxy S20+ |
| 0x06AE20 | Galaxy S21 5G |
| 0x057802 | TicWatch Pro 5 |
| 0x050F0C | Marshall Major III Voice |
| 0x052CC7 | Marshall MINOR III |
| 0x05A963 | UE WONDERBOOM 3 |
| 0x038B91 | Denon AH-C830NCW |
| 0x0744B6 | Technics EAH-AZ60M2 |
| 0x07A41C | Sony WF-C700N |
| 0x03C99C | Moto Buds 135 |
| 0x06C197 | OPPO Enco Air3 Pro |
| 0x00A168 | boAt Airdopes 621 |

### Fast Pair Device Categories

Fast Pair devices are primarily:
- **Headphones/Earbuds** (vast majority)
- **Speakers** (portable Bluetooth speakers)
- **Smartwatches** (Wear OS devices)
- **Smartphones** (for initial setup)
- **Android Auto** head units

---

## BLE GAP Appearance Values

The Generic Access Profile (GAP) Appearance characteristic (UUID `0x2A01`) identifies device type. The value is a 16-bit number where bits 15-6 define the category and bits 5-0 define the subcategory.

**Source**: [Bluetooth SIG Assigned Numbers](https://www.bluetooth.com/specifications/assigned-numbers/), [TI BLE Stack](https://software-dl.ti.com/simplelink/esd/simplelink_cc13x0_sdk/2.20.00.38/exports/docs/blestack/blestack-api/group__GAP__APPEARANCE__VALUES.html)

### Categories

| Value | Category | Tritium Classification |
|-------|----------|----------------------|
| 0x0000 | Unknown | unknown |
| 0x0040 | Generic Phone | phone |
| 0x0080 | Generic Computer | computer |
| 0x00C0 | Generic Watch | watch |
| 0x00C1 | Sports Watch | watch |
| 0x0100 | Generic Clock | iot_device |
| 0x0140 | Generic Display | iot_device |
| 0x0180 | Generic Remote Control | iot_device |
| 0x01C0 | Generic Eye-glasses | wearable |
| 0x0200 | Generic Tag | tag |
| 0x0240 | Generic Keyring | tag |
| 0x0280 | Generic Media Player | audio |
| 0x02C0 | Generic Barcode Scanner | iot_device |
| 0x0300 | Generic Thermometer | medical |
| 0x0301 | Thermometer: Ear | medical |
| 0x0340 | Generic Heart Rate Sensor | fitness |
| 0x0341 | Heart Rate Belt | fitness |
| 0x0380 | Generic Blood Pressure | medical |
| 0x0381 | Blood Pressure: Arm | medical |
| 0x0382 | Blood Pressure: Wrist | medical |
| 0x03C0 | Generic HID | hid |
| 0x03C1 | HID Keyboard | keyboard |
| 0x03C2 | HID Mouse | mouse |
| 0x03C3 | HID Joystick | gamepad |
| 0x03C4 | HID Gamepad | gamepad |
| 0x03C5 | HID Digitizer Tablet | tablet |
| 0x03C6 | HID Card Reader | iot_device |
| 0x03C7 | HID Digital Pen | stylus |
| 0x03C8 | HID Barcode Scanner | iot_device |
| 0x0440 | Generic Glucose Meter | medical |
| 0x0480 | Generic Running/Walking Sensor | fitness |
| 0x04C0 | Generic Cycling Sensor | fitness |
| 0x0500 | Generic Control Device | iot_device |
| 0x0540 | Generic Network Device | network |
| 0x0580 | Generic Sensor | sensor |
| 0x05C0 | Generic Light Fixture | smart_home |
| 0x0600 | Generic Fan | smart_home |
| 0x0640 | Generic HVAC | smart_home |
| 0x0680 | Generic Air Conditioning | smart_home |
| 0x06C0 | Generic Humidifier | smart_home |
| 0x0700 | Generic Heating | smart_home |
| 0x0740 | Generic Access Control | security |
| 0x0780 | Generic Motorized Device | iot_device |
| 0x07C0 | Generic Power Device | iot_device |
| 0x0800 | Generic Light Source | smart_home |
| 0x0840 | Generic Window Covering | smart_home |
| 0x0880 | Generic Audio Sink | audio |
| 0x08C0 | Generic Audio Source | audio |
| 0x0900 | Generic Motorized Vehicle | vehicle |
| 0x0940 | Generic Domestic Appliance | smart_home |
| 0x0980 | Generic Wearable Audio Device | audio |
| 0x09C0 | Generic Aircraft | vehicle |
| 0x0A00 | Generic AV Equipment | audio |
| 0x0A40 | Generic Display Equipment | iot_device |
| 0x0A80 | Generic Hearing Aid | medical |
| 0x0AC0 | Generic Gaming | gamepad |
| 0x0B00 | Generic Signage | iot_device |
| 0x0BC0 | Generic Pulse Oximeter | medical |
| 0x0C00 | Generic Weight Scale | medical |
| 0x0C40 | Generic Personal Mobility Device | vehicle |
| 0x0CC0 | Generic Continuous Glucose Monitor | medical |
| 0x0D00 | Generic Insulin Pump | medical |
| 0x0D40 | Generic Medication Delivery | medical |
| 0x0DC0 | Generic Spirometer | medical |

---

## BLE Service UUIDs

Standard 16-bit GATT Service UUIDs reveal device capabilities and help classify device type.

**Source**: [Bluetooth SIG GATT Services](https://www.bluetooth.com/specifications/assigned-numbers/), [Nordic Semiconductor bluetooth-numbers-database](https://github.com/NordicSemiconductor/bluetooth-numbers-database)

### Service UUIDs for Device Classification

| UUID | Service Name | Indicates Device Type |
|------|-------------|----------------------|
| 0x1800 | Generic Access | Any BLE device |
| 0x1801 | Generic Attribute | Any BLE device |
| 0x180A | Device Information | Any BLE device |
| 0x180D | Heart Rate | Fitness tracker/watch |
| 0x180F | Battery Service | Any battery device |
| 0x1802 | Immediate Alert | Proximity tag/finder |
| 0x1803 | Link Loss | Proximity tag/finder |
| 0x1804 | Tx Power | Beacon/tag |
| 0x1805 | Current Time Service | Watch/clock |
| 0x1808 | Glucose | Medical device |
| 0x1809 | Health Thermometer | Medical device |
| 0x180E | Phone Alert Status | Phone |
| 0x1810 | Blood Pressure | Medical device |
| 0x1811 | Alert Notification | Phone/watch |
| 0x1812 | Human Interface Device | Keyboard/mouse/gamepad |
| 0x1813 | Scan Parameters | Any BLE device |
| 0x1814 | Running Speed and Cadence | Fitness sensor |
| 0x1816 | Cycling Speed and Cadence | Fitness sensor |
| 0x1818 | Cycling Power | Fitness sensor |
| 0x1819 | Location and Navigation | GPS device |
| 0x181A | Environmental Sensing | Sensor/weather station |
| 0x181B | Body Composition | Smart scale |
| 0x181C | User Data | Fitness device |
| 0x181D | Weight Scale | Smart scale |
| 0x181F | Continuous Glucose Monitoring | Medical device |
| 0x1821 | Indoor Positioning | Beacon |
| 0x1822 | Pulse Oximeter | Medical device |
| 0x1824 | Transport Discovery | Any BLE device |
| 0x1826 | Fitness Machine | Gym equipment |
| 0x1827 | Mesh Provisioning | Mesh node |
| 0x1828 | Mesh Proxy | Mesh node |

### Vendor-Specific Service UUIDs

| UUID | Service | Vendor |
|------|---------|--------|
| 0xFE2C | Fast Pair | Google |
| 0xFEAA | Eddystone | Google |
| 0xFEED | Tile Tracker | Tile |
| 0xFD6F | Exposure Notification | Apple/Google |

### Fitbit UUID Pattern

Fitbit devices use 128-bit UUIDs with base: `adabXXXX-6e7d-4601-bda2-bffaa68956ba`

---

## Bluetooth Company Identifiers

16-bit codes assigned by the Bluetooth SIG, found in manufacturer-specific advertisement data.

**Source**: [Bluetooth SIG Assigned Numbers](https://www.bluetooth.com/specifications/assigned-numbers/), [Nordic Semiconductor bluetooth-numbers-database](https://github.com/NordicSemiconductor/bluetooth-numbers-database)

### Top 50 Manufacturers for Device Classification

| Code (Dec) | Code (Hex) | Company | Likely Device Types |
|------------|-----------|---------|-------------------|
| 1 | 0x0001 | Nokia | phone |
| 2 | 0x0002 | Intel Corp. | computer, iot_device |
| 6 | 0x0006 | Microsoft | computer, gamepad |
| 8 | 0x0008 | Motorola | phone |
| 10 | 0x000A | Qualcomm (QTIL) | phone, audio |
| 13 | 0x000D | Texas Instruments | iot_device, sensor |
| 15 | 0x000F | Broadcom | computer, phone |
| 76 | 0x004C | Apple, Inc. | phone, watch, computer, audio, tag |
| 85 | 0x0055 | Plantronics | audio |
| 87 | 0x0057 | Harman International | audio |
| 89 | 0x0059 | Nordic Semiconductor | iot_device, sensor |
| 101 | 0x0065 | HP, Inc. | computer |
| 107 | 0x006B | Polar Electro | fitness |
| 117 | 0x0075 | Samsung Electronics | phone, watch, audio |
| 120 | 0x0078 | Nike, Inc. | fitness |
| 135 | 0x0087 | Garmin International | fitness, watch |
| 158 | 0x009E | Bose Corporation | audio |
| 196 | 0x00C4 | LG Electronics | phone, audio |
| 224 | 0x00E0 | Google | phone, audio |
| 259 | 0x0103 | Bang & Olufsen | audio |
| 270 | 0x010E | Audi AG | vehicle |
| 286 | 0x011E | Skoda Auto | vehicle |
| 287 | 0x011F | Volkswagen AG | vehicle |
| 288 | 0x0120 | Porsche AG | vehicle |
| 301 | 0x012D | Sony Corporation | phone, audio, gamepad |
| 336 | 0x0150 | Pioneer | audio |
| 380 | 0x017C | Mercedes-Benz Group AG | vehicle |
| 398 | 0x018E | Google LLC | phone, audio |
| 427 | 0x01AB | Meta Platforms | vr_headset |
| 474 | 0x01DA | Logitech International SA | hid, mouse, keyboard |
| 477 | 0x01DD | Philips (Signify) | smart_home |
| 555 | 0x022B | Tesla, Inc. | vehicle |
| 709 | 0x02C5 | Lenovo | computer |
| 749 | 0x02ED | HTC Corporation | phone, vr_headset |
| 754 | 0x02F2 | GoPro, Inc. | camera |
| 866 | 0x0362 | ON Semiconductor | iot_device |
| 911 | 0x038F | Xiaomi Inc. | phone, watch, iot_device |
| 1023 | 0x03FF | Withings | fitness, medical |
| 1054 | 0x041E | Dell Computer | computer |
| 1172 | 0x0494 | Sennheiser | audio |
| 1363 | 0x0553 | Nintendo Co., Ltd. | gamepad |
| 1373 | 0x055D | Valve Corporation | gamepad |
| 1447 | 0x05A7 | Sonos Inc | audio |
| 1515 | 0x05EB | BMW AG | vehicle |
| 1551 | 0x060F | Signify Netherlands | smart_home |
| 1660 | 0x067C | Tile, Inc. | tag |
| 1678 | 0x068E | Razer Inc. | hid, audio |
| 1827 | 0x0723 | Ford Motor Company | vehicle |
| 1839 | 0x072F | OnePlus Electronics | phone |

---

## OUI to Device Type Mapping

The first 3 bytes (OUI) of a Bluetooth MAC address identify the manufacturer. Large manufacturers have multiple OUIs. Combined with company ID and advertisement data, this enables device classification.

### Apple OUI Ranges (selected)

Apple has 300+ registered OUI blocks. Key patterns:
- `00:CD:FE` — Apple (common on iPhones)
- `A4:D1:8C` — Apple
- `F0:D1:A9` — Apple
- `AC:BC:32` — Apple
- `70:56:81` — Apple

All Apple OUIs map to company ID `0x004C`. Device type is determined by Continuity message analysis (see above).

### Top OUI Manufacturer to Device Type Map

| OUI Prefix | Manufacturer | Primary Device Types |
|------------|-------------|---------------------|
| Apple OUIs | Apple, Inc. | phone, watch, computer, tablet, audio |
| Samsung OUIs | Samsung Electronics | phone, watch, tablet, audio, tv |
| Google OUIs | Google LLC | phone, audio, smart_home |
| Microsoft OUIs | Microsoft Corp. | computer, gamepad |
| Intel OUIs | Intel Corp. | computer |
| Broadcom OUIs | Broadcom | computer (embedded) |
| Qualcomm OUIs | Qualcomm | phone (embedded) |
| Espressif OUIs | Espressif (ESP32) | iot_device |
| Nordic Semi OUIs | Nordic Semiconductor | iot_device, sensor |
| Fitbit OUIs | Fitbit, Inc. | fitness |
| Garmin OUIs | Garmin International | fitness, watch |
| Tile OUIs | Tile, Inc. | tag |
| Bose OUIs | Bose Corporation | audio |
| Sony OUIs | Sony Corporation | audio, phone, gamepad |
| Xiaomi OUIs | Xiaomi Inc. | phone, watch, iot_device |
| Huawei OUIs | Huawei Technologies | phone, watch |
| LG OUIs | LG Electronics | phone, audio |
| Motorola OUIs | Motorola | phone |
| HTC OUIs | HTC Corporation | phone, vr_headset |
| Tesla OUIs | Tesla, Inc. | vehicle |

**Note**: Many modern BLE devices use random/private MAC addresses, making OUI-based identification unreliable for privacy-preserving devices. Apple devices rotate their MAC address every ~15 minutes. Android devices also randomize by default since Android 10. OUI lookup is most useful for IoT devices and older hardware that use fixed public addresses.

---

## Integration Guide

### How to Use These Tables in Tritium-Edge BLE Classifier

The BLE classifier in tritium-edge should check these data sources in priority order:

#### 1. Check Manufacturer-Specific Data (Most Reliable)

```
If manufacturer_data starts with company_id 0x004C (Apple):
    Parse Continuity TLV:
    - Type 0x07 → AirPods/Beats (check model byte for specific product)
    - Type 0x10 → Nearby Info (check device_class for iPhone/iPad/Mac/Watch)
    - Type 0x0F → Nearby Action (check action_type for setup context)
    - Type 0x12 → Find My (AirTag or lost device)
    - Type 0x02 → iBeacon (location beacon)

If manufacturer_data starts with company_id 0x00E0 or 0x018E (Google):
    Check for Fast Pair service UUID 0xFE2C
    Extract 24-bit Model ID, look up in database
```

#### 2. Check GAP Appearance (if present)

```
If appearance value present in advertisement:
    Map category (bits 15-6) to device type
    0x0040 → phone
    0x0080 → computer
    0x00C0 → watch
    0x0340 → fitness
    etc.
```

#### 3. Check Service UUIDs (if present)

```
If service UUIDs in advertisement:
    0x180D (Heart Rate) → fitness tracker
    0x1812 (HID) → keyboard/mouse/gamepad
    0x180E (Phone Alert) → phone
    0xFE2C (Fast Pair) → Google-paired audio device
    0xFEAA (Eddystone) → Google beacon
    0xFEED → Tile tracker
    0xFD6F → COVID exposure notification
```

#### 4. Check Company ID

```
Look up company_id in manufacturer data:
    0x004C → Apple (parse Continuity for specifics)
    0x0075 → Samsung (phone/watch/audio)
    0x0087 → Garmin (fitness/watch)
    0x009E → Bose (audio)
    0x067C → Tile (tag)
    etc.
```

#### 5. Fall Back to OUI Lookup

```
If no manufacturer data or service UUIDs:
    Extract OUI (first 3 bytes of MAC)
    Look up manufacturer
    Map to general device category
    NOTE: May be randomized MAC — check bit 1 of first byte
    If (mac[0] & 0x02) → random address, OUI unreliable
```

### Classification Priority

For target tracking, classify devices in this order of confidence:

1. **Apple Continuity device_class** — directly identifies device type (highest confidence)
2. **GAP Appearance** — standard BLE device type identification
3. **Service UUIDs** — reveals device capabilities
4. **Company ID + known patterns** — manufacturer-based classification
5. **Fast Pair Model ID** — specific product identification
6. **OUI lookup** — manufacturer guess (lowest confidence, unreliable with random MACs)

### Machine-Readable Data

The lookup tables are available in JSON format at:
`tritium-lib/src/tritium_lib/data/ble_fingerprints.json`

This file contains all tables from this document in a format directly loadable by the BLE classifier.

---

## References

- [furiousMAC/continuity](https://github.com/furiousMAC/continuity) — Apple Continuity Protocol reverse engineering
- [ECTO-1A/AppleJuice](https://github.com/ECTO-1A/AppleJuice) — Apple proximity pairing device codes
- [Google Fast Pair Specification](https://developers.google.com/nearby/fast-pair/specifications/introduction)
- [DiamondRoPlayz/FastPair-Models](https://github.com/DiamondRoPlayz/FastPair-Models) — Fast Pair model ID database
- [Nordic Semiconductor bluetooth-numbers-database](https://github.com/NordicSemiconductor/bluetooth-numbers-database) — Company IDs, Service UUIDs, Appearance values
- [Bluetooth SIG Assigned Numbers](https://www.bluetooth.com/specifications/assigned-numbers/)
- [ESPresense](https://espresense.com/apple/) — ESP32-based Apple device fingerprinting
- Martin et al., "Handoff All Your Privacy" (PETS 2019) — Apple Continuity protocol analysis
- Celosia & Cunche, "Discontinued Privacy" (PETS 2020) — Apple BLE data leaks
