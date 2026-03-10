# Hardware Abstraction Requirements

Tritium-Edge device management for heterogeneous edge hardware.

## 1. Overview

Tritium-Edge manages fleets of heterogeneous edge devices. The ESP32-S3 is the
first supported hardware family, but the architecture accommodates any MCU or
single-board computer that can run the heartbeat client. Peripheral drivers for
sensors, displays, and codecs are written once and shared across families.

Hardware abstraction is organized into three layers:

```
+-------------------------------------------------------------+
|                     Application / Apps                       |
+-------------------------------------------------------------+
|                   High-Level HALs                            |
|         (hal_heartbeat, hal_config, hal_ota, ...)            |
+-------------------------------------------------------------+
|  Layer 3: Peripheral Drivers       |  Layer 2: BSP          |
|  (qmi8658, es8311, axp2101, ...)   |  (pin maps, inventory) |
+-------------------------------------------------------------+
|          Layer 1: Platform Abstraction Layer (PAL)           |
|     (I2C, SPI, GPIO, networking, storage, OTA, system)      |
+-------------------------------------------------------------+
|         Hardware / Silicon / OS                              |
|     (ESP-IDF, Zephyr, Linux sysfs, Pico SDK, ...)           |
+-------------------------------------------------------------+
```

**Layer 1 -- Platform Abstraction Layer (PAL).**
Chip-specific implementations of I2C, SPI, GPIO, networking, storage, and OTA.
Each hardware family provides one PAL implementation. All higher layers depend
only on PAL headers -- never on framework-specific APIs.

**Layer 2 -- Board Support Package (BSP).**
Pin maps, peripheral inventory, I2C bus assignments, and capability lists.
One BSP header per physical board.

**Layer 3 -- Peripheral Drivers.**
Sensor, display, codec, and power management drivers written in
platform-agnostic C. They call PAL functions exclusively.

---

## 2. Platform Abstraction Layer (PAL)

The PAL is a thin C interface that each hardware family implements. Every
function returns 0 on success and a negative error code on failure unless
otherwise noted. Opaque handle types hide platform internals.

### 2.1 PAL Directory Layout

```
pal/
  pal_i2c.h
  pal_spi.h
  pal_gpio.h
  pal_net.h
  pal_storage.h
  pal_ota.h
  pal_system.h

platforms/
  esp32/
    pal_i2c.c
    pal_spi.c
    pal_gpio.c
    pal_net.c
    pal_storage.c
    pal_ota.c
    pal_system.c
  stm32/
    ...
  nrf52/
    ...
  rp2040/
    ...
  linux/
    ...
```

### 2.2 pal_i2c.h

```c
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct pal_i2c_bus pal_i2c_bus_t;

pal_i2c_bus_t* pal_i2c_init(int bus_num, int sda_pin, int scl_pin,
                             uint32_t freq_hz);
void           pal_i2c_deinit(pal_i2c_bus_t* bus);

int pal_i2c_write_reg(pal_i2c_bus_t* bus, uint8_t addr, uint8_t reg,
                       const uint8_t* data, size_t len);
int pal_i2c_read_reg(pal_i2c_bus_t* bus, uint8_t addr, uint8_t reg,
                      uint8_t* data, size_t len);

int pal_i2c_write(pal_i2c_bus_t* bus, uint8_t addr,
                   const uint8_t* data, size_t len);
int pal_i2c_read(pal_i2c_bus_t* bus, uint8_t addr,
                  uint8_t* data, size_t len);
```

Notes:
- `bus_num` maps to a hardware peripheral (e.g., I2C_NUM_0 on ESP32,
  `/dev/i2c-1` on Linux).
- `sda_pin` and `scl_pin` are ignored on platforms that do not support
  runtime pin remapping (Linux SBCs use kernel-configured pins).
- The bus handle is reference-counted internally. Multiple calls to
  `pal_i2c_init` with the same `bus_num` return the same handle.

### 2.3 pal_spi.h

```c
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct pal_spi_bus pal_spi_bus_t;

pal_spi_bus_t* pal_spi_init(int bus_num, int mosi, int miso, int sclk,
                             uint32_t freq_hz);
void           pal_spi_deinit(pal_spi_bus_t* bus);

int pal_spi_transfer(pal_spi_bus_t* bus, int cs_pin,
                      const uint8_t* tx, uint8_t* rx, size_t len);
```

### 2.4 pal_gpio.h

