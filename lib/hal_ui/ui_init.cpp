#include "ui_init.h"
#include <cstdlib>

#ifndef SIMULATOR
#include <esp_heap_caps.h>
#endif

static lgfx::LGFX_Device* s_lcd = nullptr;

static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    s_lcd->startWrite();
    s_lcd->setAddrWindow(area->x1, area->y1, w, h);
    s_lcd->pushPixels((uint16_t*)px_map, w * h, true);
    s_lcd->endWrite();

    lv_display_flush_ready(disp);
}

static void allocate_buffers(size_t buf_size, uint8_t*& buf1, uint8_t*& buf2) {
    buf1 = nullptr;
    buf2 = nullptr;

#ifndef SIMULATOR
    // Try PSRAM first on ESP32
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > buf_size * 2) {
        buf1 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        buf2 = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    }
#endif

    if (!buf1) {
        buf1 = (uint8_t*)malloc(buf_size);
        buf2 = (uint8_t*)malloc(buf_size);
    }
}

lv_display_t* ui_init(lgfx::LGFX_Device& display) {
    s_lcd = &display;

    lv_init();

    int w = display.width();
    int h = display.height();

    lv_display_t* disp = lv_display_create(w, h);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    // Use 1/10th of screen size for partial rendering
    size_t buf_size = (size_t)w * (h / 10) * sizeof(uint16_t);
    if (buf_size < 32 * 1024) buf_size = 32 * 1024;

    uint8_t* buf1;
    uint8_t* buf2;
    allocate_buffers(buf_size, buf1, buf2);

    lv_display_set_buffers(disp, buf1, buf2, buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    return disp;
}

uint32_t ui_tick() {
    return lv_timer_handler();
}
