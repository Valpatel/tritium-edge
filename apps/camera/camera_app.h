#pragma once
#include "app.h"

class CameraApp : public App {
public:
    const char* name() override { return "Camera"; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;

private:
    esp_lcd_panel_handle_t _panel = nullptr;
    int _w = 0;
    int _h = 0;
    uint16_t* _framebuf = nullptr;  // PSRAM framebuffer (display size)
    uint16_t* _dma_buf = nullptr;   // DMA chunk buffer
    static constexpr int CHUNK_ROWS = 40;

    void pushFramebuffer();
};
