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

**LVGL apps** (planned) layer LVGL on top of LovyanGFX for widget-based UIs:

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

- `[env]` -- shared settings: platform, framework, lib_deps, default app flags, `build_src_filter`
- `[env:<board>]` -- per-board overrides: `build_flags` with board macro and display dimensions

The `build_src_filter` in `[env]` controls which source files are compiled:

```ini
build_src_filter =
    +<../src/>
    +<../apps/starfield/>
    +<../lib/StarField/>
```

When adding a new app, update the filter to include the app's source directory.

### Makefile

The Makefile wraps PlatformIO for convenience:

- `make build BOARD=<env>` -- build for a specific board
- `make flash BOARD=<env>` -- build and upload
- `make new-app NAME=<name>` -- scaffold from `apps/_template/`

## Shared Libraries

Reusable engines live in `lib/`:

- **StarField** (`lib/StarField/`) -- 3D starfield simulation with configurable star count, speed, and projection. Used by the starfield app but decoupled from display code.

Libraries in `lib/` are available to any app via include path. App-specific code goes in `apps/<name>/`.

## Design Decisions

**LovyanGFX over TFT_eSPI**: Better QSPI support, cleaner multi-panel API, built-in sprite double-buffering, active maintenance for ESP32-S3 targets.

**Compile-time selection over runtime**: Both board and app are selected at compile time via preprocessor macros. This eliminates runtime overhead, dead code, and simplifies memory allocation. The tradeoff is that switching apps or boards requires a rebuild.

**PSRAM-backed sprites**: All boards have 8MB PSRAM. Full-frame `LGFX_Sprite` with `setPsram(true)` enables flicker-free double-buffered rendering without consuming internal SRAM.

**Orthogonal board/app axes**: Board configs know nothing about apps. Apps know nothing about specific boards. The only coupling is the `LGFX` type and `DISPLAY_WIDTH`/`DISPLAY_HEIGHT` macros.

## Planned: LVGL Integration

An LVGL-based UI framework is planned for apps that need widgets, icons, and structured layouts. The `App` interface already supports this via the `usesLVGL()` method. LVGL apps will initialize LVGL's display driver on top of the LovyanGFX `LGFX` instance in their `setup()` method, keeping the board abstraction layer unchanged.
