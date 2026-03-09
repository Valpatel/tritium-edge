#pragma once
// Template for creating a new app.
//
// To create a new app:
//   1. Copy this folder to apps/your_app_name/
//   2. Rename TemplateApp to YourAppName in both .h and .cpp
//   3. Add -DAPP_YOUR_APP to platformio.ini build_flags (or pass on CLI)
//   4. Add an #elif for your app in src/main.cpp's app selection block
//   5. Add your app's source path to src_filter in platformio.ini
//
// Your app receives a fully initialized esp_lcd panel handle.
// Use the width/height passed to setup() for resolution-aware logic.
//
// Rendering pattern:
//   Draw into the PSRAM framebuffer (_framebuf), then call pushFramebuffer()
//   to DMA-transfer it to the panel in chunks.

#include "app.h"

class TemplateApp : public App {
public:
    const char* name() override { return "Template"; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;
    uint16_t* getFramebuffer(int& width, int& height) override { width = _w; height = _h; return _framebuf; }

private:
    esp_lcd_panel_handle_t _panel = nullptr;
    int _w = 0, _h = 0;
    uint16_t* _framebuf = nullptr;
    uint16_t* _dma_buf = nullptr;
    static constexpr int CHUNK_ROWS = 40;

    void pushFramebuffer();

    // Add your state here
    uint32_t _frame = 0;
};
