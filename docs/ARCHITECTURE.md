# Architecture

## Overview

The firmware is structured around two orthogonal axes:

- **Boards** -- hardware-specific display initialization (pins, bus, panel driver)
- **Apps** -- portable application logic that draws to an abstract display

Both are selected at compile time via preprocessor flags. Any app can run on any board without modification.

## Layer Diagram

```
+---------------------------+
|        App Layer          |  apps/starfield/, apps/my_app/, ...
|  (App interface: setup/loop)
+---------------------------+
|     App Base Class        |  include/app.h
+---------------------------+
|    Display Abstraction    |  include/display_init.h (board-specific LGFX class)
+---------------------------+
|       LovyanGFX           |  lib_deps (LGFX_Device, LGFX_Sprite, Bus_*, Panel_*)
+---------------------------+
|     Board Pin Defs        |  include/boards/*.h
+---------------------------+
|      ESP32-S3 HW          |  SPI, I2C, RGB parallel, PSRAM, GPIO
+---------------------------+
```

## Board Abstraction

Each board has:

1. **Pin header** (`include/boards/esp32_s3_<name>.h`) -- `#define` constants for every GPIO: display bus pins, touch I2C, IMU, RTC, etc.

2. **LGFX class** (in `include/display_init.h`) -- a board-specific `class LGFX : public lgfx::LGFX_Device` that configures the correct bus type (`Bus_SPI` for QSPI panels, `Bus_RGB` for RGB parallel) and panel driver (`Panel_RM690B0`, `Panel_RM67162`, etc.) using the pin defines.

3. **PlatformIO environment** (`platformio.ini`) -- sets `-DBOARD_<NAME>`, `-DDISPLAY_WIDTH`, `-DDISPLAY_HEIGHT`, and board-specific build settings.

The `#if defined(BOARD_*)` chain in `display_init.h` selects exactly one LGFX class at compile time. The resulting `LGFX` type is what apps receive.

### Display Technologies

| Bus Type | Boards | LovyanGFX Classes |
|---|---|---|
| QSPI (SPI with 4 data lines) | 2.41-B, 1.91-M, 1.8, 3.5B-C, 3.49 | `Bus_SPI` + `Panel_*` |
| RGB parallel (16-bit) | 4.3C-BOX | `Bus_RGB` + `Panel_RGB` |

## App Architecture

### App Interface

```cpp
// include/app.h
class App {
public:
    virtual ~App() = default;
    virtual const char* name() = 0;
    virtual void setup(LGFX& display) = 0;
    virtual void loop(LGFX& display) = 0;
    virtual bool usesLVGL() { return false; }
};
```

All apps implement `setup()` (called once after display init) and `loop()` (called repeatedly). The display reference is fully initialized before `setup()` is called.

### Two App Patterns

**Raw GFX apps** draw directly to LovyanGFX:

```cpp
void MyApp::loop(LGFX& display) {
    canvas->fillScreen(TFT_BLACK);
    // ... draw with LovyanGFX primitives ...
    canvas->pushSprite(0, 0);
}
```

**LVGL apps** use LVGL on top of LovyanGFX for widget-based UIs (see `apps/ui_demo/` and `apps/wifi_setup/`):

```cpp
void MyLvglApp::setup(LGFX& display) {
    lv_init();
    // set up LVGL display driver backed by LGFX
    // create widgets
}

void MyLvglApp::loop(LGFX& display) {
    lv_timer_handler();
    delay(5);
}

bool MyLvglApp::usesLVGL() { return true; }
```

The `usesLVGL()` flag allows the launcher to adjust behavior for LVGL apps (e.g., tick timing) without affecting raw GFX apps.

### App Selection

In `src/main.cpp`, a `#if defined(APP_*)` block selects the app:

```cpp
#if defined(APP_STARFIELD)
#include "starfield_app.h"
static StarfieldApp app_instance;
#elif defined(APP_MY_APP)
#include "my_app.h"
static MyApp app_instance;
#else
// Default
#include "starfield_app.h"
static StarfieldApp app_instance;
#endif
```

The default app flag (`-DAPP_STARFIELD`) is set in the shared `[env]` section of `platformio.ini`.

