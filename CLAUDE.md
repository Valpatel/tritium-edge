# Tritium-Edge — Software Defined IoT

Multi-family edge device management platform. ESP32-S3 Waveshare boards are the first hardware family (6 boards: AMOLED QSPI, LCD QSPI, RGB parallel). Management server handles fleet provisioning, OTA, remote config, and monitoring.

## Git Conventions

- **No co-authors on commits** — never add "Co-Authored-By" lines
- Remote: `git@github.com:Valpatel/tritium-edge.git`
- Copyright: Created by Matthew Valancy / Copyright 2026 Valpatel Software LLC / AGPL-3.0

## Directory Structure

```
esp32-hardware/
  platformio.ini      # Build config with per-board environments
  Makefile            # Build automation (make build/flash/monitor)
  src/                # Main firmware source (main.cpp entry point)
  include/            # Headers
    app.h             # Base App class interface
    boards/           # Per-board pin definitions (one .h per board)
  lib/                # Shared libraries and HALs
    display/          # esp_lcd display HAL
      display.h       # display_init(), display_get_panel(), display_push_fb()
      display.cpp     # Board-agnostic esp_lcd init + DMA chunked push
      backlight.h     # Backlight control (PWM + active-LOW support)
      drivers/        # Per-panel esp_lcd drivers (AXS15231B, RM690B0, etc.)
      boards/         # Per-board display configs (pins, timing, init_cmds)
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
make build BOARD=touch-lcd-349               # Build starfield for board
make build BOARD=touch-lcd-35bc APP=camera   # Build camera app
make flash BOARD=touch-lcd-349               # Build + upload (auto-detects port + fixes perms)
make flash BOARD=touch-lcd-35bc APP=camera   # Flash camera app
make monitor                                 # Serial monitor
make flash-monitor BOARD=touch-lcd-349       # Flash then monitor

# Discovery
make list-boards                             # Show all boards
make list-apps                               # Show all apps
make identify                                # Detect connected boards via USB

# Utilities
make clean                                   # Clean build artifacts
make new-app NAME=myapp                      # Scaffold new app from template
make format                                  # clang-format all sources
make sim BOARD=touch-lcd-35bc                # Desktop simulator
```

Or use PlatformIO directly: `pio run -e touch-lcd-349-starfield`

## Supported Boards

| Environment | Board | Resolution | Display | Status |
|---|---|---|---|---|
| touch-amoled-241b | ESP32-S3-Touch-AMOLED-2.41-B | 450x600 | RM690B0 QSPI | HW Verified |
| amoled-191m | ESP32-S3-AMOLED-1.91-M | 240x536 | RM67162 QSPI | Needs verification |
| touch-amoled-18 | ESP32-S3-Touch-AMOLED-1.8 | 368x448 | SH8601Z QSPI | Needs verification |
| touch-lcd-35bc | ESP32-S3-Touch-LCD-3.5B-C | 320x480 | AXS15231B QSPI | HW Verified (display+camera+audio) |
| touch-lcd-43c-box | ESP32-S3-Touch-LCD-4.3C-BOX | 800x480 | ST7262 RGB | Pin-verified |
| touch-lcd-349 | ESP32-S3-Touch-LCD-3.49 | 172x640 | AXS15231B QSPI | HW Verified (esp_lcd) |

## Adding a New Board

1. Add pin definitions in `include/boards/esp32_s3_<name>.h`
2. Add a board config in `lib/display/boards/` with panel-specific pins, timing, and init_cmds
3. Add a `[env:<name>]` section in `platformio.ini` with `-DBOARD_<NAME>` build flag
4. Find the official Waveshare demo code and extract the display init sequence
5. Update `Makefile` BOARDS list

## Adding a New App

Run `make new-app NAME=myapp` to scaffold, or manually:

1. Create `apps/<name>/<name>_app.h` implementing the `App` interface from `include/app.h`
2. Create `apps/<name>/<name>_app.cpp` with `setup(esp_lcd_panel_handle_t panel, int w, int h)` and `loop()`
3. Add `#elif defined(APP_<NAME>)` block in `src/main.cpp`
4. Add `[app_<name>]` section in `platformio.ini` with build flags and src filter
5. Add board-specific env: `[env:board-appname]` referencing the app section

