# ESP32 Hardware Project

Multi-board ESP32-S3 display firmware targeting 6 Waveshare development boards with different display technologies (AMOLED QSPI, LCD QSPI, RGB parallel).

## Directory Structure

```
esp32-hardware/
  platformio.ini      # Build config with per-board environments
  Makefile            # Build automation (make build/flash/monitor)
  setup.sh            # Dev environment setup script
  src/                # Main firmware source (main.cpp entry point)
  include/            # Headers
    display_init.h    # Board-specific LovyanGFX display initialization
    app.h             # Base App class interface
    boards/           # Per-board pin definitions (one .h per board)
  lib/                # Shared libraries (StarField, etc.)
  apps/               # App implementations (starfield, _template)
  scripts/            # Dev tools (flash.sh, monitor.sh, new-app.sh)
  docs/               # Project documentation
```

## Building and Flashing

```bash
make build                          # Build default board (touch-amoled-241b)
make build BOARD=amoled-191m        # Build for specific board
make flash BOARD=touch-amoled-241b  # Build + upload
make monitor                        # Serial monitor at 115200 baud
make flash-monitor BOARD=...        # Flash then monitor
make clean                          # Clean build artifacts
make list-boards                    # Show all board environments
make list-apps                      # Show available apps
make new-app NAME=myapp             # Scaffold new app from template
make format                         # Run clang-format on all sources
```

Or use PlatformIO directly: `pio run -e touch-amoled-241b`

## Supported Boards

| Environment | Board | Resolution | Display | Status |
|---|---|---|---|---|
| touch-amoled-241b | ESP32-S3-Touch-AMOLED-2.41-B | 600x450 | RM690B0 QSPI | Pin-verified |
| amoled-191m | ESP32-S3-AMOLED-1.91-M | 240x536 | RM67162 QSPI | Needs verification |
| touch-amoled-18 | ESP32-S3-Touch-AMOLED-1.8 | 368x448 | SH8601Z QSPI | Needs verification |
| touch-lcd-35bc | ESP32-S3-Touch-LCD-3.5B-C | 320x480 | AXS15231B QSPI | Placeholder driver |
| touch-lcd-43c-box | ESP32-S3-Touch-LCD-4.3C-BOX | 800x480 | ST7262 RGB | Pin-verified |
| touch-lcd-349 | ESP32-S3-Touch-LCD-3.49 | 172x640 | AXS15231B QSPI | Placeholder driver |

## Adding a New Board

1. Add pin definitions in `include/boards/esp32_s3_<name>.h`
2. Add an LGFX class block in `include/display_init.h` with `#elif defined(BOARD_<NAME>)`
3. Add a `[env:<name>]` section in `platformio.ini` with `-DBOARD_<NAME>` build flag
4. Update `Makefile` BOARDS list and `list-boards` target

## Adding a New App

Run `make new-app NAME=myapp` to scaffold, or manually:

1. Create `apps/<name>/<name>_app.h` implementing the `App` interface from `include/app.h`
2. Create `apps/<name>/<name>_app.cpp` with setup() and loop() implementations
3. The App base class provides `setup(LGFX& display)` and `loop(LGFX& display)`

## Architecture Decisions

- **LovyanGFX** over TFT_eSPI: Better QSPI support, cleaner multi-panel API, built-in sprite double-buffering, active maintenance for ESP32-S3.
- **Compile-time board selection**: Each board gets a PlatformIO environment with `-DBOARD_*` flag. `display_init.h` uses `#if defined()` to select the right LGFX class. No runtime overhead.
- **App pattern**: Apps inherit from `App` base class (`app.h`) with virtual `setup()`/`loop()`. Allows multi-app selector architecture.
- **PSRAM sprites**: All boards have PSRAM. Double-buffered rendering via `LGFX_Sprite` with `setPsram(true)` for flicker-free animation.

## Coding Conventions

- C++17, Arduino framework
- 4-space indentation, 100-column line width (see `.clang-format`)
- Google-based style with `Attach` brace style
- Board pin headers: `SCREAMING_SNAKE_CASE` for pin defines
- App classes: `PascalCase` + `App` suffix (e.g., `StarfieldApp`)
- File naming: `snake_case` for all source files
- Run `make format` before committing

## Common Pitfalls

- **AXS15231B boards (touch-lcd-35bc, touch-lcd-349)**: Use placeholder ST7789 driver. Display will NOT work until a custom `Panel_AXS15231B` is implemented.
- **ESP32-S3 native USB**: CDC-on-boot is enabled (`ARDUINO_USB_CDC_ON_BOOT=1`). Serial output goes through native USB, not UART pins.
- **PSRAM cache issue**: Build flag `-mfix-esp32-psram-cache-issue` is required for stable PSRAM access on ESP32-S3.
- **RGB parallel panel (43C-BOX)**: Uses `Bus_RGB` + `Panel_RGB` instead of `Bus_SPI`. Different initialization pattern from QSPI boards.
- **RM690B0 memory offset**: The 2.41-B panel has 452px memory width but 450px visible. Requires `offset_x = 16`.
