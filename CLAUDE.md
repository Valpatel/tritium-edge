# Tritium-Edge — Software Defined IoT

Multi-family edge device management platform. ESP32-S3 Waveshare boards are the first hardware family (6 boards: AMOLED QSPI, LCD QSPI, RGB parallel). Management server handles fleet provisioning, OTA, remote config, and monitoring.

## Git Conventions

- **No co-authors on commits** — never add "Co-Authored-By" lines
- Remote: `git@github.com:Valpatel/tritium-edge.git`
- Copyright: Created by Matthew Valancy / Copyright 2026 Valpatel Software LLC / AGPL-3.0

## Directory Structure

```
tritium-edge/
  platformio.ini      # Build config with per-board environments
  Makefile            # Build automation (make build/flash/monitor)
  src/                # Main firmware source (main.cpp entry point)
  include/            # Headers
    display_init.h    # Board-specific LovyanGFX display initialization
    app.h             # Base App class interface
    boards/           # Per-board pin definitions (one .h per board)
  lib/                # Shared libraries and HALs
    Panel_AXS15231B/  # Custom LovyanGFX panel driver for AXS15231B QSPI LCD
    hal_camera/       # OV5640 DVP camera via esp_camera
    hal_audio/        # ES8311 codec + I2S
    hal_imu/          # QMI8658 IMU
    hal_rtc/          # PCF85063 RTC
    hal_power/        # AXP2101 PMIC
    hal_wifi/         # Multi-network WiFi with NVS
    ...               # See lib/ for full list
  apps/               # App implementations
    starfield/        # Default demo app
    camera/           # Camera preview (3.5B-C only)
    system/           # Full hardware dashboard
    ui_demo/          # LVGL UI demo
    wifi_setup/       # WiFi configuration
  scripts/            # Dev tools (flash.sh, monitor.sh, new-app.sh)
  tools/              # Utilities (detect_boards.py)
  references/         # Official Waveshare demo code for all boards
```

## Building and Flashing

```bash
# Basic workflow
make build BOARD=touch-lcd-35bc             # Build starfield for board
make build BOARD=touch-lcd-35bc APP=camera  # Build camera app
make flash BOARD=touch-lcd-35bc             # Build + upload (auto-detects port + fixes perms)
make flash BOARD=touch-lcd-35bc APP=camera  # Flash camera app
make monitor                                # Serial monitor
make flash-monitor BOARD=touch-lcd-35bc     # Flash then monitor

# Discovery
make list-boards                            # Show all boards
make list-apps                              # Show all apps
make identify                               # Detect connected boards via USB

# Utilities
make clean                                  # Clean build artifacts
make new-app NAME=myapp                     # Scaffold new app from template
make format                                 # clang-format all sources
make sim BOARD=touch-lcd-35bc               # Desktop simulator
```

Or use PlatformIO directly: `pio run -e touch-lcd-35bc-camera`

## Supported Boards

| Environment | Board | Resolution | Display | Status |
|---|---|---|---|---|
| touch-amoled-241b | ESP32-S3-Touch-AMOLED-2.41-B | 450x600 | RM690B0 QSPI | HW Verified |
| amoled-191m | ESP32-S3-AMOLED-1.91-M | 240x536 | RM67162 QSPI | Needs verification |
| touch-amoled-18 | ESP32-S3-Touch-AMOLED-1.8 | 368x448 | SH8601Z QSPI | Needs verification |
| touch-lcd-35bc | ESP32-S3-Touch-LCD-3.5B-C | 320x480 | AXS15231B QSPI | HW Verified (display+camera+audio) |
| touch-lcd-43c-box | ESP32-S3-Touch-LCD-4.3C-BOX | 800x480 | ST7262 RGB | Pin-verified |
| touch-lcd-349 | ESP32-S3-Touch-LCD-3.49 | 172x640 | AXS15231B QSPI | HW Verified |

## Adding a New Board

1. Add pin definitions in `include/boards/esp32_s3_<name>.h`
2. Add an LGFX class block in `include/display_init.h` with `#elif defined(BOARD_<NAME>)`
3. Add a `[env:<name>]` section in `platformio.ini` with `-DBOARD_<NAME>` build flag
4. Find the official Waveshare demo code and extract the display init sequence
5. Update `Makefile` BOARDS list

