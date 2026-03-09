#pragma once
#include "app.h"

class EffectsApp : public App {
public:
    const char* name() override { return "Effects"; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;
    uint16_t* getFramebuffer(int& width, int& height) override { width = _w; height = _h; return _framebuf; }

private:
    esp_lcd_panel_handle_t _panel = nullptr;
    int _w = 0;
    int _h = 0;
    uint16_t* _framebuf = nullptr;
    uint16_t* _dma_buf = nullptr;
    static constexpr int CHUNK_ROWS = 40;

    int _current = 0;
    uint32_t _last_frame = 0;
    uint32_t _effect_start = 0;
    uint32_t _frame_count = 0;
    uint32_t _fps_timer = 0;
    float _fps = 0;
    float _last_time = 0;
    static constexpr uint32_t EFFECT_DURATION_MS = 8000;
    static constexpr uint32_t TRANSITION_MS = 500;
    static constexpr int TARGET_FPS = 30;
    static constexpr uint32_t FRAME_MIN_MS = 1000 / TARGET_FPS;

    void pushFramebuffer();
    void drawOverlay(const char* name);
};
