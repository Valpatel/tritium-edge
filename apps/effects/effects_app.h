#pragma once
#include "app.h"

class EffectsApp : public App {
public:
    const char* name() override { return "Effects"; }
    void setup(LGFX& display) override;
    void loop(LGFX& display) override;

private:
    LGFX_Sprite* _canvas = nullptr;
    int _current = 0;
    uint32_t _last_frame = 0;
    uint32_t _effect_start = 0;
    uint32_t _frame_count = 0;
    uint32_t _fps_timer = 0;
    float _fps = 0;
    float _last_time = 0;
    static constexpr uint32_t EFFECT_DURATION_MS = 8000;  // 8s per effect
    static constexpr uint32_t TRANSITION_MS = 500;
    static constexpr int TARGET_FPS = 30;  // Simulate ESP32-ish frame rate
    static constexpr uint32_t FRAME_MIN_MS = 1000 / TARGET_FPS;

    void drawOverlay(const char* name, int w, int h);
};
