#pragma once
#include "app.h"
#include <lvgl.h>
#include "ui_scaling.h"

class UiDemoApp : public App {
public:
    const char* name() override { return "UI Demo"; }
    bool usesLVGL() override { return true; }
    void setup(LGFX& display) override;
    void loop(LGFX& display) override;

private:
    lv_obj_t* _status_bar = nullptr;
    lv_obj_t* _list = nullptr;
    uint32_t _uptime_timer = 0;
    UiScale _scale = {};
};
