#include <Arduino.h>
#include "display_init.h"
#include "debug_log.h"
#include "app.h"

// --- App selection via build flag ---
#if defined(APP_SYSTEM)
#include "system_app.h"
static SystemApp app_instance;

#elif defined(APP_WIFI_SETUP)
#include "wifi_setup_app.h"
static WifiSetupApp app_instance;

#elif defined(APP_UI_DEMO)
#include "ui_demo_app.h"
static UiDemoApp app_instance;

#elif defined(APP_STARFIELD)
#include "starfield_app.h"
static StarfieldApp app_instance;

#else
// Default app: starfield
#include "starfield_app.h"
static StarfieldApp app_instance;
#endif

static LGFX display;
static App* app = &app_instance;

void setup() {
    Serial.begin(115200);
    delay(500);
    DebugLog::init(DBG_BACKEND_SERIAL);

    DBG_INFO("main", "Board: %s", DISPLAY_DRIVER);
    DBG_INFO("main", "Display: %dx%d via %s", DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_IF);

    // Board-specific pre-init (I/O expander display reset, etc.)
#if defined(BOARD_TOUCH_LCD_35BC)
    tca9554_reset_display();
#endif

    display.init();
    display.setRotation(DISPLAY_ROTATION);
    display.fillScreen(TFT_BLACK);
    display.setBrightness(255);

    // Manual backlight fallback — only for 3.5B-C where Light_PWM alone isn't enough
#if defined(BOARD_TOUCH_LCD_35BC) && defined(LCD_BL) && LCD_BL >= 0
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);
#endif

    DBG_INFO("main", "Display ready: %dx%d (rotation %d)",
             display.width(), display.height(), DISPLAY_ROTATION);

    app->setup(display);
    DBG_INFO("main", "App '%s' started", app->name());
}

// Serial command buffer for board identification
static char _cmd_buf[32];
static uint8_t _cmd_idx = 0;

static void handleSerialCommands() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            _cmd_buf[_cmd_idx] = '\0';
            if (strcmp(_cmd_buf, "IDENTIFY") == 0) {
                Serial.printf("{\"board\":\"%s\",\"display\":\"%dx%d\",\"interface\":\"%s\",\"app\":\"%s\"}\n",
                              DISPLAY_DRIVER, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_IF, app->name());
            }
            _cmd_idx = 0;
        } else if (_cmd_idx < sizeof(_cmd_buf) - 1) {
            _cmd_buf[_cmd_idx++] = c;
        }
    }
}

void loop() {
    handleSerialCommands();
    app->loop(display);
}
