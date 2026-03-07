#pragma once
#include "app.h"
#include "StarField.h"

class StarfieldApp : public App {
public:
    const char* name() override { return "Starfield"; }
    void setup(LGFX& display) override;
    void loop(LGFX& display) override;

private:
    static uint16_t tintedColor(float brightness, StarTint tint);
    void drawSplash(LGFX& display);
    void drawFpsOverlay();
    float currentSpeed() const;

    LGFX_Sprite* _canvas = nullptr;
    StarField* _starfield = nullptr;

    // FPS tracking & profiling
    uint32_t _frame_count = 0;
    uint32_t _fps_timer = 0;
    float _fps = 0.0f;
    uint32_t _render_ms = 0;
    uint32_t _push_ms = 0;

    // Splash screen
    uint32_t _start_time = 0;
    bool _splash_done = false;

    // Warp
    uint32_t _warp_timer = 0;
    bool _warping = false;
};
