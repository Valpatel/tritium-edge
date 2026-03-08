#include <Arduino.h>
#include "display.h"
#include "app.h"

// --- App selection via build flag ---
#if defined(APP_STARFIELD)
#include "starfield_app.h"
static StarfieldApp app_instance;

#elif !defined(APP_STARFIELD)
// All other apps need porting to esp_lcd
#if defined(APP_SYSTEM)
#error "system app not yet ported to esp_lcd"
#elif defined(APP_WIFI_SETUP)
#error "wifi_setup app not yet ported to esp_lcd"
#elif defined(APP_UI_DEMO)
#error "ui_demo app not yet ported to esp_lcd"
#elif defined(APP_CAMERA)
#error "camera app not yet ported to esp_lcd"
#elif defined(APP_EFFECTS)
#error "effects app not yet ported to esp_lcd"
#elif defined(APP_TEST)
#error "test app not yet ported to esp_lcd"
#else
// Default app: starfield
#include "starfield_app.h"
static StarfieldApp app_instance;
#endif
#endif

static App* app = &app_instance;

// Serial command buffer for board identification
static char _cmd_buf[32];
static uint8_t _cmd_idx = 0;

static void handleSerialCommands() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            _cmd_buf[_cmd_idx] = '\0';
            if (strcmp(_cmd_buf, "IDENTIFY") == 0) {
                Serial.printf("{\"board\":\"%s\",\"display\":\"%dx%d\",\"app\":\"%s\"}\n",
                              DISPLAY_DRIVER,
                              display_get_width(), display_get_height(),
                              app->name());
            }
            _cmd_idx = 0;
        } else if (_cmd_idx < sizeof(_cmd_buf) - 1) {
            _cmd_buf[_cmd_idx++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    esp_err_t ret = display_init();
    if (ret != ESP_OK) {
        Serial.printf("[tritium] Display init FAILED: 0x%x\n", ret);
        return;
    }

    int w = display_get_width();
    int h = display_get_height();
    esp_lcd_panel_handle_t panel = display_get_panel();

    display_set_brightness(255);

    Serial.printf("[tritium] Board: %s %dx%d\n", DISPLAY_DRIVER, w, h);

    app->setup(panel, w, h);
    Serial.printf("[tritium] App '%s' started\n", app->name());
}

void loop() {
    handleSerialCommands();
    app->loop();
}