```c
#pragma once
#include <stdint.h>

typedef enum {
    PAL_GPIO_INPUT,
    PAL_GPIO_OUTPUT,
    PAL_GPIO_INPUT_PULLUP,
    PAL_GPIO_INPUT_PULLDOWN,
    PAL_GPIO_PWM
} pal_gpio_mode_t;

void pal_gpio_init(int pin, pal_gpio_mode_t mode);
int  pal_gpio_read(int pin);
void pal_gpio_write(int pin, int value);
void pal_gpio_pwm(int pin, uint32_t freq_hz, uint8_t duty_percent);
```

### 2.5 pal_net.h

```c
#pragma once
#include <stddef.h>

int pal_http_post(const char* url, const char* body,
                  char* response, size_t max_len, int timeout_ms);
int pal_http_get(const char* url,
                 char* response, size_t max_len, int timeout_ms);
int pal_wifi_connected(void);
```

On Linux SBCs the network stack is managed by the OS. `pal_wifi_connected`
returns 1 when any interface has a routable address.

### 2.6 pal_storage.h

```c
#pragma once
#include <stddef.h>

int pal_nvs_init(void);
int pal_nvs_set_blob(const char* ns, const char* key,
                     const void* data, size_t len);
int pal_nvs_get_blob(const char* ns, const char* key,
                     void* data, size_t* len);
int pal_nvs_erase(const char* ns, const char* key);
```

On MCUs this maps to flash-backed key-value stores (NVS on ESP32, Settings
subsystem on Zephyr). On Linux it maps to files under
`/var/lib/tritium/<ns>/<key>`.

### 2.7 pal_ota.h

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

int  pal_ota_begin(size_t image_size);
int  pal_ota_write(const uint8_t* data, size_t len);
int  pal_ota_end(bool verify);
int  pal_ota_rollback(void);
int  pal_ota_confirm(void);
void pal_ota_get_fw_hash(uint8_t hash[32]);
```

The OTA flow is: `begin` -> repeated `write` -> `end` -> reboot -> `confirm`.
If `confirm` is not called within the heartbeat watchdog window, the device
rolls back automatically.

### 2.8 pal_system.h

```c
#pragma once
#include <stdint.h>

void        pal_reboot(void);
uint32_t    pal_uptime_ms(void);
uint32_t    pal_free_heap(void);
float       pal_temperature(void);    /* chip temperature, or -1 if unavailable */
void        pal_get_mac(uint8_t mac[6]);
const char* pal_board_name(void);
const char* pal_family_name(void);    /* "esp32", "stm32", "nrf52", "linux" */
```

### 2.9 Platform Implementations

| Family   | Framework          | PAL Location      | Build System       |
|----------|--------------------|-------------------|--------------------|
| ESP32-S3 | ESP-IDF 5.5.2      | platforms/esp32/  | PlatformIO         |
| STM32    | Zephyr / STM32 HAL | platforms/stm32/  | PlatformIO or CMake|
| nRF52    | Zephyr / nRF SDK   | platforms/nrf52/  | CMake              |
| RP2040   | Pico SDK           | platforms/rp2040/ | CMake              |
| Linux SBC| sysfs / i2c-dev    | platforms/linux/  | CMake              |

Each platform directory contains one `.c` file per PAL interface. The build
system links the correct platform at compile time via include path and source
filter configuration.

---

## 3. Peripheral Drivers

Shared C drivers that depend only on PAL interfaces. Written once, compiled for
any platform that provides a PAL implementation.

### 3.1 Driver Structure

```c
/* drivers/sensors/qmi8658.h */
#pragma once
#include "pal_i2c.h"

typedef struct {
    pal_i2c_bus_t* bus;
    uint8_t        addr;
    float          accel[3];   /* last reading: x, y, z in g          */
    float          gyro[3];    /* last reading: x, y, z in deg/s      */
} qmi8658_t;

int  qmi8658_init(qmi8658_t* dev, pal_i2c_bus_t* bus, uint8_t addr);
int  qmi8658_read(qmi8658_t* dev);       /* updates accel[] and gyro[] */
int  qmi8658_set_rate(qmi8658_t* dev, int hz);
void qmi8658_deinit(qmi8658_t* dev);
```

Every driver follows this pattern:
- An opaque-ish struct holding the bus handle, address, and cached state.
- `init` / `deinit` lifecycle functions.
- `read` or `poll` to update cached values.
- Configuration functions as needed.
- No heap allocation. The caller owns the struct.

### 3.2 Driver Directory Layout

```
drivers/
  sensors/
    qmi8658.h / qmi8658.c
    bme280.h  / bme280.c
    sht4x.h   / sht4x.c
  power/
    axp2101.h / axp2101.c
  display/
    axs15231b.h / axs15231b.c
    rm690b0.h   / rm690b0.c
    sh8601z.h   / sh8601z.c
  codec/
    es8311.h / es8311.c
  camera/
    ov5640.h / ov5640.c
  rtc/
    pcf85063.h / pcf85063.c
  io/
    tca9554.h / tca9554.c
