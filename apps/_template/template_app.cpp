#include "template_app.h"
#include <Arduino.h>

void TemplateApp::setup(LGFX& display) {
    Serial.printf("App: %s\n", name());
    display.fillScreen(TFT_BLACK);

    // Initialize your app state here.
    // display.width() and display.height() give the actual panel resolution.
}

void TemplateApp::loop(LGFX& display) {
    // Called repeatedly. Render one frame here.
    _frame++;
}