## Adding a New App

Run `make new-app NAME=myapp` to scaffold, or manually:

1. Create `apps/<name>/<name>_app.h` implementing the `App` interface from `include/app.h`
2. Create `apps/<name>/<name>_app.cpp` with setup() and loop()
3. Add `#elif defined(APP_<NAME>)` block in `src/main.cpp`
4. Add `[app_<name>]` section in `platformio.ini` with build flags and src filter
5. Add board-specific env: `[env:board-appname]` referencing the app section

## Architecture Decisions

- **LovyanGFX** over TFT_eSPI: Better QSPI support, cleaner multi-panel API, built-in sprite double-buffering, active maintenance for ESP32-S3.
- **Compile-time board selection**: Each board gets a PlatformIO environment with `-DBOARD_*` flag. `display_init.h` uses `#if defined()` to select the right LGFX class. No runtime overhead.
- **App pattern**: Apps inherit from `App` base class with virtual `setup()`/`loop()`. Selected via `-DAPP_*` build flag.
- **PSRAM sprites**: All boards have 8MB PSRAM. Use `LGFX_Sprite` with `setPsram(true)` for double-buffered rendering.
- **HAL dual-mode I2C**: Peripherals on shared I2C bus support both Arduino Wire and LovyanGFX lgfx::i2c backends.

## Common Pitfalls

- **AXS15231B display init**: Needs full register init sequence (~500 bytes), not just Sleep Out + Display On. See `Panel_AXS15231B.hpp` for the complete sequences converted from official Waveshare Arduino_GFX demos.
- **3.5B-C TCA9554 display reset**: Display reset is routed through I/O expander (I2C 0x20, pin 1). Must call `tca9554_reset_display()` before `display.init()`. Without this, the display stays black.
- **3.5B-C backlight**: Needs manual `pinMode(LCD_BL, OUTPUT); digitalWrite(LCD_BL, HIGH)` as fallback — LovyanGFX Light_PWM alone may not suffice.
- **Camera orientation**: OV5640 on 3.5B-C needs `setFlip(false, true)` for correct orientation.
- **QSPI pixel push**: `pushImage()` works reliably on AXS15231B. `setAddrWindow()` + `pushPixels()` may not render on QSPI panels — use sprite `pushSprite()` instead.
- **Serial port permissions**: The Makefile auto-fixes `/dev/ttyACM0` permissions. If flashing manually, run `sudo chmod 666 /dev/ttyACM0` first, or add user to `dialout` group.
- **ESP32-S3 native USB**: CDC-on-boot is enabled. Serial goes through native USB, not UART.
- **PSRAM cache**: `-mfix-esp32-psram-cache-issue` build flag is required for stable PSRAM access.
- **RGB parallel panel (43C-BOX)**: Uses `Bus_RGB` + `Panel_RGB`, different from QSPI boards.
- **RM690B0 memory offset**: The 2.41-B panel has 452px memory width but 450px visible. Requires `offset_x = 16`.
- **Reference code**: Official Waveshare demos are in `references/`. Repos are under `waveshareteam` GitHub org (not `waveshare`). Always check these when debugging display issues.

## Coding Conventions

- C++17, Arduino framework
- 4-space indentation, 100-column line width
- Board pin headers: `SCREAMING_SNAKE_CASE` for pin defines
- App classes: `PascalCase` + `App` suffix (e.g., `CameraApp`)
- File naming: `snake_case` for all source files

## Flashing and Monitoring

Always use the standard scripts — never use manual pio/chmod commands:

```bash
# Flash firmware to a connected board
./scripts/flash.sh BOARD [APP]
# Examples:
./scripts/flash.sh touch-lcd-349              # Flash starfield (default)
./scripts/flash.sh touch-lcd-35bc camera      # Flash camera app
./scripts/flash.sh touch-amoled-241b system   # Flash system dashboard

# Serial monitor (auto-detects port)
./scripts/monitor.sh

# Flash then monitor
./scripts/flash.sh BOARD [APP] && ./scripts/monitor.sh
```

The scripts auto-detect the serial port and fix permissions as needed.