```

### 3.3 Current Driver Inventory

| Category | Driver     | Chip       | Interface  | Used On              |
|----------|-----------|------------|------------|----------------------|
| Sensor   | qmi8658   | QMI8658    | I2C        | ESP32 3.5B-C         |
| Sensor   | bme280    | BME280     | I2C / SPI  | Future (STM32)       |
| Sensor   | sht4x     | SHT40/41/45| I2C       | Future               |
| Power    | axp2101   | AXP2101    | I2C        | ESP32 3.5B-C         |
| Display  | axs15231b | AXS15231B  | QSPI + I2C | ESP32 3.5B-C, 3.49  |
| Display  | rm690b0   | RM690B0    | QSPI       | ESP32 2.41-B         |
| Display  | sh8601z   | SH8601Z    | QSPI       | ESP32 1.8            |
| Codec    | es8311    | ES8311     | I2C + I2S  | ESP32 3.5B-C         |
| Camera   | ov5640    | OV5640     | DVP + I2C  | ESP32 3.5B-C         |
| RTC      | pcf85063  | PCF85063   | I2C        | ESP32 3.5B-C         |
| I/O      | tca9554   | TCA9554    | I2C        | ESP32 3.5B-C         |

### 3.4 Adding a New Driver

1. Create `drivers/{category}/{chip}.h` with PAL-only includes.
2. Create `drivers/{category}/{chip}.c` with the implementation.
3. Add the device to the target board's BSP peripheral inventory.
4. Register the module name in the heartbeat capabilities list.
5. Write a minimal test that exercises `init`, `read`, and `deinit`
   against a mock PAL (see `tests/mock_pal/`).

---

## 4. Board Support Package (BSP)

Each physical board has a BSP header that declares pin mappings, peripheral
addresses, bus assignments, and a capabilities list for heartbeat reporting.

### 4.1 BSP Header Format

```c
/* boards/esp32/touch-lcd-35bc.h */
#pragma once

#define BOARD_NAME      "touch-lcd-35bc"
#define BOARD_FAMILY    "esp32"

/* ---- I2C Bus 0 ---- */
/* Shared: touch, PMIC, IMU, RTC, codec, I/O expander */
#define I2C0_SDA        8
#define I2C0_SCL        7
#define I2C0_FREQ       400000

/* ---- Peripheral presence and addresses ---- */
#define HAS_IMU         1
#define IMU_ADDR        0x6B
#define IMU_BUS         0

#define HAS_POWER       1
#define POWER_ADDR      0x34
#define POWER_BUS       0

#define HAS_RTC         1
#define RTC_ADDR        0x51
#define RTC_BUS         0

#define HAS_CAMERA      1
#define HAS_AUDIO       1
#define HAS_DISPLAY     1
#define HAS_TOUCH       1
#define HAS_IO_EXPANDER 1

/* ---- Capability list (reported in heartbeat) ---- */
#define BOARD_CAPABILITIES \
    "camera", "audio", "imu", "display", "touch", "rtc", "power", "mesh"
```

### 4.2 BSP Rules

- One header per board. File name matches the PlatformIO environment name.
- Only `#define` statements. No function declarations, no includes beyond
  `#pragma once`.
- Pin names use `SCREAMING_SNAKE_CASE`.
- `HAS_*` macros are either defined to 1 or not defined at all.
  Never define `HAS_FOO 0`.
- `BOARD_CAPABILITIES` is a comma-separated string list consumed by the
  heartbeat module at compile time.

### 4.3 BSP Inventory

| BSP Header             | Family | Board               | Peripherals                         |
|------------------------|--------|---------------------|-------------------------------------|
| touch-lcd-35bc.h       | esp32  | Touch-LCD-3.5B-C    | IMU, PMIC, RTC, camera, codec, I/O  |
| touch-lcd-349.h        | esp32  | Touch-LCD-3.49      | Display, touch                       |
| touch-amoled-241b.h    | esp32  | Touch-AMOLED-2.41-B | Display, touch                       |
| amoled-191m.h          | esp32  | AMOLED-1.91-M       | Display                              |
| touch-amoled-18.h      | esp32  | Touch-AMOLED-1.8    | Display, touch                       |
| touch-lcd-43c-box.h    | esp32  | Touch-LCD-4.3C-BOX  | Display, touch                       |

---

## 5. High-Level HALs

High-level HALs sit above drivers and PAL. They provide application-facing APIs
and may be platform-specific where the underlying technology differs too much
across families.

