#pragma once

#ifdef SIMULATOR
#include "sim_display.h"
#else
#include "display_init.h"
#endif

// Base interface for all apps.
//
// Raw GFX apps (e.g. starfield): draw directly to LGFX in loop().
// LVGL apps: initialize LVGL on top of LGFX in setup(), call
//   lv_timer_handler() in loop(), and return true from usesLVGL().
class App {
public:
    virtual ~App() = default;
    virtual const char* name() = 0;
    virtual void setup(LGFX& display) = 0;
    virtual void loop(LGFX& display) = 0;

    // Override to return true if this app manages its own LVGL instance.
    // The launcher may use this to adjust tick timing or skip direct draws.
    virtual bool usesLVGL() { return false; }
};
