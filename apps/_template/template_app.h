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
// Your app receives a fully initialized LGFX display reference.
// Use DISPLAY_WIDTH / DISPLAY_HEIGHT macros for resolution-aware logic.
//
// Two patterns are supported:
//   Raw GFX: Draw directly to LGFX in loop(). (e.g. starfield)
//   LVGL UI: Init LVGL on LGFX in setup(), call lv_timer_handler() in loop(),
//            and override usesLVGL() to return true.

#include "app.h"

class TemplateApp : public App {
public:
    const char* name() override { return "Template"; }
    void setup(LGFX& display) override;
    void loop(LGFX& display) override;

private:
    // Add your state here
    uint32_t _frame = 0;
};
