# ESP32 Hardware

Multi-board ESP32-S3 display firmware targeting 6 Waveshare development boards. Supports AMOLED (QSPI), IPS LCD (QSPI), and IPS LCD (RGB parallel) display technologies through a unified app architecture.

## Supported Boards

| Environment | Board | Resolution | Display | Touch | Status |
|---|---|---|---|---|---|
| `touch-amoled-241b` | ESP32-S3-Touch-AMOLED-2.41-B | 600x450 | RM690B0 QSPI | FT6336 | Pin-verified |
| `amoled-191m` | ESP32-S3-AMOLED-1.91-M | 240x536 | RM67162 QSPI | FT3168 | Needs verification |
| `touch-amoled-18` | ESP32-S3-Touch-AMOLED-1.8 | 368x448 | SH8601Z QSPI | FT3168 | Needs verification |
| `touch-lcd-35bc` | ESP32-S3-Touch-LCD-3.5B-C | 320x480 | AXS15231B QSPI | Integrated | Placeholder driver |
| `touch-lcd-43c-box` | ESP32-S3-Touch-LCD-4.3C-BOX | 800x480 | ST7262 RGB | GT911 | Pin-verified |
| `touch-lcd-349` | ESP32-S3-Touch-LCD-3.49 | 172x640 | AXS15231B QSPI | Integrated | Placeholder driver |

All boards share: ESP32-S3 dual-core 240MHz, 16MB flash, 8MB PSRAM, WiFi, BLE 5, USB-C.

## Quick Start

### Prerequisites

Run the setup script (Ubuntu/Debian) to install PlatformIO, udev rules, and USB permissions:

```bash
./setup.sh
```

Or install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation.html) manually.

### Build and Flash

```bash
make build                          # Build default board (touch-amoled-241b)
make build BOARD=amoled-191m        # Build for a specific board
make flash BOARD=touch-amoled-241b  # Build and upload
make monitor                        # Serial monitor (115200 baud)
make flash-monitor BOARD=...        # Flash then monitor
```

Or use PlatformIO directly:

```bash
pio run -e touch-amoled-241b            # Build
pio run -e touch-amoled-241b -t upload  # Flash
pio device monitor -b 115200            # Monitor
```

### Other Commands

```bash
make list-boards       # Show all board environments
make list-apps         # Show available apps
make new-app NAME=foo  # Scaffold a new app from template
make clean             # Clean build artifacts
make format            # Run clang-format on all sources
```

## Project Structure

```
esp32-hardware/
  platformio.ini          Build config with per-board environments
  Makefile                Build automation wrapper
  setup.sh                Dev environment setup (Ubuntu/Debian)
  src/
    main.cpp              Thin launcher: init display, select app, run loop
  include/
    app.h                 Base App interface (setup/loop virtual methods)
    display_init.h        Board-specific LovyanGFX display class (LGFX)
    boards/               Per-board pin definition headers
  lib/
    StarField/            Reusable starfield simulation engine
  apps/
    starfield/            Starfield app (wraps StarField lib into App interface)
    _template/            Skeleton for creating new apps
  scripts/                Dev tools (flash.sh, monitor.sh, new-app.sh)
  docs/                   Documentation
```

## Architecture

Board selection and app selection are orthogonal -- any app can run on any board.

- **Board selection**: Compile-time via `-DBOARD_*` flag in each `platformio.ini` environment. `display_init.h` uses `#if defined()` to instantiate the correct LovyanGFX `LGFX` class for that board's display panel, bus, and pins.

- **App selection**: Compile-time via `-DAPP_*` flag. `src/main.cpp` uses `#if defined()` to instantiate the selected app. The default is starfield.

- **App interface**: All apps inherit from `App` (defined in `include/app.h`) with virtual `setup(LGFX&)` and `loop(LGFX&)` methods. Apps can draw directly with LovyanGFX (raw GFX) or layer LVGL on top for widget-based UIs.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for details.

## Adding a New App

```bash
make new-app NAME=myapp
```

Or see [docs/ADDING_AN_APP.md](docs/ADDING_AN_APP.md) for the manual process.

## Adding a New Board

See [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md).

## Documentation

- [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) -- From-scratch setup, first build, first flash, troubleshooting
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) -- System architecture and design decisions
- [docs/ADDING_AN_APP.md](docs/ADDING_AN_APP.md) -- How to create a new app
- [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md) -- How to add a new board
- [docs/boards.md](docs/boards.md) -- Detailed board specs and Waveshare links
