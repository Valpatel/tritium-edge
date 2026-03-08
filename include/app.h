#pragma once
#include "esp_lcd_panel_ops.h"

class App {
public:
    virtual ~App() = default;
    virtual const char* name() = 0;
    virtual bool usesLVGL() { return false; }
    virtual void setup(esp_lcd_panel_handle_t panel, int width, int height) = 0;
    virtual void loop() = 0;
};