## Architecture Decisions

- **esp_lcd over LovyanGFX**: Native ESP-IDF driver, DMA completion semaphore, no abstraction overhead, proven 55+ FPS on 3.49. Display path: `display_init()` -> `display_get_panel()` -> `esp_lcd_panel_draw_bitmap()`.
- **Compile-time board selection**: Each board gets a PlatformIO environment with `-DBOARD_*` flag. Board configs in `lib/display/boards/` use `#if defined()` to select the right driver. No runtime overhead.
- **App pattern**: Apps inherit from `App` base class with virtual `setup(esp_lcd_panel_handle_t panel, int w, int h)` / `loop()`. Selected via `-DAPP_*` build flag.
- **PSRAM framebuffer + DMA buffer**: All boards have 8MB PSRAM. Framebuffer allocated in PSRAM (`MALLOC_CAP_SPIRAM`), DMA transfer buffer in internal SRAM (`MALLOC_CAP_DMA`). Chunked push copies framebuffer slices to DMA buffer for SPI transfer.
- **HAL dual-mode I2C**: Peripherals on shared I2C bus support both Arduino Wire and lgfx::i2c backends.

## Common Pitfalls

- **RGB565 byte-swap for QSPI**: All QSPI panels need byte-swapped pixels for SPI transport: `(c >> 8) | (c << 8)`. Without this, colors are wrong.
- **DMA buffer must be in internal SRAM**: `heap_caps_malloc(size, MALLOC_CAP_DMA)` for the DMA transfer buffer. Framebuffer goes in PSRAM (`MALLOC_CAP_SPIRAM`). DMA from PSRAM directly will fail or corrupt.
- **SPI host enum values changed in ESP-IDF 5.x**: SPI2_HOST=1, SPI3_HOST=2 (was 2, 3 in ESP-IDF 4.x). Always use enum names (`SPI3_HOST`), never raw integers. Using `3` silently picks the wrong peripheral.
- **AXS15231B on 3.49**: `vendor_specific_init_default` works fine -- just send SleepOut (0x11) + DispOn (0x29). No need for the full register init sequence. ROM defaults handle the rest.
- **3.5B-C TCA9554 display reset**: Display reset is routed through I/O expander (I2C 0x20, pin 1). Must call `tca9554_reset_display()` before panel init. Without this, the display stays black.
- **3.5B-C backlight**: Needs manual `pinMode(LCD_BL, OUTPUT); digitalWrite(LCD_BL, HIGH)` as fallback.
- **Camera LEDC timer conflict**: Camera (esp_camera) claims LEDC_TIMER_0 + LEDC_CHANNEL_0 for XCLK (20MHz). Display backlight PWM should use ch4+ (TIMER2/TIMER3) when camera is active.
- **Camera orientation**: OV5640 on 3.5B-C needs `setFlip(false, true)` for correct orientation.
- **Power cycle vs soft reset**: AXS15231B retains state across ESP32 soft resets (RTS). A bad init can persist until USB power is physically unplugged. Always power cycle between test iterations.
- **Serial port permissions**: The Makefile auto-fixes `/dev/ttyACM0` permissions. If flashing manually, run `sudo chmod 666 /dev/ttyACM0` first, or add user to `dialout` group.
- **ESP32-S3 native USB**: CDC-on-boot is enabled. Serial goes through native USB, not UART.
- **RGB parallel panel (43C-BOX)**: Uses esp_lcd RGB panel driver, different config from QSPI boards.
- **RM690B0 memory offset**: The 2.41-B panel has 452px memory width but 450px visible. Requires `offset_x = 16`.
- **Reference code**: Official Waveshare demos are in `references/`. Repos are under `waveshareteam` GitHub org (not `waveshare`). Always check these when debugging display issues.

## Coding Conventions

- C++17, Arduino framework
- 4-space indentation, 100-column line width
- Board pin headers: `SCREAMING_SNAKE_CASE` for pin defines
- App classes: `PascalCase` + `App` suffix (e.g., `CameraApp`)
- File naming: `snake_case` for all source files

## Flashing and Monitoring

Always use the standard scripts -- never use manual pio/chmod commands:

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
