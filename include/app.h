#pragma once
#include "esp_lcd_panel_ops.h"
#include <cstdint>

class App {
public:
    virtual ~App() = default;
    virtual const char* name() = 0;
    virtual bool usesLVGL() { return false; }
    virtual void setup(esp_lcd_panel_handle_t panel, int width, int height) = 0;
    virtual void loop() = 0;

    // Screen capture — returns RGB565 framebuffer pointer and dimensions.
    // Apps with a software framebuffer override this; others return nullptr.
    virtual uint16_t* getFramebuffer(int& width, int& height) { width = 0; height = 0; return nullptr; }
};
