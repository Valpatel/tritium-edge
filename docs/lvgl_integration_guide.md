# LVGL 9.x Integration Guide for ESP32-S3 Multi-Board Project

This document covers the standard patterns for integrating LVGL 9.x with Espressif's
esp_lcd framework on ESP32-S3, tailored to our multi-board project architecture.

## Table of Contents

1. [LVGL 8 vs LVGL 9 API Changes](#lvgl-8-vs-lvgl-9-api-changes)
2. [Display Flush Callback Pattern](#display-flush-callback-pattern)
3. [DMA Buffer Management](#dma-buffer-management)
4. [Input Driver Pattern](#input-driver-pattern)
5. [lv_conf.h Recommendations](#lv_confh-recommendations)
6. [esp_lvgl_port Component](#esp_lvgl_port-component)
7. [Vendor Demo Analysis](#vendor-demo-analysis)
8. [Our Current Setup](#our-current-setup)
9. [Raw Pixel Apps Without LVGL](#raw-pixel-apps-without-lvgl)
10. [Dual Architecture: LVGL + Raw Pixel Apps](#dual-architecture-lvgl--raw-pixel-apps)

---

## LVGL 8 vs LVGL 9 API Changes

### Display Registration

**LVGL 8** used a driver struct pattern:

```c
static lv_disp_draw_buf_t disp_buf;
static lv_disp_drv_t disp_drv;

lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_pixel_count);
lv_disp_drv_init(&disp_drv);
disp_drv.hor_res = 320;
disp_drv.ver_res = 480;
disp_drv.flush_cb = my_flush_cb;
disp_drv.draw_buf = &disp_buf;
disp_drv.user_data = panel_handle;
disp_drv.full_refresh = 1;
lv_disp_drv_register(&disp_drv);
```

**LVGL 9** uses object-oriented creation functions:

```c
lv_display_t* disp = lv_display_create(320, 480);
lv_display_set_flush_cb(disp, my_flush_cb);
lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
lv_display_set_buffers(disp, buf1, buf2, buf_size_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
lv_display_set_user_data(disp, panel_handle);
```

Key changes:
- `lv_disp_drv_t` struct is gone. Everything is set via `lv_display_*()` functions.
- `lv_disp_draw_buf_t` is gone. Buffers set directly with `lv_display_set_buffers()`.
- `lv_disp_flush_ready()` renamed to `lv_display_flush_ready()`.
- Buffer size is in **bytes** (not pixel count).
- Render mode is explicit: `LV_DISPLAY_RENDER_MODE_PARTIAL`, `_DIRECT`, or `_FULL`.

### Input Device Registration

**LVGL 8:**
```c
static lv_indev_drv_t indev_drv;
lv_indev_drv_init(&indev_drv);
indev_drv.type = LV_INDEV_TYPE_POINTER;
indev_drv.read_cb = my_touch_cb;
lv_indev_drv_register(&indev_drv);
```

**LVGL 9:**
```c
lv_indev_t* indev = lv_indev_create();
lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
lv_indev_set_read_cb(indev, my_touch_cb);
```

### Tick Source

**LVGL 8:** Either `lv_tick_inc()` from timer ISR, or `LV_TICK_CUSTOM` with an expression.

**LVGL 9:** Same options, plus new `lv_tick_set_cb(my_get_millis)` function. Our project
uses `LV_TICK_CUSTOM` with `millis()` which works for both.

### Color Format / Byte Swap

**LVGL 8:** `LV_COLOR_16_SWAP 1` in lv_conf.h handled byte swapping globally.

**LVGL 9:** `LV_COLOR_16_SWAP` is removed. Options:
1. Use `LV_COLOR_FORMAT_RGB565` and call `lv_draw_sw_rgb565_swap()` in the flush callback.
2. Use `LV_COLOR_FORMAT_NATIVE_REVERSE` (renders pre-swapped, zero overhead).
3. Configure SPI hardware for different byte order (best if hardware supports it).

For our QSPI panels (AXS15231B, RM690B0, SH8601Z, RM67162), option 1 is the proven
approach from vendor demos. Option 2 may not work on all LVGL versions.

---

## Display Flush Callback Pattern

### Pattern A: LovyanGFX Backend (Our Current Approach)

This is what we use now in `lib/hal_ui/ui_init.cpp`. LovyanGFX handles all SPI/QSPI
transactions internally:

```cpp
static lgfx::LGFX_Device* s_lcd = nullptr;

static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    s_lcd->startWrite();
    s_lcd->setAddrWindow(area->x1, area->y1, w, h);
    s_lcd->pushPixels((uint16_t*)px_map, w * h, true);  // true = swap bytes
    s_lcd->endWrite();

    lv_display_flush_ready(disp);
}
```

Pros: Simple, LovyanGFX handles byte swap and bus abstraction.
Cons: Synchronous (blocks until transfer completes), no DMA overlap with LVGL rendering.

### Pattern B: esp_lcd Backend with DMA (Vendor Demo Pattern)

This is what Waveshare demos use. The flush callback initiates an async DMA transfer,
and the DMA-complete ISR signals LVGL to continue:

```c
// ISR callback - called when DMA transfer completes
static bool notify_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx) {
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

// LVGL flush callback
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                           uint8_t *color_p) {
    esp_lcd_panel_handle_t panel =
        (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    // Byte-swap for SPI byte order (big-endian wire format)
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(color_p, w * h);

    // Initiate async DMA transfer - returns immediately
    // notify_flush_ready() will be called from ISR when done
    esp_lcd_panel_draw_bitmap(panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color_p);
    // Do NOT call lv_display_flush_ready() here!
    // The ISR callback handles it.
}
```

The `on_color_trans_done` callback is registered during panel IO setup:

```c
esp_lcd_panel_io_spi_config_t io_config = {
    .cs_gpio_num = LCD_CS,
    .dc_gpio_num = -1,              // QSPI has no DC pin
    .spi_mode = 3,
    .pclk_hz = 40 * 1000 * 1000,
    .trans_queue_depth = 10,
    .on_color_trans_done = notify_flush_ready,
    .user_ctx = disp,               // passed to ISR callback
    .lcd_cmd_bits = 32,
    .lcd_param_bits = 8,
    .flags.quad_mode = true,
};
```

Pros: True async DMA - LVGL renders next buffer while DMA pushes current one.
Cons: More complex setup, requires esp_lcd panel driver for each display controller.

### Pattern C: Full-Refresh with PSRAM + DMA Chunking (Waveshare 3.49 LVGL 9 Demo)

For QSPI panels that require full-screen refresh (like AXS15231B), the vendor demo
uses PSRAM for full-frame buffers but chunks the DMA transfer since DMA cannot access
PSRAM directly:

```c
#define LVGL_DMA_BUFF_LEN  (LCD_HRES * 64 * 2)   // ~22KB DMA-capable chunk
#define LVGL_SPIRAM_BUFF_LEN (LCD_HRES * LCD_VRES * 2)  // Full frame in PSRAM

static uint16_t *dma_buf;    // Internal RAM, DMA-capable
static SemaphoreHandle_t flush_done;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
    esp_lcd_panel_handle_t panel =
        (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    lv_draw_sw_rgb565_swap(color_p, LCD_HRES * LCD_VRES);

    const int chunk_rows = 64;
    const int chunks = LCD_VRES / chunk_rows;
    uint16_t *src = (uint16_t *)color_p;

    xSemaphoreGive(flush_done);  // Prime the semaphore
    for (int i = 0; i < chunks; i++) {
        xSemaphoreTake(flush_done, portMAX_DELAY);
        memcpy(dma_buf, src, LVGL_DMA_BUFF_LEN);
        esp_lcd_panel_draw_bitmap(panel,
                                  0, i * chunk_rows,
                                  LCD_HRES, (i + 1) * chunk_rows,
                                  dma_buf);
        src += (LVGL_DMA_BUFF_LEN / 2);
    }
    xSemaphoreTake(flush_done, portMAX_DELAY);
    lv_display_flush_ready(disp);
}
```

Buffer setup for this pattern:

```c
// Two full-frame buffers in PSRAM for double-buffering
uint8_t *buf1 = (uint8_t *)heap_caps_malloc(SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
uint8_t *buf2 = (uint8_t *)heap_caps_malloc(SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
// One DMA-capable chunk buffer in internal RAM
dma_buf = (uint16_t *)heap_caps_malloc(DMA_BUFF_LEN, MALLOC_CAP_DMA);

lv_display_set_buffers(disp, buf1, buf2, SPIRAM_BUFF_LEN,
                       LV_DISPLAY_RENDER_MODE_FULL);
```

This pattern is required for our AXS15231B boards (3.49 and 3.5B-C) because:
- The QSPI panel needs full-frame updates (partial refresh causes tearing/artifacts).
- Full-frame buffers must live in PSRAM (too large for internal RAM).
- DMA cannot access PSRAM on ESP32-S3, so data must be copied in chunks to an
  internal-RAM DMA buffer.

---

## DMA Buffer Management

### Memory Regions on ESP32-S3

| Region | Capability Flag | Size | DMA? | Speed |
|--------|----------------|------|------|-------|
| Internal SRAM | `MALLOC_CAP_INTERNAL` | ~320KB total | Yes | Fast |
| Internal DMA-capable | `MALLOC_CAP_DMA` | Subset of internal | Yes | Fast |
| PSRAM (SPI) | `MALLOC_CAP_SPIRAM` | 8MB | No* | Slower |

*ESP32-S3 PSRAM is not directly DMA-accessible for SPI LCD transfers.

### Buffer Sizing Recommendations

| Board | Resolution | Full Frame (RGB565) | Recommended Strategy |
|-------|-----------|---------------------|---------------------|
| touch-lcd-349 | 172x640 | 220KB | Full refresh, 2x PSRAM + DMA chunk |
| touch-lcd-35bc | 320x480 | 307KB | Full refresh, 2x PSRAM + DMA chunk |
| touch-amoled-18 | 368x448 | 330KB | Partial (1/10), 2x internal or PSRAM |
| touch-amoled-241b | 450x600 | 540KB | Partial (1/10), 2x PSRAM |
| amoled-191m | 240x536 | 257KB | Partial (1/10), 2x internal (25KB each) |
| touch-lcd-43c-box | 800x480 | 768KB | Partial (1/10), 2x PSRAM, RGB direct |

For partial rendering, 1/10th of screen is the recommended minimum. For boards with
< 50KB per 1/10th buffer (191m, 18), internal DMA-capable RAM is feasible.

### Allocation Pattern

```cpp
size_t buf_size;
uint8_t *buf1, *buf2;
lv_display_render_mode_t mode;

if (needs_full_refresh) {
    // AXS15231B QSPI panels need full refresh
    buf_size = width * height * 2;  // Full frame RGB565
    buf1 = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    buf2 = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    mode = LV_DISPLAY_RENDER_MODE_FULL;
} else {
    // AMOLED panels support partial refresh
    buf_size = width * (height / 10) * 2;
    if (buf_size <= 40 * 1024) {
        // Small enough for internal DMA-capable RAM
        buf1 = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
        buf2 = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    } else {
        buf1 = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        buf2 = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    }
    mode = LV_DISPLAY_RENDER_MODE_PARTIAL;
}

lv_display_set_buffers(disp, buf1, buf2, buf_size, mode);
```

### Full Refresh vs Partial Refresh

**Full Refresh** (`LV_DISPLAY_RENDER_MODE_FULL`):
- LVGL renders entire screen every frame, flush_cb receives full framebuffer.
- Required for QSPI panels (AXS15231B) that cannot do partial area updates reliably.
- Higher memory usage (2x full-frame buffers) but simpler flush logic.
- Double-buffering: LVGL renders into one while the other is being transmitted.

**Partial Refresh** (`LV_DISPLAY_RENDER_MODE_PARTIAL`):
- LVGL only redraws changed areas, flush_cb receives sub-regions.
- Works well with AMOLED panels (RM690B0, SH8601Z, RM67162) that support `setAddrWindow`.
- Lower memory usage (1/10th screen buffers), better for complex UIs with few changes.
- Multiple flush calls per frame (one per dirty region).

**Direct Refresh** (`LV_DISPLAY_RENDER_MODE_DIRECT`):
- Buffers must be full-screen sized. LVGL renders only changed pixels within the buffer.
- Best for RGB parallel panels (ST7262 on 43C-BOX) where the framebuffer IS the display.
- The flush_cb can do a simple memcpy to the display framebuffer.

---

## Input Driver Pattern

### LVGL 9 Touch Input Setup

```cpp
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    uint16_t x, y;
    bool touched = read_touch(&x, &y);  // Your touch driver

    if (touched) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// During init:
lv_indev_t *touch_indev = lv_indev_create();
lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
lv_indev_set_read_cb(touch_indev, touch_read_cb);
```

### AXS15231B Integrated Touch (3.49 and 3.5B-C)

The AXS15231B touch uses a 4-byte handshake protocol over I2C. From the vendor demo:

```c
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
    uint8_t buf[32] = {0};

    // I2C write-then-read to touch address 0x3B
    i2c_master_write_read_dev(touch_handle, cmd, 11, buf, 32);

    uint16_t x = (((uint16_t)buf[2] & 0x0f) << 8) | (uint16_t)buf[3];
    uint16_t y = (((uint16_t)buf[4] & 0x0f) << 8) | (uint16_t)buf[5];

    if (buf[1] > 0 && buf[1] < 5) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
```

Note: Our project already handles AXS15231B touch via `Touch_AXS15231B` in the custom
LovyanGFX panel driver (`lib/Panel_AXS15231B/`). For LVGL input, we can read touch
coordinates from the LovyanGFX touch driver:

```cpp
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    lgfx::touch_point_t tp;
    int count = s_lcd->getTouchRaw(&tp, 1);

    if (count > 0) {
        data->point.x = tp.x;
        data->point.y = tp.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
```

### CST816S Touch (AMOLED boards)

The CST816S (used on the 2.41-B via FT5x06-compatible protocol) is handled similarly.
The vendor demo uses `esp_lcd_touch` abstraction. With LovyanGFX, the same
`getTouchRaw()` pattern works since LovyanGFX has built-in CST816S/FT5x06 support.

---

## lv_conf.h Recommendations

Our current `include/lv_conf.h` is already structured for LVGL 9. Below are the
critical settings with explanations.

### Color Settings

```c
#define LV_COLOR_DEPTH 16   // RGB565 - matches all our display panels
```

Note: `LV_COLOR_16_SWAP` does not exist in LVGL 9. Byte swapping must be handled in
the flush callback (see above).

### Memory / Stdlib

```c
// Option 1: LVGL built-in allocator (current, simpler)
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
#define LV_MEM_SIZE           (96 * 1024U)   // 96KB for widget tree/styles
// Note: LV_MEM_SIZE is for LVGL's internal heap (widget objects, styles, etc.)
// Display buffers are allocated separately via heap_caps_malloc().

// Option 2: Use C stdlib (routes to ESP32 heap, can use PSRAM)
// #define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
// With this option, LVGL uses malloc/free which on ESP32 with Arduino
// can be configured to use PSRAM via menuconfig or build flags.
```

Recommendation: Use `LV_STDLIB_BUILTIN` with 64-96KB. This keeps LVGL's internal
allocations in fast internal RAM. Display buffers are allocated separately with
explicit PSRAM/DMA placement.

### Tick Source

```c
#define LV_TICK_CUSTOM 1
#ifdef SIMULATOR
    #define LV_TICK_CUSTOM_INCLUDE <SDL2/SDL.h>
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (SDL_GetTicks())
#else
    #define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif
```

This is correct and sufficient. With `LV_TICK_CUSTOM`, no `lv_tick_inc()` timer is
needed. LVGL reads `millis()` directly. The Waveshare demos use `lv_tick_inc()` from
an `esp_timer` ISR, which is also valid but unnecessary with `LV_TICK_CUSTOM`.

### Refresh Period

```c
#define LV_DEF_REFR_PERIOD 16    // ~60 FPS target
```

For QSPI panels with full-refresh, 33ms (30 FPS) may be more realistic. For partial-
refresh AMOLED panels, 16ms is achievable.

### Operating System / Thread Safety

```c
// For Arduino + FreeRTOS on ESP32:
#define LV_USE_OS  LV_OS_NONE
```

Even though ESP32 runs FreeRTOS, `LV_OS_NONE` is correct for our architecture because:
- We handle thread safety manually with a mutex (same as vendor demos).
- `LV_OS_FREERTOS` requires ESP-IDF native FreeRTOS headers that may conflict with
  Arduino's FreeRTOS wrapper.
- The LVGL task loop + mutex pattern is well-proven on ESP32 Arduino.

Manual mutex pattern (from vendor demos):

```cpp
static SemaphoreHandle_t lvgl_mux = xSemaphoreCreateMutex();

bool lvgl_lock(int timeout_ms) {
    TickType_t ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, ticks) == pdTRUE;
}

void lvgl_unlock() {
    xSemaphoreGive(lvgl_mux);
}

// LVGL task (runs on core 0)
void lvgl_task(void *arg) {
    uint32_t delay_ms;
    for (;;) {
        if (lvgl_lock(-1)) {
            delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        delay_ms = constrain(delay_ms, 5, 500);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
```

### ARM Draw Backends

```c
// Disable ARM-specific backends (ESP32-S3 is Xtensa, not ARM)
#define LV_USE_DRAW_ARM2D  0
#define LV_USE_DRAW_HELIUM 0
```

### Display Buffer Sizing by Board

For `LV_MEM_SIZE` (LVGL internal heap for widgets), 64-96KB is sufficient for most UIs.
Increase to 128KB+ for complex screens with many widgets, animations, or image caching.

Display buffer sizes (allocated separately, not from LV_MEM_SIZE):

| Board | Resolution | Partial (1/10) | Full Frame | Recommended |
|-------|-----------|---------------|------------|-------------|
| touch-lcd-349 | 172x640 | 22KB | 220KB | Full, 2x PSRAM |
| touch-lcd-35bc | 320x480 | 30KB | 307KB | Full, 2x PSRAM |
| touch-amoled-18 | 368x448 | 33KB | 330KB | Partial, 2x internal |
| touch-amoled-241b | 450x600 | 54KB | 540KB | Partial, 2x PSRAM |
| amoled-191m | 240x536 | 26KB | 257KB | Partial, 2x internal |
| touch-lcd-43c-box | 800x480 | 77KB | 768KB | Direct (RGB panel) |

### Complete Recommended lv_conf.h

Below are the settings that differ from defaults or are critical for ESP32-S3. All
other settings can remain at LVGL 9 defaults:

```c
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// Color
#define LV_COLOR_DEPTH 16

// Stdlib - use LVGL's built-in allocator for internal heap
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN

// LVGL internal heap size (widgets, styles, event data - NOT display buffers)
#define LV_MEM_SIZE (96 * 1024U)

// HAL
#define LV_DEF_REFR_PERIOD 33    // 30 FPS default (override per-board if needed)

// OS - manual mutex, not LVGL-managed
#define LV_USE_OS LV_OS_NONE

// Tick - use millis() directly
#define LV_TICK_CUSTOM 1
#ifdef SIMULATOR
    #define LV_TICK_CUSTOM_INCLUDE <SDL2/SDL.h>
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (SDL_GetTicks())
#else
    #define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

// Disable ARM draw backends (Xtensa CPU)
#define LV_USE_DRAW_ARM2D  0
#define LV_USE_DRAW_HELIUM 0

// Logging (enable for debugging, disable for production)
#define LV_USE_LOG 0

// Fonts
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// Widgets (enable what we need)
#define LV_USE_LABEL     1
#define LV_USE_BTN       1
#define LV_USE_BTNMATRIX 1
#define LV_USE_BAR       1
#define LV_USE_SLIDER    1
#define LV_USE_SWITCH    1
#define LV_USE_LIST      1
#define LV_USE_MSGBOX    1
#define LV_USE_ROLLER    1
#define LV_USE_DROPDOWN  1
#define LV_USE_TEXTAREA  1
#define LV_USE_TABLE     1
#define LV_USE_ARC       1
#define LV_USE_SPINNER   1
#define LV_USE_IMG       1
#define LV_USE_LINE      1
#define LV_USE_CHECKBOX  1

// Layouts
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

// Themes
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

// Symbol fonts
#define LV_USE_FONT_PLACEHOLDER 1

#endif // LV_CONF_H
```

---

## esp_lvgl_port Component

### What It Is

`esp_lvgl_port` is Espressif's official LVGL integration component for ESP-IDF. It
handles:
- Display registration with esp_lcd
- Touch input registration with esp_lcd_touch
- Tick timer management
- LVGL task creation with FreeRTOS mutex
- Rotation/mirroring coordination

### Framework Compatibility

**esp_lvgl_port is ESP-IDF only.** It is not compatible with the Arduino framework.
It depends on ESP-IDF component manager (`idf_component.yml`) and ESP-IDF-specific
APIs. It cannot be used in PlatformIO Arduino projects.

### What We Do Instead

Since we use Arduino framework with PlatformIO, we implement the same functionality
manually in `lib/hal_ui/`:

| esp_lvgl_port feature | Our equivalent |
|-----------------------|----------------|
| Display flush | `ui_init.cpp` flush callback via LovyanGFX |
| Buffer allocation | `allocate_buffers()` with PSRAM fallback |
| Touch input | LovyanGFX touch via `getTouchRaw()` |
| Tick timer | `LV_TICK_CUSTOM` with `millis()` |
| LVGL task | `ui_tick()` called from `App::loop()` |
| Mutex | Manual FreeRTOS mutex (to be added for multi-task) |

The key feature we are currently missing compared to esp_lvgl_port is the async
DMA flush (Pattern B/C above) and the FreeRTOS task with mutex protection.

### Available Versions

As of 2026, esp_lvgl_port v2.7.2 is available via the ESP Component Registry. It
supports LVGL 8 and 9. If we ever migrate from Arduino to ESP-IDF, this would be
the standard integration path.

---

## Vendor Demo Analysis

### 3.49 LVGL 8 Demo (Pattern: Full Refresh + DMA Chunking)

File: `references/ESP32-S3-Touch-LCD-3.49/Examples/Arduino/09_LVGL_V8_Test/lvgl_port.c`

Key observations:
- Uses esp_lcd with `esp_lcd_new_panel_axs15231b()` (vendor panel driver).
- `disp_drv.full_refresh = 1` - AXS15231B requires full-frame updates.
- Two PSRAM buffers (full frame each), one internal DMA buffer for chunked transfer.
- `lv_disp_flush_ready()` called after all DMA chunks complete.
- Touch uses raw I2C with 4-byte handshake protocol.

### 3.49 LVGL 9 Demo (Pattern: Same, LVGL 9 API)

File: `references/ESP32-S3-Touch-LCD-3.49/Examples/Arduino/10_LVGL_V9_Test/lvgl_port.c`

Same DMA chunking pattern, updated to LVGL 9 API:
- `lv_display_create()` instead of `lv_disp_drv_init()`.
- `lv_display_set_buffers()` with `LV_DISPLAY_RENDER_MODE_FULL`.
- `lv_display_set_user_data()` for panel handle.
- `lv_indev_create()` + `lv_indev_set_type()` + `lv_indev_set_read_cb()`.
- `lv_draw_sw_rgb565_swap()` in flush callback for byte order.
- `lv_disp_flush_ready()` still used (alias for `lv_display_flush_ready()` in 9.x).

### 2.41-B AMOLED Demo (Pattern: Partial Refresh + DMA ISR)

File: `references/ESP32-S3-Touch-AMOLED-2.41-Demo/CN/Arduino/examples/09_LVGL_Test/`

Key observations:
- Uses LVGL 8 API with `lv_disp_drv_t`.
- Uses esp_lcd with `esp_lcd_new_panel_sh8601()` (SH8601Z QSPI AMOLED driver).
- **Partial refresh** - buffers are 1/10th screen (buf_height = VRES/10).
- Buffers allocated in internal DMA-capable RAM (`MALLOC_CAP_DMA`), ~54KB each.
- DMA ISR callback calls `lv_disp_flush_ready()` directly (true async).
- Touch via esp_lcd_touch_ft5x06 I2C driver.
- `rounder_cb` rounds coordinates to even numbers (SH8601Z requirement).
- Memory offset: portrait mode needs `+16` on X axis for RM690B0 (452px memory width).
- No byte swapping - SH8601Z may handle endianness internally, or the esp_lcd driver
  handles it.

### Key Takeaways from Vendor Demos

1. **AXS15231B boards MUST use full refresh** with DMA chunking pattern.
2. **AMOLED boards can use partial refresh** with smaller internal-RAM DMA buffers.
3. **All demos use esp_lcd** (not LovyanGFX) for display + touch.
4. **Thread safety** is always a manual mutex + dedicated FreeRTOS task.
5. **Byte swapping** is done in flush_cb via `lv_draw_sw_rgb565_swap()` in LVGL 9.

---

## Our Current Setup

### What We Have

`lib/hal_ui/` provides:
- `ui_init.h/cpp`: LVGL 9 init via LovyanGFX, partial rendering, PSRAM buffers.
- `ui_theme.h/cpp`: Dark theme for AMOLED.
- `ui_widgets.h/cpp`: Status bar, notifications, battery icon.
- `ui_scaling.h`: Resolution-adaptive layout parameters.

### Current Flush Approach

Our flush callback uses LovyanGFX synchronously:

```cpp
s_lcd->startWrite();
s_lcd->setAddrWindow(area->x1, area->y1, w, h);
s_lcd->pushPixels((uint16_t*)px_map, w * h, true);
s_lcd->endWrite();
lv_display_flush_ready(disp);
```

This works but is suboptimal:
- **Synchronous**: CPU blocks during SPI transfer.
- **No full-refresh mode**: Uses `LV_DISPLAY_RENDER_MODE_PARTIAL` which may cause
  issues on AXS15231B panels.
- **No DMA overlap**: Cannot render next frame while pushing current one.

### Missing Touch Input

`ui_init()` does not register an LVGL input device. Touch input needs to be added
using the pattern described in the Input Driver section above.

### Recommended Improvements

1. Add touch input registration to `ui_init()`.
2. Detect board type and select full-refresh vs partial-refresh accordingly.
3. For AXS15231B boards, implement DMA chunking pattern (Pattern C).
4. Add mutex protection for multi-task scenarios.
5. Consider dedicated LVGL FreeRTOS task instead of calling from `loop()`.

---

## Raw Pixel Apps Without LVGL

### Direct LovyanGFX Rendering (Current Approach)

Apps like starfield draw directly to an `LGFX_Sprite` and push to display:

```cpp
void StarfieldApp::loop(LGFX& display) {
    sprite.fillScreen(0);
    // ... draw stars ...
    sprite.pushSprite(&display, 0, 0);
}
```

This bypasses LVGL entirely. LovyanGFX handles the SPI/QSPI transfer.

### Direct esp_lcd Rendering (Without LovyanGFX or LVGL)

If using esp_lcd directly for raw pixel apps:

```cpp
// Allocate framebuffer
uint16_t *fb = (uint16_t *)heap_caps_malloc(width * height * 2, MALLOC_CAP_SPIRAM);

// Draw pixels directly
fb[y * width + x] = color_rgb565;

// Push entire frame to display
esp_lcd_panel_draw_bitmap(panel, 0, 0, width, height, fb);
```

For QSPI panels with DMA, use the same chunking pattern as the LVGL flush callback
but driven from your render loop instead.

### Benefits of esp_lcd for Raw Pixel Apps

- Async DMA transfers (non-blocking).
- Standard ESP-IDF API, works alongside LVGL.
- No LovyanGFX dependency for simple framebuffer pushes.

---

## Dual Architecture: LVGL + Raw Pixel Apps

### The Problem

Our project has both:
- **LVGL apps** (ui_demo, wifi_setup, system): Need LVGL widget tree, touch input.
- **Raw pixel apps** (starfield, camera, effects): Draw directly to framebuffer.

Both need to coexist in the same firmware image.

### Solution: App-Level Separation (Current Design)

Our `App` base class already supports this with `usesLVGL()`:

```cpp
class App {
public:
    virtual void setup(LGFX& display) = 0;
    virtual void loop(LGFX& display) = 0;
    virtual bool usesLVGL() { return false; }
};
```

The launcher/main.cpp checks `usesLVGL()`:
- If `true`: Initialize LVGL, register display/touch, call `lv_timer_handler()` in loop.
- If `false`: Skip LVGL init, app draws directly to display.

### Solution: Shared esp_lcd Panel Handle

A more integrated approach for the future:

1. Initialize esp_lcd panel once at startup (board-level).
2. Pass `esp_lcd_panel_handle_t` to apps.
3. LVGL apps: Register the handle with LVGL flush callback.
4. Raw pixel apps: Call `esp_lcd_panel_draw_bitmap()` directly.
5. Both use the same DMA path.

```
Board Init (esp_lcd)
  |
  +-- panel_handle
  |
  +-- LVGL App: lv_display_set_user_data(disp, panel_handle)
  |             flush_cb -> esp_lcd_panel_draw_bitmap()
  |
  +-- Raw App:  esp_lcd_panel_draw_bitmap(panel_handle, ...)
```

This eliminates the need for LovyanGFX in the display path entirely, though LovyanGFX
would still be useful for its sprite engine, font rendering, and geometric primitives
in raw pixel apps.

### Switching Between Apps

When switching from an LVGL app to a raw pixel app:
1. Delete the LVGL display object (`lv_display_delete()`).
2. Free LVGL buffers.
3. Call `lv_deinit()` (available in LVGL 9).
4. Raw app takes over the panel handle directly.

When switching from raw to LVGL:
1. Stop raw rendering.
2. Call `lv_init()`, create display, set buffers and flush callback.
3. Register touch input.
4. Start LVGL task.

This is the cleanest approach since LVGL init/deinit is lightweight.

---

## References

### Official Documentation

- [LVGL 9.2 Display Porting](https://docs.lvgl.io/9.2/porting/display.html)
- [LVGL 9.x Display Setup](https://docs.lvgl.io/master/main-modules/display/setup.html)
- [LVGL 9.x Color Format](https://docs.lvgl.io/master/main-modules/display/color_format.html)
- [LVGL ESP32 Tips and Tricks](https://docs.lvgl.io/master/integration/chip_vendors/espressif/tips_and_tricks.html)
- [ESP-IDF LCD API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd.html)
- [esp_lvgl_port Component](https://components.espressif.com/components/espressif/esp_lvgl_port)
- [LV_COLOR_16_SWAP Removal (Issue #6104)](https://github.com/lvgl/lvgl/issues/6104)
- [LV_COLOR_16_SWAP Performance (Issue #6317)](https://github.com/lvgl/lvgl/issues/6317)

### Vendor Reference Code

- 3.49 LVGL 8: `references/ESP32-S3-Touch-LCD-3.49/Examples/Arduino/09_LVGL_V8_Test/`
- 3.49 LVGL 9: `references/ESP32-S3-Touch-LCD-3.49/Examples/Arduino/10_LVGL_V9_Test/`
- 2.41-B LVGL 8: `references/ESP32-S3-Touch-AMOLED-2.41-Demo/CN/Arduino/examples/09_LVGL_Test/`
- 3.49 lv_conf.h (v9): `references/ESP32-S3-Touch-LCD-3.49/Arduino_Libraries/lvgl9/lv_conf.h`
- 2.41-B lv_conf.h (v8): `references/ESP32-S3-Touch-AMOLED-2.41-Demo/CN/Arduino/libraries/lv_conf.h`

### Our Implementation

- Current LVGL init: `lib/hal_ui/ui_init.cpp`
- Current lv_conf.h: `include/lv_conf.h`
- App base class: `include/app.h`
- Custom panel driver: `lib/Panel_AXS15231B/`
