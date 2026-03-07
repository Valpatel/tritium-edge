# Adding a New App

## Automated

```bash
make new-app NAME=myapp
```

This copies `apps/_template/` to `apps/myapp/`, renames the class, and prints next steps.

## Manual Steps

### 1. Create the App Directory

```bash
mkdir apps/myapp
```

### 2. Create the Header (`apps/myapp/myapp_app.h`)

```cpp
#pragma once
#include "app.h"

class MyappApp : public App {
public:
    const char* name() override { return "My App"; }
    void setup(LGFX& display) override;
    void loop(LGFX& display) override;

private:
    // Your state here
};
```

The `App` base class is defined in `include/app.h`. It provides:
- `setup(LGFX& display)` -- called once after display is initialized
- `loop(LGFX& display)` -- called repeatedly
- `usesLVGL()` -- override to return `true` if your app uses LVGL

### 3. Create the Implementation (`apps/myapp/myapp_app.cpp`)

```cpp
#include "myapp_app.h"
#include <Arduino.h>

void MyappApp::setup(LGFX& display) {
    Serial.printf("App: %s\n", name());
    display.fillScreen(TFT_BLACK);

    int w = display.width();
    int h = display.height();
    // Initialize your app state
}

void MyappApp::loop(LGFX& display) {
    // Render one frame
}
```

Use `display.width()` and `display.height()` for resolution-aware rendering. Use `DISPLAY_WIDTH` and `DISPLAY_HEIGHT` macros if you need compile-time constants.

### 4. Register the App in `src/main.cpp`

Add an `#elif` block to the app selection chain:

```cpp
#elif defined(APP_MYAPP)
#include "myapp_app.h"
static MyappApp app_instance;
```

### 5. Create an App Preset in `platformio.ini`

Add a reusable `[app_myapp]` section that defines what gets compiled for your app:

```ini
[app_myapp]
build_flags =
    -DAPP_MYAPP
    -Iapps/myapp
build_src_filter =
    +<../src/>
    +<../apps/myapp/>
```

If your app depends on shared libraries from `lib/`, add them here:

```ini
[app_myapp]
build_flags =
    -DAPP_MYAPP
    -Iapps/myapp
    -Ilib/hal_imu
    -Ilib/hal_wifi
build_src_filter =
    +<../src/>
    +<../apps/myapp/>
    +<../lib/hal_imu/>
    +<../lib/hal_wifi/>
```

### 6. Create Board-App Environments

For each board that should run your app, add an environment that combines the board config with your app preset:

```ini
[env:touch-amoled-241b-myapp]
extends = board_base
build_flags = ${env.build_flags} ${app_myapp.build_flags}
    -DBOARD_TOUCH_AMOLED_241B
    -include include/boards/esp32_s3_touch_amoled_241b.h
build_src_filter = ${app_myapp.build_src_filter}
```

The naming convention is `<board>-<app>`. This enables building with:

```bash
./scripts/build.sh touch-amoled-241b myapp
# or: make build BOARD=touch-amoled-241b APP=myapp
# or: pio run -e touch-amoled-241b-myapp
```

### 7. Build and Test

```bash
./scripts/flash.sh touch-amoled-241b myapp
./scripts/monitor.sh
```

## Existing Apps

| App | Flag | Description |
|-----|------|-------------|
| `starfield` | `APP_STARFIELD` | Default demo: 3D starfield animation |
| `camera` | `APP_CAMERA` | Camera preview (boards with OV5640) |
| `system` | `APP_SYSTEM` | Full hardware dashboard (all peripherals) |
| `ui_demo` | `APP_UI_DEMO` | LVGL UI demo |
| `wifi_setup` | `APP_WIFI_SETUP` | WiFi configuration UI |

## Tips

- **Double buffering**: Create an `LGFX_Sprite` with `setPsram(true)` for flicker-free rendering. Draw to the sprite, then `pushSprite(0, 0)`.
- **Resolution independence**: Use `display.width()` and `display.height()` rather than hardcoded values. Your app should work on any board.
- **FPS monitoring**: Print FPS to serial with a simple frame counter (see the starfield app for an example).
- **LVGL apps**: Use `hal_ui` for LVGL init. Call `lv_timer_handler()` in `loop()` and override `usesLVGL()` to return true. See `apps/ui_demo/` and `apps/wifi_setup/` for examples.
- **QSPI rendering**: On QSPI panels, prefer `pushSprite()` over `setAddrWindow()` + `pushPixels()` for reliable rendering.