| HAL            | Purpose                          | Depends On                 |
|----------------|----------------------------------|----------------------------|
| hal_heartbeat  | Heartbeat client, config sync    | pal_net, pal_storage       |
| hal_config     | Config manager, module enable    | pal_storage, all drivers   |
| hal_ota        | OTA orchestration, verification  | pal_ota, pal_net           |
| hal_wifi       | WiFi management                  | Platform-specific (ESP32)  |
| hal_mesh       | ESP-NOW mesh networking          | Platform-specific (ESP32)  |
| hal_display    | Display abstraction              | Display drivers, PAL       |
| hal_audio      | Audio capture and playback       | Codec drivers, PAL         |
| hal_power      | Battery and power management     | Power drivers, PAL         |

Platform-specific HALs are acceptable. WiFi and BLE behave differently enough
across families that a shared abstraction would be leaky and wasteful. The
critical shared components are the I2C/SPI peripheral drivers and the heartbeat
protocol.

---

## 6. Migration Path

### Phase 1: Current State (ESP32 Only)

- Existing C++ HALs in `lib/hal_*` continue to work unchanged.
- ESP-IDF 5.5.2 acts as the platform layer.
- No refactoring is required at this stage.

### Phase 2: Extract PAL Interfaces (Triggered by Second Family)

- Define `pal/*.h` header files with the interfaces documented above.
- Create `platforms/esp32/` that wraps existing ESP-IDF calls.
- Gradually migrate sensor-specific code from `lib/hal_*` into
  `drivers/{category}/` using PAL calls.
- Existing apps and high-level HALs remain untouched.

### Phase 3: Second Hardware Family

- Implement `platforms/stm32/` (or whichever family comes next).
- Reuse `drivers/` for any shared peripherals.
- New board BSP declares which drivers to initialize.
- Heartbeat protocol works identically -- the server does not care about
  the family.

### Phase 4: Linux SBC Support

- Implement `platforms/linux/` using `i2c-dev`, `spidev`, sysfs GPIO,
  `libcurl` for HTTP, and filesystem-backed NVS.
- OTA maps to package-manager or container image updates.
- Same heartbeat protocol, same driver code for any I2C/SPI peripherals
  connected to the SBC's expansion header.

The guiding principle: **do not refactor prematurely.** The PAL extraction
happens when the second family is actually being brought up, not before. The
interfaces are designed now so the architecture is ready, but the code stays
simple until it needs to grow.

---

## 7. Server-Side: Capability-Aware Management

The management server uses device capabilities reported in heartbeats to drive
the admin UI, validate configurations, and filter firmware.

### 7.1 Heartbeat Capability Report

Every heartbeat includes hardware identity:

```json
{
    "family": "esp32",
    "board": "touch-lcd-35bc",
    "capabilities": [
        "camera", "audio", "imu", "display",
        "touch", "rtc", "power", "mesh"
    ]
}
```

### 7.2 Product Profile Compatibility

Product profiles declare what hardware they require:

```json
{
    "name": "Security Camera Node",
    "required_capabilities": ["camera", "wifi"],
    "compatible_families": ["esp32", "linux"],
    "default_config": {
        "camera_resolution": "vga",
        "stream_interval_ms": 1000
    }
}
```

### 7.3 Server Behavior

| Action                    | How Capabilities Are Used                              |
|---------------------------|--------------------------------------------------------|
| Admin UI config panel     | Only show settings for peripherals the device has      |
| Profile assignment        | Reject if device lacks required capabilities           |
| Firmware filtering        | ESP32 firmware is not offered to STM32 devices         |
| Fleet grouping            | Devices grouped by family and board in dashboard views  |
| Alert routing             | Power alerts only for devices with `power` capability  |

---

## 8. Design Principles

1. **Drivers depend only on PAL.** No `#include <Wire.h>`, no
   `#include <zephyr/drivers/i2c.h>`. Only `#include "pal_i2c.h"`.

2. **BSP declares peripherals.** It does not implement protocol logic or
   initialize buses. It is a static manifest of what exists on the board.

3. **PAL is thin.** It wraps platform primitives with minimal logic. No
   retry policies, no caching, no business rules.

4. **Do not abstract prematurely.** Extract PAL when the second hardware
   family arrives. Until then, the existing ESP32 Arduino code is fine.

5. **Server treats all devices the same.** The heartbeat protocol is
   family-agnostic. An ESP32 and a Jetson Nano look identical to the
   server except for their capability lists.

6. **Capabilities drive the UI.** The admin portal adapts dynamically to
   what each device can do. No hardcoded board-specific pages.

7. **Caller owns memory.** Driver structs are allocated by the caller
   (typically on the stack or in static storage). Drivers never call
   `malloc`.

8. **Errors are integers.** All PAL and driver functions return 0 on
   success, negative `errno`-style codes on failure. No exceptions, no
   error strings in the hot path.