## Launcher (src/main.cpp)

The launcher is intentionally thin:

1. Init serial
2. Init display (via board-specific LGFX constructor)
3. Call `app->setup(display)`
4. Loop `app->loop(display)` forever

All application logic lives in the app, not in `main.cpp`.

## Build System

### PlatformIO

`platformio.ini` uses PlatformIO's environment inheritance:

- `[env]` -- shared settings: platform, framework, lib_deps, common build flags
- `[board_base]` -- common board hardware settings (flash size, PSRAM, partition table). Board envs use `extends = board_base`.
- `[app_*]` -- reusable app presets. Each defines `build_flags` (app macro + include paths) and `build_src_filter` (which sources to compile). Examples: `[app_starfield]`, `[app_camera]`, `[app_system]`.
- `[env:<board>]` or `[env:<board>-<app>]` -- per-board environments that combine a board macro with an app preset via `${app_name.build_flags}` and `${app_name.build_src_filter}`.

Example board-app environment:

```ini
[env:touch-lcd-35bc-camera]
extends = board_base
build_flags = ${env.build_flags} ${app_camera.build_flags}
    -DBOARD_TOUCH_LCD_35BC
    -include include/boards/esp32_s3_touch_lcd_35bc.h
build_src_filter = ${app_camera.build_src_filter}
```

### Makefile and Scripts

Shell scripts in `scripts/` are the primary interface:

- `./scripts/build.sh BOARD [APP]` -- build firmware
- `./scripts/flash.sh BOARD [APP]` -- build, flash (auto-detects port, fixes permissions)
- `./scripts/monitor.sh` -- serial monitor
- `./scripts/identify.sh` -- detect connected boards

The Makefile wraps the same operations:

- `make build BOARD=<env> [APP=<app>]`
- `make flash BOARD=<env> [APP=<app>]`
- `make monitor`
- `make new-app NAME=<name>` -- scaffold from `apps/_template/`

## Shared Libraries

Libraries in `lib/` are organized into three categories (see `lib/README.md` for full details):

- **Display Drivers** -- Custom LovyanGFX panel/touch drivers for ICs not in upstream (e.g., `Panel_AXS15231B`).
- **Hardware Abstraction (HAL)** -- Board-agnostic wrappers for peripherals: `hal_imu`, `hal_rtc`, `hal_power`, `hal_audio`, `hal_camera`, `hal_wifi`, `hal_mqtt`, `hal_ui` (LVGL), `hal_voice`, `hal_sleep`, `hal_ble`, `hal_touch`, `hal_io_expander`, `hal_sdcard`, `hal_debug`.
- **Utility Libraries** -- Reusable engines not tied to hardware (e.g., `StarField`).

HALs read pin definitions from the board header at compile time. Several HALs support dual I2C mode (`init(TwoWire&)` for standalone, `initLgfx()` for shared LovyanGFX I2C bus).

Libraries are only compiled when explicitly included in an app's `build_src_filter`. App-specific code goes in `apps/<name>/`.

## Design Decisions

**LovyanGFX over TFT_eSPI**: Better QSPI support, cleaner multi-panel API, built-in sprite double-buffering, active maintenance for ESP32-S3 targets.

**Compile-time selection over runtime**: Both board and app are selected at compile time via preprocessor macros. This eliminates runtime overhead, dead code, and simplifies memory allocation. The tradeoff is that switching apps or boards requires a rebuild.

**PSRAM-backed sprites**: All boards have 8MB PSRAM. Full-frame `LGFX_Sprite` with `setPsram(true)` enables flicker-free double-buffered rendering without consuming internal SRAM.

**Orthogonal board/app axes**: Board configs know nothing about apps. Apps know nothing about specific boards. The only coupling is the `LGFX` type and `DISPLAY_WIDTH`/`DISPLAY_HEIGHT` macros.

## LVGL Integration

LVGL 9.2 is integrated via the `hal_ui` library. LVGL apps use `usesLVGL()` to signal the launcher. The `hal_ui` library handles LVGL display driver setup on top of the LovyanGFX `LGFX` instance, theming, and common widgets. See `apps/ui_demo/` for a basic example and `apps/wifi_setup/` for a production LVGL app.
