// Desktop simulator entry point.
// Uses LovyanGFX SDL2 backend to render apps in a desktop window.
//
// Usage: .pio/build/simulator/program [--board NAME]
//   Default board: touch-amoled-241b

#include "sim_display.h"
#include "sim_hal.h"

#if defined(SDL_h_)

#include "app.h"

// App selection via build flag (same pattern as ESP32)
#if defined(APP_EFFECTS)
#include "effects_app.h"
static EffectsApp app_instance;
#elif defined(APP_UI_DEMO)
#include "ui_demo_app.h"
static UiDemoApp app_instance;
#elif defined(APP_STARFIELD)
#include "starfield_app.h"
static StarfieldApp app_instance;
#else
#include "starfield_app.h"
static StarfieldApp app_instance;
#endif

static LGFX* display = nullptr;
static App* app = &app_instance;

static const char* board_name = "touch-amoled-241b";

static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--board") == 0 && i + 1 < argc) {
            board_name = argv[++i];
        }
    }
}

void setup() {
    // Already initialized in main
}

void loop() {
    app->loop(*display);
}

int user_func(bool* running) {
    const BoardInfo* board = find_board(board_name);
    if (!board) {
        fprintf(stderr, "Unknown board: %s\nAvailable boards:\n", board_name);
        for (int i = 0; i < BOARD_COUNT; i++) {
            fprintf(stderr, "  %s (%dx%d)\n", BOARDS[i].env_name, BOARDS[i].width, BOARDS[i].height);
        }
        return 1;
    }

    // Auto-scale small displays for visibility
    int scale = 1;
    if (board->width < 300 || board->height < 300) scale = 2;

    display = new LGFX(board->width, board->height, scale);

    char title[128];
    snprintf(title, sizeof(title), "ESP32 Sim: %s (%dx%d %s)",
             board->display_name, board->width, board->height, board->driver);
    display->setTitle(title);

    display->init();
    display->fillScreen(TFT_BLACK);
    display->setBrightness(255);

    printf("Simulator: %s %dx%d (%s via %s)\n",
           board->display_name, board->width, board->height,
           board->driver, board->interface);
    printf("App: %s\n", app->name());

    app->setup(*display);

    while (*running) {
        loop();
    }

    delete display;
    return 0;
}

int main(int argc, char** argv) {
    parse_args(argc, argv);
    return lgfx::Panel_sdl::main(user_func);
}

#endif
