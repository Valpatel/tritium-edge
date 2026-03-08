#pragma once
#include "app.h"
#include "StarField.h"

class StarfieldApp : public App {
public:
    const char* name() override { return "Starfield"; }
    void setup(esp_lcd_panel_handle_t panel, int width, int height) override;
    void loop() override;

private:
    static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);
    static uint16_t tintedColor(float brightness, StarTint tint);
    float currentSpeed() const;
    void pushFramebuffer();

    esp_lcd_panel_handle_t _panel = nullptr;
    int _w = 0;
    int _h = 0;

    uint16_t* _framebuf = nullptr;
    uint16_t* _dma_buf = nullptr;
    StarField* _starfield = nullptr;

    static constexpr int CHUNK_ROWS = 64;

    // FPS tracking
    uint32_t _frame_count = 0;
    uint32_t _fps_timer = 0;

    // Warp speed cycling
    uint32_t _warp_timer = 0;
    bool _warping = false;
    uint32_t _warp_cycle_timer = 0;
};
