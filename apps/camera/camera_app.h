#pragma once
#include "app.h"

class CameraApp : public App {
public:
    const char* name() override { return "Camera"; }
    void setup(LGFX& display) override;
    void loop(LGFX& display) override;
};
