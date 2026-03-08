# ESP32-S3 Library Ecosystem for Tritium

Reference document mapping the essential libraries, frameworks, and tools available for ESP32-S3 development. Covers what we use, what we evaluated, and what to adopt next.

**Platform**: pioarduino (Arduino ESP32 v3.x on ESP-IDF 5.x)
**Framework**: Arduino
**Build system**: PlatformIO

---

## Table of Contents

1. [BLE](#1-ble)
2. [WiFi / Networking](#2-wifi--networking)
3. [Display](#3-display)
4. [Sensors](#4-sensors)
5. [Audio](#5-audio)
6. [Camera](#6-camera)
7. [Storage](#7-storage)
8. [Security](#8-security)
9. [OTA](#9-ota)
10. [Mesh Networking](#10-mesh-networking)
11. [AI/ML on Edge](#11-aiml-on-edge)
12. [Time](#12-time)
13. [Power Management](#13-power-management)
14. [Protocol Buffers / Serialization](#14-protocol-buffers--serialization)
15. [Testing](#15-testing)
16. [Self-Seeding SD Card](#16-self-seeding-sd-card)
17. [Acoustic Modem / Data-over-Audio](#17-acoustic-modem--data-over-audio)
18. [Offline GIS / Map Tiles](#18-offline-gis--map-tiles)
19. [Fleet Management (Commercial Reference)](#19-fleet-management-commercial-reference)

---

## 1. BLE

### NimBLE-Arduino (our choice)

| | |
|---|---|
| **lib_deps** | `h2zero/NimBLE-Arduino@^2.3.8` |
| **Source** | https://github.com/h2zero/NimBLE-Arduino |
| **What it does** | Full BLE stack (central, peripheral, broadcaster, observer) built on Apache NimBLE |
| **Status** | **In use** — `lib/hal_ble`, `lib/hal_ble_ota`, `lib/hal_ble_scanner` |

**Why NimBLE over Bluedroid:**

| Metric | NimBLE | Bluedroid |
|---|---|---|
| Flash usage | ~110 KB | ~350 KB |
| RAM usage | ~15 KB idle | ~70 KB idle |
| Max connections | 9 | 4 (default) |
| Compile time | Fast | Slow |
| API | Clean C++ | Verbose, Java-like |

**Compatibility notes:**
- Arduino ESP32 v3.x bundles NimBLE in the ESP-IDF layer, so for basic BLE you *could* skip the external library. However, `h2zero/NimBLE-Arduino` provides a much cleaner Arduino-native API and is what we use.
- v2.x is the current major version line. The 1.x API is substantially different — do not mix.
- Works with pioarduino out of the box. No special build flags needed beyond adding the lib_dep.

**Our HALs:**
- `hal_ble` — BLE manager with connection handling, GATT services
- `hal_ble_ota` — BLE-based OTA firmware updates
- `hal_ble_scanner` — Passive BLE environment scanner (sensor node)

---

## 2. WiFi / Networking

### Built-in WiFi (ESP-IDF / Arduino WiFi.h)

| | |
|---|---|
| **lib_deps** | None (part of framework) |
| **What it does** | STA, AP, STA+AP modes; WPA2/WPA3; mDNS; SmartConfig |
| **Status** | **In use** — `lib/hal_wifi` wraps this with multi-network NVS persistence |

No external library needed. Arduino ESP32 v3.x exposes the full ESP-IDF WiFi stack through `WiFi.h`. Our `hal_wifi` adds:
- Multiple saved networks with fallback
- NVS-backed credential storage
- Connection state machine with auto-reconnect

### ESPAsyncWebServer

| | |
|---|---|
| **lib_deps** | `ESP32Async/ESPAsyncWebServer@^3.8.0` |
| **Source** | https://github.com/ESP32Async/ESPAsyncWebServer |
| **What it does** | Async HTTP/WebSocket server; handles requests without blocking loop() |
| **Status** | **Should adopt** — our `hal_webserver` currently uses the synchronous ESP-IDF HTTP server |

**Important:** Use the `ESP32Async` fork, not the original `me-no-dev` version. The original is abandoned and incompatible with Arduino ESP32 v3.x / ESP-IDF 5.x. The ESP32Async fork is actively maintained and compatible.

Requires AsyncTCP (pulled in automatically as a dependency):

| | |
|---|---|
| **lib_deps** | `ESP32Async/AsyncTCP@^3.3.2` |

### MQTT

**PubSubClient (evaluated):**

| | |
|---|---|
| **lib_deps** | `knolleary/PubSubClient@^2.8` |
| **What it does** | Synchronous MQTT 3.1.1 client |
| **Status** | **Evaluated, not active** — commented out in platformio.ini |

Our `hal_mqtt` is a custom lightweight implementation. PubSubClient works but has limitations:
- Synchronous — blocks during publish/subscribe
- 256-byte default message size (configurable but awkward)
- No MQTT 5.0 support

**Async alternatives to consider:**

| Library | lib_deps | Notes |
|---|---|---|
| AsyncMqttClient | `marvinroger/async-mqtt-client@^0.9.0` | Non-blocking, but abandoned upstream |
| esp_mqtt (built-in) | None | ESP-IDF component, event-driven, MQTT 5.0 capable. Best option for new code. |

**Recommendation:** For new MQTT work, use the ESP-IDF `esp_mqtt` client directly. It is event-driven, supports MQTT 5.0, and requires no external dependency.

---

## 3. Display

### esp_lcd (our choice)

| | |
|---|---|
| **lib_deps** | None (part of ESP-IDF, bundled with Arduino ESP32 v3.x) |
| **What it does** | Hardware-abstracted LCD panel driver framework — SPI, QSPI, I2C, I80, RGB parallel |
| **Status** | **In use** — `lib/display/` with custom board configs and panel drivers |

Our display stack:
- `lib/display/display.h` — unified display init for all 6 boards
- `lib/display/drivers/` — custom esp_lcd panel drivers (AXS15231B, SH8601Z)
- `lib/display/boards/` — per-board pin/timing configs
- Works with LVGL's esp_lcd flush callback

### LVGL

| | |
|---|---|
| **lib_deps** | `lvgl/lvgl@~9.2.0` |
| **Source** | https://github.com/lvgl/lvgl |
| **What it does** | UI widget toolkit — buttons, charts, lists, animations, themes |
| **Status** | **In use** — `lib/hal_ui` provides LVGL init, theming, and widget helpers |

**Configuration:** LVGL is configured via `lv_conf.h` or build flags (`-DLV_CONF_INCLUDE_SIMPLE`). Version 9.x is the current major line; the API changed significantly from 8.x.

**Key LVGL 9.x changes from 8.x:**
- Display/input driver registration API rewritten
- `lv_disp_draw_buf_init` replaced with new buffer API
- Style system simplified
- Better memory management

### TFT_eSPI (not recommended)

| | |
|---|---|
| **lib_deps** | `bodmer/TFT_eSPI@^2.5.43` |
| **What it does** | Popular display driver for SPI/parallel TFTs |
| **Why we don't use it** | No QSPI support, no built-in panel abstraction, config via User_Setup.h is fragile for multi-board projects. Does not support our AXS15231B or SH8601Z panels. |

### LovyanGFX (previous choice, migrated away)

| | |
|---|---|
| **lib_deps** | `lovyan03/LovyanGFX@^1.2.0` |
| **What it does** | Display driver with sprite engine, multi-panel support, QSPI |
| **Why we moved away** | We migrated to esp_lcd for tighter ESP-IDF integration, better LVGL compatibility via the standard esp_lcd flush path, and to avoid the LovyanGFX I2C bus ownership problem (lgfx::i2c conflicts with Arduino Wire on shared buses). See commit `71ecfe9`. |

LovyanGFX is excellent for projects that need its sprite engine or don't use LVGL. For our use case (LVGL-based UI on esp_lcd), it added unnecessary complexity.

---

## 4. Sensors

### Our Custom HALs

| HAL | Device | Bus | Notes |
|---|---|---|---|
| `hal_imu` | QMI8658 | I2C | 6-axis accel+gyro, dual-mode I2C (Wire / lgfx) |
| `hal_power` | AXP2101 | I2C | PMIC: battery, charging, voltage rails, ADC |
| `hal_rtc` | PCF85063 | I2C | Real-time clock with alarm |
| `hal_touch` | AXS15231B / CST816 | I2C | Touch input for display panels |
| `hal_io_expander` | TCA9554 | I2C | GPIO expander for display reset (3.5B-C) |

### Third-Party Sensor Libraries

| Library | lib_deps | Use case |
|---|---|---|
| Adafruit Unified Sensor | `adafruit/Adafruit Unified Sensor@^1.1.14` | Common interface for Adafruit sensor libs |
| Adafruit BusIO | `adafruit/Adafruit BusIO@^1.16.2` | I2C/SPI abstraction used by all Adafruit libs |
| SparkFun Qwiic libs | Various (`sparkfun/SparkFun ...`) | SparkFun I2C sensor breakouts |

**Our approach:** We write custom HALs instead of using Adafruit/SparkFun libraries because:
- Smaller footprint (no Adafruit abstraction layers)
- Dual-mode I2C support (Arduino Wire + lgfx::i2c) needed for bus sharing
- Direct register access for full feature coverage
- No dependency chains (Adafruit libs pull in BusIO, Unified Sensor, etc.)

**When to use third-party libs:** For one-off sensor prototyping or sensors we don't have HALs for. Don't fight writing a custom driver for a BME280 when Adafruit's works fine.

---

## 5. Audio

### Our ES8311 HAL

| | |
|---|---|
| **lib_deps** | None (custom HAL) |
| **Location** | `lib/hal_audio` |
| **Status** | **In use** — ES8311 codec + I2S, mic + speaker, spectrum analysis |

Key details:
- ES8311 codec configured over I2C (dual-mode), audio over I2S
- 16kHz 16-bit stereo, APLL-derived MCLK at 4.096 MHz
- Critical: CSM (register 0x00=0x80) must be enabled or ADC returns zeros

### ESP-ADF (Espressif Audio Development Framework)

| | |
|---|---|
| **Source** | https://github.com/espressif/esp-adf |
| **What it does** | Full audio pipeline: codecs, streams, effects, Bluetooth audio |
| **Compatibility** | ESP-IDF only, NOT compatible with Arduino framework |
| **Status** | **Not usable** in our Arduino-based project |

If we ever move to pure ESP-IDF, ESP-ADF would replace hal_audio. Until then, our custom HAL is the right choice.

### I2S Driver (built-in)

The ESP-IDF I2S driver is available through Arduino ESP32 v3.x. Our hal_audio uses it directly. Note that ESP-IDF 5.x deprecated the legacy `i2s.h` driver in favor of `driver/i2s_std.h` / `driver/i2s_tdm.h` / `driver/i2s_pdm.h`.

---

## 6. Camera

### esp32-camera

| | |
|---|---|
| **lib_deps** | None (bundled with Arduino ESP32) |
| **Source** | https://github.com/espressif/esp32-camera |
| **What it does** | DVP camera interface for OV2640, OV5640, etc. |
| **Status** | **In use** — `lib/hal_camera` wraps this for OV5640 on 3.5B-C |

**Key gotchas:**
- Claims LEDC_TIMER_0 + LEDC_CHANNEL_0 for XCLK generation — avoid timer conflicts with backlight PWM
- OV5640 needs `setFlip(false, true)` for correct orientation on 3.5B-C
- QVGA (320x240) achieves ~3.8 FPS full-screen on 320x480 QSPI; VGA is too slow
- PSRAM required for frame buffers larger than QVGA

**Alternatives:** None worth considering. esp32-camera is the only maintained camera driver for ESP32-S3 DVP interface.

---

## 7. Storage

### LittleFS (our choice)

| | |
|---|---|
| **lib_deps** | None (built into Arduino ESP32 v3.x) |
| **What it does** | Wear-leveled flash filesystem, power-loss resilient |
| **Status** | **In use** — `lib/hal_fs` wraps LittleFS with config helpers |

### SPIFFS (deprecated)

| | |
|---|---|
| **Status** | **Do not use** — deprecated in ESP-IDF 5.x, no directory support, poor wear leveling |

LittleFS is the direct replacement. Migration is straightforward (same API surface).

### SD / SD_MMC

| | |
|---|---|
| **lib_deps** | None (built into Arduino ESP32 v3.x) |
| **What it does** | SD card access via SPI (`SD.h`) or SDMMC 1-bit/4-bit (`SD_MMC.h`) |
| **Status** | **In use** — `lib/hal_sdcard` uses SDMMC 1-bit mode with format + performance testing |

**Prefer SD_MMC** over SD (SPI mode) when hardware supports it — significantly faster throughput.

### NVS Preferences

| | |
|---|---|
| **lib_deps** | None (built into Arduino ESP32 v3.x) |
| **What it does** | Key-value store in flash, persistent across reboots |
| **Status** | **In use** — `hal_wifi` stores credentials, `hal_provision` stores device identity |

NVS is ideal for small config values (WiFi credentials, device IDs, calibration). Do not store large blobs — use LittleFS for that.

---

## 8. Security

### mbedTLS (built-in)

| | |
|---|---|
| **lib_deps** | None (part of ESP-IDF) |
| **What it does** | TLS 1.2/1.3, X.509 certs, AES, SHA, RSA, ECDSA, ECDH |
| **Status** | Available — used implicitly by WiFi (WPA2-Enterprise), HTTPS, MQTT over TLS |

ESP32-S3 has hardware acceleration for AES and SHA, which mbedTLS uses automatically.

### WolfSSL

| | |
|---|---|
| **lib_deps** | `wolfssl/wolfssl@^5.7.0` |
| **What it does** | Alternative TLS stack, smaller footprint than mbedTLS |
| **Status** | **Not needed** — mbedTLS is already built in and hardware-accelerated |

Only consider WolfSSL if you need DTLS (for CoAP/UDP) or specific cipher suites mbedTLS doesn't cover.

### ESP32 Secure Boot & Flash Encryption

| | |
|---|---|
| **What it does** | Verifies firmware signature on boot (Secure Boot v2), encrypts flash contents |
| **Configuration** | `menuconfig` / `sdkconfig` — not configurable from Arduino layer |
| **Status** | **Should adopt for production** — requires ESP-IDF menuconfig or sdkconfig.defaults |

**Production checklist:**
- [ ] Enable Secure Boot v2 (RSA-PSS 3072-bit)
- [ ] Enable Flash Encryption (AES-256-XTS)
- [ ] Burn eFuses to lock boot mode (one-time, irreversible)
- [ ] Store signing keys offline, never on build machines
- [ ] Test OTA with signed firmware before locking eFuses

---

## 9. OTA

### Our hal_ota

| | |
|---|---|
| **Location** | `lib/hal_ota` |
| **What it does** | OTA via WiFi (push/pull), SD card, dual partition rollback |
| **Status** | **In use** |

### hal_ble_ota

| | |
|---|---|
| **Location** | `lib/hal_ble_ota` |
| **What it does** | OTA firmware update over BLE (NimBLE) |
| **Status** | **In use** |

### ESP-IDF OTA (built-in)

| | |
|---|---|
| **lib_deps** | None |
| **What it does** | `esp_ota_ops.h` — write to OTA partition, set boot partition, rollback |
| **Status** | **In use** — hal_ota wraps this |

Key API: `esp_ota_begin()`, `esp_ota_write()`, `esp_ota_end()`, `esp_ota_set_boot_partition()`.

**Important:** On Arduino ESP32, use `esp_ota_get_app_description()` (NOT `esp_app_get_description()` which is ESP-IDF 5.x native only).

### ArduinoOTA

| | |
|---|---|
| **lib_deps** | None (built into Arduino ESP32) |
| **What it does** | Simple OTA via WiFi, compatible with Arduino IDE and PlatformIO upload |
| **Status** | **Not recommended** — no rollback, no verification, no partial update support. Fine for development, not for production. |

**OTA partition scheme:** We use `partitions_ota_16MB.csv` with dual OTA partitions for rollback support.

---

## 10. Mesh Networking

### ESP-NOW (our choice)

| | |
|---|---|
| **lib_deps** | None (part of ESP-IDF) |
| **What it does** | Connectionless 802.11 peer-to-peer, 250-byte packets, <1ms latency |
| **Status** | **In use** — `lib/hal_espnow` with multi-hop flooding mesh, dedup, PING/PONG discovery |

**Advantages:** No WiFi AP needed, extremely low latency, works alongside WiFi STA mode.
**Limitations:** 250-byte max payload, no built-in routing (we implement flooding), 20 peer limit (ESP-IDF default, configurable).

### painlessMesh

| | |
|---|---|
| **lib_deps** | `painlessmesh/painlessMesh@^1.4.5` |
| **Source** | https://gitlab.com/painlessMesh/painlessMesh |
| **What it does** | Self-healing WiFi mesh with automatic topology management |
| **Status** | **Evaluated, not adopted** |

**Why we don't use it:**
- Uses WiFi AP+STA mode, which conflicts with normal WiFi connectivity
- Higher latency than ESP-NOW (~50-200ms vs <1ms)
- JSON-based messaging adds overhead
- Our ESP-NOW mesh with flooding is simpler and meets our needs

### ESP-WIFI-MESH (ESP-MDF)

| | |
|---|---|
| **Source** | https://github.com/espressif/esp-mdf |
| **What it does** | Espressif's official WiFi mesh framework, tree topology |
| **Compatibility** | ESP-IDF only, NOT compatible with Arduino framework |
| **Status** | **Not usable** — requires pure ESP-IDF |

---

## 11. AI/ML on Edge

### TensorFlow Lite Micro (TFLM)

| | |
|---|---|
| **Source** | https://github.com/espressif/esp-tflite-micro |
| **What it does** | Run TF Lite models on microcontrollers — classification, keyword spotting, anomaly detection |
| **Compatibility** | Primarily ESP-IDF. PlatformIO integration is possible but requires manual setup. |
| **Status** | **Should evaluate** for future voice/sensor ML |

**ESP32-S3 performance:** 15-20 FPS inference at 240 MHz for small models. The S3's vector instructions (ESP-NN) accelerate common NN operations 3-5x over generic C.

**PlatformIO integration path:**
1. Clone esp-tflite-micro as a local library
2. Add ESP-NN as a component for hardware acceleration
3. Convert models with `xxd` to C arrays or load from LittleFS/SD

### ESP-NN

| | |
|---|---|
| **Source** | https://github.com/espressif/esp-nn |
| **What it does** | Optimized neural network kernels for ESP32-S3 (SIMD, loop unrolling) |
| **Status** | **Should adopt** alongside TFLM |

### Our hal_voice (MFCC/DTW)

| | |
|---|---|
| **Location** | `lib/hal_voice` |
| **What it does** | Voice activity detection + keyword spotting using MFCC features and DTW template matching |
| **Status** | **In use** — lightweight, no ML framework needed |

Our approach is simpler than TFLM but limited to template matching. For more complex voice commands or multi-class classification, migrate to TFLM.

**Note:** esp-sr (WakeNet/MultiNet) requires ESP-IDF and is NOT compatible with Arduino framework.

---

## 12. Time

### Our hal_ntp

| | |
|---|---|
| **Location** | `lib/hal_ntp` |
| **What it does** | NTP time sync, timezone support, drift estimation |
| **Status** | **In use** |

### ESP-IDF SNTP (built-in)

| | |
|---|---|
| **lib_deps** | None |
| **What it does** | `esp_sntp.h` — SNTP client with smooth/step sync modes |
| **Status** | **In use** — hal_ntp wraps this |

Arduino ESP32 v3.x exposes `configTime()` and `configTzTime()` which use SNTP internally. Our hal_ntp provides higher-level features (drift tracking, timezone management).

### PCF85063 RTC

| | |
|---|---|
| **Location** | `lib/hal_rtc` |
| **What it does** | Hardware RTC for timekeeping across deep sleep / power loss |
| **Status** | **In use** on boards with PCF85063 (3.5B-C) |

**Best practice:** Sync RTC from NTP on boot, use RTC as fallback when WiFi is unavailable.

---

## 13. Power Management

### ESP-IDF Power Management (built-in)

| | |
|---|---|
| **lib_deps** | None |
| **What it does** | Dynamic frequency scaling, light sleep, deep sleep, wake sources |
| **Status** | **In use** — wrapped by hal_sleep and hal_power |

### Our HALs

| HAL | What it does |
|---|---|
| `hal_sleep` | Light/deep sleep with wake sources, display sleep/wake, touch wake |
| `hal_power` | AXP2101 PMIC control — battery voltage, charge current, rail enable/disable, ADC |

**Power optimization checklist:**
- [ ] Disable unused peripherals before sleep (WiFi, BLE, camera)
- [ ] Use `esp_wifi_stop()` not just disconnect
- [ ] Configure GPIO hold for output states during deep sleep
- [ ] Use ULP coprocessor for periodic wake if available (not on all S3 variants)
- [ ] AXP2101 can cut power rails to peripherals independently

---

## 14. Protocol Buffers / Serialization

### Nanopb (recommended)

| | |
|---|---|
| **lib_deps** | `nanopb/Nanopb@^0.4.9` |
| **Source** | https://github.com/nanopb/nanopb |
| **What it does** | Protocol Buffers for embedded — tiny footprint, no dynamic allocation |
| **Status** | **Should adopt** for device-to-server communication |

**Why Nanopb:**
- Compile-time code generation from .proto files
- No malloc — all buffers statically sized
- ~2-5 KB code size per message type
- Compatible with any protobuf implementation on the server side

**Workflow:**
1. Define messages in `.proto` files
2. Run `protoc --nanopb_out=. message.proto` to generate `.pb.c` and `.pb.h`
3. Include generated files in your PlatformIO project
4. Encode/decode with `pb_encode()` / `pb_decode()`

### MessagePack

| | |
|---|---|
| **lib_deps** | `bblanchon/ArduinoJson@^7.3.0` (includes MessagePack support) |
| **What it does** | Binary JSON — smaller than JSON, faster to parse, schemaless |
| **Status** | **Consider for ad-hoc messaging** |

ArduinoJson supports both JSON and MessagePack with the same API. Good for flexible telemetry where schema enforcement isn't critical.

### CBOR

| | |
|---|---|
| **lib_deps** | `intel/tinycbor@^0.6.0` (or use ArduinoJson which also supports CBOR in v7) |
| **What it does** | Concise Binary Object Representation — IETF standard, similar to MessagePack |
| **Status** | **Lower priority** — MessagePack via ArduinoJson is simpler |

**Recommendation:** Use Nanopb for structured device-to-server protocols (telemetry, commands). Use ArduinoJson with MessagePack for flexible/ad-hoc data. Avoid raw JSON over MQTT — the parsing overhead and bandwidth cost are unnecessary on constrained devices.

---

## 15. Testing

### Unity (ESP-IDF built-in)

| | |
|---|---|
| **lib_deps** | None (part of ESP-IDF) |
| **What it does** | C test framework — assertions, test runners, test suites |
| **Status** | Available but not actively used |

### ArduinoUnit / AUnit

| | |
|---|---|
| **lib_deps** | `bxparks/AUnit@^1.7.1` |
| **What it does** | Unit testing framework designed for Arduino, runs on-device |
| **Status** | **Should evaluate** |

### PlatformIO Test Framework

| | |
|---|---|
| **What it does** | Built-in test runner — runs tests on native (host) or on-device |
| **Configuration** | `test/` directory, `pio test -e <env>` |
| **Status** | **Should adopt** |

PlatformIO's test framework can run Unity or custom test suites. Supports:
- `test_embedded/` — runs on target hardware
- `test_native/` — runs on host (x86), useful for logic testing without hardware

### Our Current Approach

We use the `apps/test` app and `apps/diag` app for hardware validation. This is integration testing, not unit testing. For HAL logic (parsers, state machines, protocol encoding), we should add PlatformIO native tests.

**Recommended testing strategy:**
1. **Native tests** (`pio test -e native`) for pure logic: parsers, serialization, state machines
2. **On-device tests** (`pio test -e <board>`) for HAL integration: I2C communication, display init, WiFi connect
3. **Test app** (`apps/test`) for full-system hardware validation

---

## 16. Self-Seeding SD Card

A Tritium node's SD card should carry everything needed to bootstrap, update, or recover any node in the fleet. This is the "seed" concept — any node with a properly loaded SD card can provision itself or its neighbors.

### Directory Layout

```
/sd/
  firmware/
    touch-amoled-241b/
      starfield.bin           # Default app firmware
      system.bin              # System dashboard firmware
      manifest.json           # Version, SHA-256, board type, build date
    touch-lcd-35bc/
      starfield.bin
      camera.bin
      system.bin
      manifest.json
    touch-lcd-349/
      starfield.bin
      manifest.json
    ...                       # One directory per board type

  source/
    tritium-edge.bundle       # git bundle of the full repo
    README.txt                # Instructions for cloning from bundle

  config/
    wifi_networks.json        # Known WiFi SSIDs + passwords
    mqtt_broker.json          # MQTT broker address, port, credentials
    fleet_config.json         # Fleet ID, node roles, update policy
    device_identity/
      <MAC>.json              # Per-device config (name, role, location)
    templates/
      default_device.json     # Template for new device provisioning

  certs/
    ca.pem                    # Fleet CA certificate
    device/
      <MAC>.crt               # Per-device TLS certificate
      <MAC>.key               # Per-device private key (encrypted)
    ota_signing.pub           # OTA firmware signing public key

  models/
    wake_word.tflite          # Wake word detection model
    keyword_templates/        # DTW keyword templates for hal_voice
      hey_tritium.bin
      stop.bin
    anomaly_detector.tflite   # Sensor anomaly detection model

  logs/
    crash/                    # Crash dumps copied from NVS on boot
    telemetry/                # Offline telemetry buffer

  media/
    splash.bin                # Boot splash screen (raw RGB565)
    fonts/                    # Custom fonts for LVGL
```

### manifest.json Format

```json
{
  "board": "touch-lcd-35bc",
  "firmware": [
    {
      "app": "starfield",
      "file": "starfield.bin",
      "version": "1.4.2",
      "sha256": "a1b2c3d4...",
      "built": "2026-03-07T10:00:00Z",
      "min_hal_version": "2.0.0"
    }
  ]
}
```

### Self-Seeding Workflow

1. **Boot** — Node checks SD card for `config/device_identity/<MAC>.json`. If found, applies device config.
2. **WiFi** — Loads `config/wifi_networks.json`, attempts connection in priority order.
3. **Firmware check** — Compares running firmware version against `firmware/<board>/manifest.json`. If newer firmware available, triggers OTA from SD.
4. **Provisioning** — If no device identity exists, generates one from template and writes back to SD.
5. **Certificate import** — Loads fleet CA and device certs from `certs/`.
6. **Peer update** — If connected to other nodes via ESP-NOW/BLE, can push firmware to peers that report older versions.
7. **Telemetry flush** — If offline telemetry exists in `logs/telemetry/`, uploads to server when WiFi connects.

### Building a Seed Card

```bash
# Build all firmware variants
for board in touch-amoled-241b touch-lcd-35bc touch-lcd-349; do
  for app in starfield system; do
    pio run -e ${board}-${app}
    cp .pio/build/${board}-${app}/firmware.bin sd/firmware/${board}/${app}.bin
  done
done

# Create git bundle
git bundle create sd/source/tritium-edge.bundle --all

# Generate manifests
python tools/generate_manifests.py sd/firmware/

# Copy config templates
cp config/templates/* sd/config/templates/
```

### Security Considerations

- **Private keys on SD:** Device private keys (`<MAC>.key`) should be encrypted with a fleet-wide passphrase or derived from a device-specific secret (e.g., eFuse). Never store plaintext private keys.
- **Firmware signing:** All firmware binaries should be signed. Nodes verify against `certs/ota_signing.pub` before applying OTA.
- **WiFi passwords:** `wifi_networks.json` contains plaintext passwords. The SD card itself is the security boundary — physical access to the card grants network access. Consider encrypting this file with a device-derived key.
- **SD card loss:** A lost SD card exposes WiFi credentials and fleet topology. Use unique per-device keys and rotate fleet credentials if a card is lost.

---

## Quick Reference: All lib_deps

Copy-paste block for platformio.ini:

```ini
; Core (already in use)
lib_deps =
    lvgl/lvgl@~9.2.0
    h2zero/NimBLE-Arduino@^2.3.8

; Networking (adopt as needed)
;   ESP32Async/ESPAsyncWebServer@^3.8.0
;   ESP32Async/AsyncTCP@^3.3.2

; Serialization (adopt as needed)
;   nanopb/Nanopb@^0.4.9
;   bblanchon/ArduinoJson@^7.3.0

; Testing (adopt as needed)
;   bxparks/AUnit@^1.7.1

; Mesh (evaluated, not adopted)
;   painlessmesh/painlessMesh@^1.4.5
```

---

## 17. Acoustic Modem / Data-over-Audio

### ESP32Modem

| | |
|---|---|
| **Source** | https://github.com/mikegofton/ESP32Modem |
| **What it does** | ASK/FSK modulation using ESP32 LEDC (LED PWM) hardware |
| **Status** | Evaluated — our `hal_acoustic_modem` uses I2S instead for bidirectional audio (speaker + mic) |

### SoftModem

| | |
|---|---|
| **Source** | https://github.com/arms22/SoftModem |
| **What it does** | Bell 202-like FSK encoding at 1225 bps via audio jack |
| **Status** | Reference only — ATmega focused, not ESP32 native |

**Why custom over ESP32Modem:** ESP32Modem uses LEDC PWM (output-only). Our
acoustic modem needs bidirectional I2S for both transmission (speaker) and
reception (microphone). The I2S approach also gives us better frequency control
and DMA-based buffer management.

---

## 18. Offline GIS / Map Tiles

### IceNav-v3

| | |
|---|---|
| **Source** | https://github.com/jgauchia/IceNav-v3 |
| **What it does** | Full GPS navigator with offline OSM tiles on SD card, LVGL 9 + LovyanGFX |
| **Status** | Reference — same tile format (`{z}/{x}/{y}.png`), same display stack |

Uses standard OSM slippy map tile layout. Includes mass-copy scripts optimized for
small-file SD card transfers. Supports zoom levels 6-17 via Maperitive tile generation.

### OpenStreetMap-esp32

| | |
|---|---|
| **Source** | https://github.com/CelliesProjects/OpenStreetMap-esp32 |
| **What it does** | Online tile fetching with PSRAM LRU cache, LovyanGFX sprite output |
| **Status** | Candidate for online tile download + caching to SD |

Dual-core tile decode, 128KB per 256px tile in PSRAM. Could complement our offline
`hal_gis` by downloading tiles on first access when WiFi is available.

### ESP32_GPS

| | |
|---|---|
| **Source** | https://github.com/aresta/ESP32_GPS |
| **What it does** | Vector map rendering from OSM PBF extracts, custom binary format |
| **Status** | Reference — vector approach uses less storage than raster tiles |

---

## 19. Fleet Management (Commercial Reference)

### Golioth

| | |
|---|---|
| **Source** | https://golioth.io |
| **What it does** | Commercial fleet management for ESP-IDF devices — OTA, logging, settings |
| **Status** | Competitive reference — Tritium does this open-source and self-hosted |

### Espressif ESP-MDF

| | |
|---|---|
| **Source** | https://github.com/espressif/esp-mdf |
| **What it does** | ESP32 mesh networking framework (ESP-IDF only, not Arduino) |
| **Status** | Reference — we use ESP-NOW mesh instead (works with Arduino framework) |

---

## Decision Log

| Decision | Choice | Rationale |
|---|---|---|
| BLE stack | NimBLE over Bluedroid | 60% less flash, 80% less RAM, cleaner API |
| Display driver | esp_lcd over LovyanGFX | Native LVGL integration, no I2C bus conflicts |
| UI framework | LVGL 9.x | Industry standard, widget-rich, active development |
| Filesystem | LittleFS over SPIFFS | Wear leveling, directory support, power-safe |
| MQTT | Custom hal_mqtt over PubSubClient | Lighter weight, async-friendly, no 256-byte limit |
| Mesh | ESP-NOW over painlessMesh | Sub-ms latency, no WiFi AP conflict |
| Serialization | Nanopb (planned) over JSON | Binary, schema-enforced, no malloc |
| OTA | Custom hal_ota over ArduinoOTA | Rollback, verification, multi-source (WiFi/SD/BLE) |
| Sensor drivers | Custom HALs over Adafruit | Smaller, dual-mode I2C, no dependency chains |
| Voice | MFCC/DTW over esp-sr | Works with Arduino framework, no ESP-IDF dependency |
| Acoustic modem | Custom I2S over ESP32Modem | Bidirectional (speaker+mic), DMA buffers, better frequency control |
| GIS tiles | Custom hal_gis over IceNav | Same tile format, but decoupled from GPS/nav — pure tile serving and caching |
| Fleet management | Custom server over Golioth | Self-hosted, open source, no cloud dependency |
