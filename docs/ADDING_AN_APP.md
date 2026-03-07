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

### 5. Update `platformio.ini`

In the shared `[env]` section, update the default app flags and source filter. To make your app the default:

```ini
build_flags =
    ...
    -DAPP_MYAPP
    -Iapps/myapp
build_src_filter =
    +<../src/>
    +<../apps/myapp/>
```

To keep starfield as the default but build your app for a specific board, override `build_flags` in that board's `[env:<board>]` section.

### 6. If Your App Uses a Library

If your app depends on a shared library in `lib/` (like StarField), add its include path and source filter:

```ini
build_flags =
    ...
    -Ilib/YourLib
build_src_filter =
    ...
    +<../lib/YourLib/>
```

### 7. Build and Test

```bash
make build BOARD=touch-amoled-241b
```

## Tips

- **Double buffering**: Create an `LGFX_Sprite` with `setPsram(true)` for flicker-free rendering. Draw to the sprite, then `pushSprite(0, 0)`.
- **Resolution independence**: Use `display.width()` and `display.height()` rather than hardcoded values. Your app should work on any board.
- **FPS monitoring**: Print FPS to serial with a simple frame counter (see the starfield app for an example).
- **LVGL apps**: Initialize LVGL in `setup()`, call `lv_timer_handler()` in `loop()`, and override `usesLVGL()` to return true.
