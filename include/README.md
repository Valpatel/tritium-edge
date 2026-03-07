# include/

Shared headers for board abstraction, display initialization, app interface, and LVGL configuration.

## Header Dependency Graph

```mermaid
graph TD
    MAIN[src/main.cpp] --> APP_H[app.h<br>App base class]
    MAIN --> DI[display_init.h<br>Board-specific LGFX class]
    DI --> B1[boards/esp32_s3_touch_amoled_241b.h]
    DI --> B2[boards/esp32_s3_amoled_191m.h]
    DI --> B3[boards/esp32_s3_touch_amoled_18.h]
    DI --> B4[boards/esp32_s3_touch_lcd_35bc.h]
    DI --> B5[boards/esp32_s3_touch_lcd_43c_box.h]
    DI --> B6[boards/esp32_s3_touch_lcd_349.h]
    LVGL[lv_conf.h<br>LVGL settings] -.-> DI
```

## Files

| File | Purpose |
|------|---------|
| `app.h` | Base `App` class interface — all apps implement `setup()`, `loop()`, `name()` |
| `display_init.h` | Board-specific `LGFX` class definitions (one per `#if BOARD_*` block). Configures LovyanGFX bus, panel, touch, and backlight for each board. |
| `lv_conf.h` | LVGL 9.2 build configuration (color depth, memory, fonts, widgets) |
| `boards/` | Per-board pin definition headers ([details](boards/README.md)) |

> **Note:** Board headers are force-included via `-include` in platformio.ini build flags, so all compilation units (including HAL libraries) have access to pin defines and `HAS_*` feature flags.
