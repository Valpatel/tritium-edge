# ESP-IDF 4.x to 5.x API Migration Guide

Migration reference for moving from PlatformIO `espressif32` (Arduino ESP32 v2.x / ESP-IDF 4.4)
to pioarduino (Arduino ESP32 v3.3.7 / ESP-IDF 5.5.2).

---

## Table of Contents

1. [platformio.ini Changes](#1-platformioini-changes)
2. [I2S Driver (BREAKING)](#2-i2s-driver-breaking)
3. [Task Watchdog (BREAKING)](#3-task-watchdog-breaking)
4. [ESP-NOW Callbacks (BREAKING)](#4-esp-now-callbacks-breaking)
5. [BLE / NimBLE (BREAKING)](#5-ble--nimble-breaking)
6. [SNTP / NTP (DEPRECATION)](#6-sntp--ntp-deprecation)
7. [Sleep APIs (DEPRECATION)](#7-sleep-apis-deprecation)
8. [Camera (LIBRARY UPDATE)](#8-camera-library-update)
9. [OTA APIs (DEPRECATION)](#9-ota-apis-deprecation)
10. [SD/MMC (MINOR)](#10-sdmmc-minor)
11. [WiFi (MINOR)](#11-wifi-minor)
12. [Build Flags (CLEANUP)](#12-build-flags-cleanup)
13. [LovyanGFX / LVGL Compatibility](#13-lovyangfx--lvgl-compatibility)
14. [Summary Table](#14-summary-table)

---

## 1. platformio.ini Changes

**File:** `/home/scubasonar/Code/esp32-hardware/platformio.ini`

### Platform Declaration (Line 24)

**Old (v2.x):**
```ini
[env]
platform = espressif32
```

**New (v3.3.7 / ESP-IDF 5.5.2):**
```ini
[env]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.37/platform-espressif32.zip
```

The exact URL will depend on the pioarduino release that matches Arduino ESP32 v3.3.7.
Check https://github.com/pioarduino/platform-espressif32/releases for the correct version.

### Build Flags (Lines 36-42)

Remove `-mfix-esp32-psram-cache-issue` (line 39). This flag is:
- **Not needed for ESP32-S3** -- it was a workaround for an original ESP32 rev 0/1 silicon bug
- **Not recognized by newer GCC** toolchains shipped with ESP-IDF 5.x and may cause a build error

Replace `-DBOARD_HAS_PSRAM` with the appropriate board-level PSRAM config. In Arduino ESP32 v3,
PSRAM is typically enabled via `board_build.psram_type` in platformio.ini rather than a build flag.

### Library Dependencies (Lines 31-35)

```ini
lib_deps =
    lovyan03/LovyanGFX@^1.2.0          ; Check for v1.2.x ESP-IDF 5 compatibility
    lvgl/lvgl@~9.2.0                    ; LVGL 9.x should be compatible
    espressif/esp32-camera@^2.0.0       ; Need v2.0.13+ for ESP-IDF 5.x
    knolleary/PubSubClient@^2.8         ; OK, pure Arduino
    h2zero/NimBLE-Arduino@^2.0.0        ; MUST upgrade from ^1.4 to ^2.0 for ESP-IDF 5
```

**Breaking:** NimBLE-Arduino 1.4.x does NOT support Arduino ESP32 v3.x / ESP-IDF 5.x. Must use v2.0+.

---

## 2. I2S Driver (BREAKING)

**File:** `/home/scubasonar/Code/esp32-hardware/lib/hal_audio/hal_audio.cpp`
**Severity:** BREAKING -- will not compile

The legacy I2S driver (`driver/i2s.h`) is deprecated in ESP-IDF 5.0 and may be removed in future
versions. While ESP-IDF 5.x still ships it under `driver/deprecated/driver/i2s.h`, the Arduino
ESP32 v3 wrapper may not expose all legacy APIs.

### Lines Requiring Changes

| Line | Old API | New API | Notes |
|------|---------|---------|-------|
| 29 | `#include <driver/i2s.h>` | `#include <driver/i2s_std.h>` | New header |
| 204 | `i2s_config_t i2s_config = {};` | `i2s_chan_config_t` + `i2s_std_config_t` | Struct redesign |
| 205 | `I2S_MODE_MASTER \| I2S_MODE_TX \| I2S_MODE_RX` | Separate TX/RX channel handles | Architecture change |
| 207 | `I2S_BITS_PER_SAMPLE_16BIT` | `I2S_DATA_BIT_WIDTH_16BIT` | Enum rename |
| 208 | `I2S_CHANNEL_FMT_RIGHT_LEFT` | `I2S_SLOT_MODE_STEREO` | Enum rename |
| 209 | `I2S_COMM_FORMAT_STAND_I2S` | Part of `i2s_std_slot_config_t` | Moved to slot config |
| 211-212 | `dma_buf_count`, `dma_buf_len` | `dma_desc_num`, `dma_frame_num` | Field rename |
| 213 | `use_apll = true` | APLL config in clock config | Moved |
| 215 | `fixed_mclk` | `mclk_multiple` in clock config | Moved |
| 217 | `i2s_driver_install(port, &config, 0, NULL)` | `i2s_new_channel()` + `i2s_channel_init_std_mode()` | API redesign |
| 222-241 | `i2s_pin_config_t` + `i2s_set_pin()` | Pin config in `i2s_std_gpio_config_t` | Merged into init |
| 305, 314 | `i2s_write(port, buf, len, &written, timeout)` | `i2s_channel_write(tx_handle, buf, len, &written, timeout)` | Handle-based |
| 323, 331 | `i2s_read(port, buf, len, &read, timeout)` | `i2s_channel_read(rx_handle, buf, len, &read, timeout)` | Handle-based |
| 243 | `i2s_driver_uninstall(port)` | `i2s_del_channel(handle)` | Handle-based |

### New I2S Architecture

The minimum control unit is now tx/rx **channels** instead of a whole I2S controller.
TX and RX channels are initialized, started, and stopped separately.

**Migration pattern:**
```cpp
// Old (ESP-IDF 4.x)
i2s_config_t config = { ... };
i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
i2s_set_pin(I2S_NUM_0, &pin_config);

// New (ESP-IDF 5.x)
i2s_chan_handle_t tx_handle, rx_handle;
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);

i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
        .mclk = AUDIO_I2S_MCLK,
        .bclk = AUDIO_I2S_BCLK,
        .ws   = AUDIO_I2S_WS,
        .dout = AUDIO_I2S_DOUT,
        .din  = AUDIO_I2S_DIN,
    },
};
i2s_channel_init_std_mode(tx_handle, &std_cfg);
i2s_channel_init_std_mode(rx_handle, &std_cfg);
i2s_channel_enable(tx_handle);
i2s_channel_enable(rx_handle);
```

**Workaround:** If you need a quick fix, ESP-IDF 5.x still ships the legacy driver under
`driver/deprecated/driver/i2s.h`. You can suppress deprecation warnings with
`CONFIG_I2S_SUPPRESS_DEPRECATE_WARN=y` in sdkconfig, but this is not recommended long-term.

---

## 3. Task Watchdog (BREAKING)

**File:** `/home/scubasonar/Code/esp32-hardware/lib/hal_watchdog/hal_watchdog.cpp`
**Severity:** BREAKING -- will not compile

### Lines Requiring Changes

| Line | Old API | New API |
|------|---------|---------|
| 108 | `esp_task_wdt_init(timeout_s, true)` | `esp_task_wdt_init(&config)` with `esp_task_wdt_config_t` |
| 111-112 | `esp_task_wdt_deinit()` then `esp_task_wdt_init(timeout_s, true)` | `esp_task_wdt_reconfigure(&config)` |
| 119 | `esp_task_wdt_add(nullptr)` | `esp_task_wdt_add(NULL)` (same, but init now auto-subscribes idle tasks) |
| 138 | `esp_task_wdt_delete(nullptr)` | Same API, still works |
| 145 | `esp_task_wdt_reset()` | Same API, still works |

### New init signature (ESP-IDF 5.x)

```cpp
// Old (ESP-IDF 4.x)
esp_task_wdt_init(timeout_s, true);  // (uint32_t timeout, bool panic)

// New (ESP-IDF 5.x)
esp_task_wdt_config_t config = {
    .timeout_ms = timeout_s * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,  // Watch all cores
    .trigger_panic = true,
};
esp_task_wdt_init(&config);
```

**Also note:** `esp_task_wdt_deinit()` still exists but `esp_task_wdt_reconfigure()` is the
preferred way to change timeout at runtime without deinit/reinit.

### Config Changes

- `CONFIG_ESP_TASK_WDT` renamed to `CONFIG_ESP_TASK_WDT_EN`
- New `CONFIG_ESP_TASK_WDT_INIT` option controls whether WDT is initialized at startup
- Timeout is now in **milliseconds** (was seconds)

---

## 4. ESP-NOW Callbacks (BREAKING)

**File:** `/home/scubasonar/Code/esp32-hardware/lib/hal_espnow/hal_espnow.cpp`
**Severity:** BREAKING -- will not compile

### Receive Callback Signature Change

**Lines 42, 95, 322 (declaration in hal_espnow.h)**

```cpp
// Old (ESP-IDF 4.x) - line 42
static void onRecvStatic(const uint8_t* mac, const uint8_t* data, int len);

// New (ESP-IDF 5.x)
static void onRecvStatic(const esp_now_recv_info_t* info, const uint8_t* data, int len);
```

The first parameter changed from a bare MAC address pointer to an `esp_now_recv_info_t` struct:
```cpp
typedef struct {
    uint8_t *src_addr;    // Source MAC (replaces old mac parameter)
    uint8_t *des_addr;    // Destination MAC (new - can detect unicast vs broadcast)
    wifi_pkt_rx_ctrl_t *rx_ctrl;  // Rx control info (RSSI, channel, etc.)
} esp_now_recv_info_t;
```

### Required Changes

| Location | Change |
|----------|--------|
| `hal_espnow.h` line 322 | Change declaration to use `esp_now_recv_info_t*` |
| `hal_espnow.cpp` line 42 | Change function signature |
| `hal_espnow.cpp` line 44-50 | Replace `mac` with `info->src_addr` |
| `hal_espnow.cpp` line 45 | Can get RSSI from `info->rx_ctrl->rssi` instead of promiscuous mode hack |
| `hal_espnow.cpp` line 95 | Remove the `(esp_now_recv_cb_t)` cast -- use correct type directly |

### Send Callback Signature Change

**Line 59, 96, 323**

```cpp
// Old (ESP-IDF 4.x)
static void onSendStatic(const uint8_t* mac, uint8_t status);

// New (ESP-IDF 5.x) -- status type changed to esp_now_send_status_t
static void onSendStatic(const uint8_t* mac, esp_now_send_status_t status);
```

### Bonus: RSSI Without Promiscuous Mode

Lines 98-100 enable promiscuous mode solely to capture RSSI. With ESP-IDF 5.x,
`esp_now_recv_info_t::rx_ctrl->rssi` provides RSSI directly. You can remove:
- Line 99: `esp_wifi_set_promiscuous(true);`
- Line 100: `esp_wifi_set_promiscuous_rx_cb(promiscRxCb);`
- Line 130: `esp_wifi_set_promiscuous(false);`
- The entire `promiscRxCb` function and `s_lastRxRssi` static variable

---

## 5. BLE / NimBLE (BREAKING)

### hal_ble (Bluedroid API) -- BREAKING

**File:** `/home/scubasonar/Code/esp32-hardware/lib/hal_ble/ble_manager.cpp`
**Severity:** BREAKING -- Arduino ESP32 v3 defaults to NimBLE, Bluedroid API changed

In Arduino ESP32 v3, the default BLE stack is NimBLE. The code uses Bluedroid-style includes:

| Line | Issue |
|------|-------|
| 23 | `#include <BLEDevice.h>` -- In v3, this resolves to NimBLE wrappers |
| 24 | `#include <BLEServer.h>` -- Same |
| 25 | `#include <BLE2902.h>` -- **REMOVED in NimBLE 2.x**. Descriptors are auto-created |
| 78 | `txChar->addDescriptor(new BLE2902())` -- **MUST REMOVE** |
| 89 | `advertising->setMinPreferred(0x06)` -- **REMOVED in NimBLE 2.x** |

### Required Changes for ble_manager.cpp

1. **Remove** `#include <BLE2902.h>` (line 25)
2. **Remove** `txChar->addDescriptor(new BLE2902())` (line 78) -- NimBLE auto-creates CCCD
3. **Remove** `advertising->setMinPreferred(0x06)` (line 89) -- API removed
4. **Update** `characteristic->getValue()` return type -- returns `NimBLEAttValue` not `std::string` in NimBLE 2.x
5. Consider migrating to explicit `NimBLE*` class names for clarity

### hal_ble_ota (Already Uses NimBLE) -- MINOR

**File:** `/home/scubasonar/Code/esp32-hardware/lib/hal_ble_ota/hal_ble_ota.cpp`

This file already uses `NimBLEDevice.h` directly, but:

| Line | Issue |
|------|-------|
| 99 | `adv->setMinPreferred(0x06)` -- **REMOVED in NimBLE 2.x**, must remove |

### NimBLE-Arduino Version

**Must upgrade from `^1.4.0` to `^2.0.0`** in `platformio.ini` lib_deps.
NimBLE-Arduino 1.4.x does not support ESP-IDF 5.x.

---

## 6. SNTP / NTP (DEPRECATION)

**File:** `/home/scubasonar/Code/esp32-hardware/lib/hal_ntp/hal_ntp.cpp`
**Severity:** Deprecation warning (still compiles, but should migrate)

### Lines Requiring Changes

| Line | Old API | New API |
|------|---------|---------|
| 109 | `#include <esp_sntp.h>` | `#include <esp_netif_sntp.h>` |
| 115 | `configTime(0, 0, server1, server2)` | `esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server1);` + `esp_netif_sntp_init(&config);` |
| 128 | `sntp_get_sync_status()` | `esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100))` or check directly |

The `esp_sntp` API is not thread-safe. The `esp_netif_sntp` API is the recommended replacement
for ESP-IDF 5.x:

```cpp
// Old
#include <esp_sntp.h>
configTime(0, 0, "pool.ntp.org");
while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) { ... }

// New
#include <esp_netif_sntp.h>
esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
esp_netif_sntp_init(&config);
esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
```

**Note:** Arduino's `configTime()` wrapper may still work in v3, but using the ESP-IDF API
directly gives more control and is thread-safe.

---

## 7. Sleep APIs (DEPRECATION)

**File:** `/home/scubasonar/Code/esp32-hardware/lib/hal_sleep/hal_sleep.cpp`
**Severity:** Deprecation (still works in ESP-IDF 5.5, deprecated in 5.3, removed in 6.0)

### Lines with Deprecation Warnings

| Line | Old API | New API | Status |
|------|---------|---------|--------|
| 141 | `esp_sleep_enable_ext1_wakeup(mask, mode)` | `esp_sleep_enable_ext1_wakeup_io(mask, mode)` | Deprecated in 5.3, removed in 6.0 |
| 157 | `touchSleepWakeUpEnable(pin, threshold)` | Check Arduino ESP32 v3 touch API -- signature may differ | Touch API reworked in v3 |

### Touch Wake API Change

In Arduino ESP32 v3, the touch API was reworked:
- `touchSleepWakeUpEnable()` may have changed signature
- Touch sensor API uses `touch_pad_*` functions from ESP-IDF 5.x with different config structs

**Action:** Test compilation first. If `touchSleepWakeUpEnable` is still provided by Arduino v3
core, it may just work. Otherwise, migrate to ESP-IDF touch_pad_sleep API.

---

## 8. Camera (LIBRARY UPDATE)

**File:** `/home/scubasonar/Code/esp32-hardware/lib/hal_camera/hal_camera.cpp`
**Severity:** Library version dependency

The `esp32-camera` library needs to be compatible with ESP-IDF 5.x:

- The camera API itself (`esp_camera_init`, `esp_camera_fb_get`, etc.) has not changed
  significantly between ESP-IDF 4.x and 5.x
- **LEDC changes may affect camera XCLK generation** -- camera uses `LEDC_TIMER_0` / `LEDC_CHANNEL_0`
  (lines 99-100). LEDC API may have minor struct changes in ESP-IDF 5.x but the camera library
  handles this internally
- Ensure `espressif/esp32-camera@^2.0.13` or later for ESP-IDF 5.5 compatibility

### Potential Issue: Camera + SCCB I2C

Line 82-83: `pin_sccb_sda` / `pin_sccb_scl` -- the camera library in ESP-IDF 5.x may use the
new I2C master driver internally. If the camera library version is correct, this should be
handled transparently.

**Action:** Update library version in platformio.ini. Test that camera init still works.

---

## 9. OTA APIs (DEPRECATION)

**Files:**
- `/home/scubasonar/Code/esp32-hardware/lib/hal_ota/hal_ota.cpp`
- `/home/scubasonar/Code/esp32-hardware/lib/hal_heartbeat/hal_heartbeat.cpp`
- `/home/scubasonar/Code/esp32-hardware/lib/hal_ble_ota/hal_ble_ota.cpp`

**Severity:** Deprecation warning (functional, but should migrate)

### esp_ota_get_app_description() -> esp_app_get_description()

| File | Line | Old | New |
|------|------|-----|-----|
| hal_ota.cpp | 475 | `esp_ota_get_app_description()` | `esp_app_get_description()` |
| hal_ota.cpp | 1159 | `esp_ota_get_app_description()` | `esp_app_get_description()` |
| hal_heartbeat.cpp | 283 | `esp_ota_get_app_description()` | `esp_app_get_description()` |
| hal_ble_ota.cpp | 205 | `esp_ota_get_app_description()` | `esp_app_get_description()` |

The old API is maintained for backward compatibility but `esp_app_get_description()` is the
recommended API in ESP-IDF 5.x. Both return the same `esp_app_desc_t*`.

**Include change:** Add `#include <esp_app_desc.h>` alongside `#include <esp_ota_ops.h>`.

### Other OTA APIs -- UNCHANGED

The following OTA APIs used in our code are unchanged in ESP-IDF 5.x:
- `esp_ota_get_running_partition()`
- `esp_ota_get_next_update_partition()`
- `esp_ota_mark_app_valid_cancel_rollback()`
- `esp_ota_set_boot_partition()`
- `esp_ota_get_last_invalid_partition()`
- `esp_partition_read()`

### Arduino Update Library

**File:** `/home/scubasonar/Code/esp32-hardware/apps/ota/ota_app.cpp` (lines 7, 491, 551, etc.)

The Arduino `Update` library is still available in v3. The API (`Update.begin()`, `Update.write()`,
`Update.end()`, `Update.abort()`, `Update.errorString()`) is compatible. No changes needed.

---

## 10. SD/MMC (MINOR)

**Files:**
- `/home/scubasonar/Code/esp32-hardware/lib/hal_sdcard/hal_sdcard.cpp`
- `/home/scubasonar/Code/esp32-hardware/src/main.cpp`

**Severity:** Mostly compatible, minor changes possible

### Arduino SD_MMC Library

The `SD_MMC` class API (`begin()`, `setPins()`, `open()`, `exists()`, `remove()`, etc.) is
maintained in Arduino ESP32 v3. The main differences:

- `SD_MMC.begin()` parameter order may differ slightly -- verify signature
- `SD_MMC.format()` availability -- test compilation

### Low-level SDMMC APIs

**File:** `hal_sdcard.cpp` lines 222-251

```cpp
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
```

These headers are still available in ESP-IDF 5.x. The `SDMMC_HOST_DEFAULT()` macro and
`sdmmc_card_init()` / `sdmmc_read_sectors()` / `sdmmc_write_sectors()` APIs are unchanged.

**Action:** Test compilation. Should work without changes.

---

## 11. WiFi (MINOR)

**File:** `/home/scubasonar/Code/esp32-hardware/lib/hal_wifi/wifi_manager.cpp`
**Severity:** Minor -- mostly compatible

### WiFi.macAddress() Issue

Known issue in Arduino ESP32 v3: `WiFi.macAddress()` may return `00:00:00:00:00:00` if called
before WiFi is fully initialized. This affects:

| File | Line | Usage |
|------|------|-------|
| system_app.cpp | 1028 | `WiFi.macAddress().c_str()` |
| hal_heartbeat.cpp | 129, 409 | `WiFi.macAddress().c_str()` |
| hal_webserver.cpp | 360, 597 | `WiFi.macAddress(mac)` |
| hal_mqtt.cpp | 47 | `WiFi.macAddress(mac)` |

**Fix:** Ensure `WiFi.mode(WIFI_STA)` is called before `WiFi.macAddress()`. Our code already
does this in `wifi_manager.cpp` line 65, but standalone uses outside the wifi manager should
verify WiFi is initialized first.

### WiFi Event Callback

**File:** `wifi_manager.cpp` line 36

```cpp
static void wifiEventHandler(WiFiEvent_t event) {
```

The `WiFiEvent_t` enum values (`ARDUINO_EVENT_WIFI_STA_GOT_IP`, etc.) are the same in v3.
The `WiFi.onEvent()` API is compatible.

### esp_wifi_get_mac()

**File:** `hal_espnow.cpp` line 106

```cpp
esp_wifi_get_mac(WIFI_IF_STA, _mac);
```

In ESP-IDF 5.x, `WIFI_IF_STA` changed to `ESP_IF_WIFI_STA`. However, Arduino ESP32 v3 may
define a compatibility alias. **Test compilation** -- if it fails, replace:
- `WIFI_IF_STA` -> `ESP_IF_WIFI_STA`

### NVS (via Preferences)

The `Preferences` class used in `wifi_manager.cpp` (lines 192-236) wraps NVS. This API is
unchanged in Arduino ESP32 v3. No changes needed.

---

## 12. Build Flags (CLEANUP)

**File:** `/home/scubasonar/Code/esp32-hardware/platformio.ini`

### Flags to Remove

| Line | Flag | Reason |
|------|------|--------|
| 39 | `-mfix-esp32-psram-cache-issue` | ESP32-S3 is unaffected by this bug. Flag may cause errors with ESP-IDF 5.x GCC 12+ toolchain |

### Flags to Verify

| Line | Flag | Status |
|------|------|--------|
| 37 | `-DARDUINO_USB_CDC_ON_BOOT=1` | Still valid in v3, but may be configured differently via board config |
| 38 | `-DBOARD_HAS_PSRAM` | May need to use `board_build.psram_type = opi` instead |
| 40 | `-DLV_CONF_INCLUDE_SIMPLE` | Still valid for LVGL 9.x |

### New Flags to Consider

```ini
build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DLV_CONF_INCLUDE_SIMPLE
    -DDBG_LEVEL=4
    -Iinclude
    ; ESP-IDF 5.x specific:
    ; -DCONFIG_I2S_SUPPRESS_DEPRECATE_WARN=1  ; Only if using legacy I2S temporarily
```

---

## 13. LovyanGFX / LVGL Compatibility

### LovyanGFX

LovyanGFX v1.2.x supports ESP-IDF 5.x. The `lgfx::i2c::` namespace APIs used extensively
in our HALs should continue to work:

- `lgfx::i2c::init()` -- used in hal_imu, Touch_AXS15231B
- `lgfx::i2c::transactionWrite()` -- used in hal_audio, hal_imu, hal_power, hal_rtc
- `lgfx::i2c::transactionWriteRead()` -- used in hal_audio, hal_imu, hal_power, hal_rtc, Touch_AXS15231B

**Action:** Verify LovyanGFX version supports ESP-IDF 5.5.2 specifically. Check LovyanGFX
releases/issues for any ESP-IDF 5.5 compatibility notes.

### LVGL 9.x

LVGL 9.2 should be compatible with ESP-IDF 5.x. No API changes expected in our usage.

---

## 14. Summary Table

| Component | File(s) | Severity | Effort |
|-----------|---------|----------|--------|
| **I2S Driver** | lib/hal_audio/hal_audio.cpp | BREAKING | HIGH -- full rewrite of init/read/write |
| **Task Watchdog** | lib/hal_watchdog/hal_watchdog.cpp | BREAKING | LOW -- change init() call + struct |
| **ESP-NOW Callbacks** | lib/hal_espnow/hal_espnow.cpp + .h | BREAKING | MEDIUM -- change recv callback signature + cleanup |
| **BLE (Bluedroid->NimBLE)** | lib/hal_ble/ble_manager.cpp | BREAKING | MEDIUM -- remove BLE2902, update API calls |
| **NimBLE Version** | platformio.ini | BREAKING | LOW -- version bump |
| **NimBLE setMinPreferred** | hal_ble_ota.cpp, ble_manager.cpp | BREAKING | LOW -- remove call |
| **SNTP** | lib/hal_ntp/hal_ntp.cpp | DEPRECATION | LOW -- swap to esp_netif_sntp |
| **Sleep ext1** | lib/hal_sleep/hal_sleep.cpp | DEPRECATION | LOW -- rename function |
| **OTA app_desc** | hal_ota, hal_heartbeat, hal_ble_ota | DEPRECATION | LOW -- swap function name |
| **PSRAM flag** | platformio.ini | CLEANUP | TRIVIAL -- remove flag |
| **Platform** | platformio.ini | REQUIRED | LOW -- change platform URL |
| **WiFi MAC** | Multiple files | MINOR | LOW -- verify init order |
| **WIFI_IF_STA** | hal_espnow.cpp | POSSIBLE BREAK | TRIVIAL -- rename if needed |
| **Camera** | lib/hal_camera/hal_camera.cpp | LIBRARY | LOW -- update lib version |
| **SD_MMC** | lib/hal_sdcard/hal_sdcard.cpp | MINOR | LOW -- test compilation |
| **WebServer** | lib/hal_webserver/hal_webserver.cpp | COMPATIBLE | NONE -- Arduino WebServer unchanged |
| **LittleFS** | lib/hal_fs/hal_fs.cpp | COMPATIBLE | NONE -- API unchanged |
| **Preferences/NVS** | lib/hal_wifi/wifi_manager.cpp | COMPATIBLE | NONE -- API unchanged |
| **LovyanGFX** | include/display_init.h + all HALs | VERIFY | LOW -- check version compat |
| **LVGL** | lib/hal_ui/* | COMPATIBLE | NONE |

### Recommended Migration Order

1. **platformio.ini** -- platform, flags, lib versions (gets it to attempt compilation)
2. **hal_watchdog** -- quick win, isolated change
3. **hal_espnow** -- callback signature + cleanup promiscuous mode
4. **hal_ble / hal_ble_ota** -- NimBLE 2.x migration
5. **hal_audio** -- I2S rewrite (largest effort)
6. **hal_ntp** -- SNTP migration
7. **hal_sleep** -- ext1 wakeup rename
8. **hal_ota / hal_heartbeat** -- app_desc function rename
9. **Test everything** -- camera, SD, WiFi, touch, display

---

## References

- [ESP-IDF 5.0 Migration Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-5.x/5.0/index.html)
- [ESP-IDF 5.0 System Migration](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-5.x/5.0/system.html)
- [ESP-IDF 5.0 Peripherals Migration](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32/migration-guides/release-5.x/peripherals.html)
- [ESP-IDF 5.3 System Migration (sleep)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/migration-guides/release-5.x/5.3/system.html)
- [ESP-IDF 5.x Networking Migration (SNTP)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/migration-guides/release-5.x/5.0/networking.html)
- [ESP-IDF 5.x I2S Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/i2s.html)
- [ESP-IDF 5.x ESP-NOW Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html)
- [Arduino ESP32 2.x to 3.0 Migration Guide](https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html)
- [NimBLE-Arduino Migration Guide](https://github.com/h2zero/NimBLE-Arduino/blob/master/docs/Migration_guide.md)
- [NimBLE-Arduino ESP-IDF 5 Support Issue](https://github.com/h2zero/NimBLE-Arduino/issues/641)
- [pioarduino Releases](https://github.com/pioarduino/platform-espressif32/releases)
- [ESP32 PSRAM Cache Issue Discussion](https://esp32.com/viewtopic.php?t=39821)
- [esp32-camera GitHub](https://github.com/espressif/esp32-camera)
