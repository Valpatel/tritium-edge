#pragma once

// Simulator display using LovyanGFX SDL2 backend.
// Creates an SDL window at the specified board resolution.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#if defined(SDL_h_)

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_sdl _panel;

public:
    LGFX(int w, int h, int scale = 1) {
        auto cfg = _panel.config();
        cfg.memory_width = w;
        cfg.memory_height = h;
        cfg.panel_width = w;
        cfg.panel_height = h;
        _panel.config(cfg);
        _panel.setScaling(scale, scale);
        setPanel(&_panel);
    }

    void setTitle(const char* title) {
        _panel.setWindowTitle(title);
    }
};

#endif

// Board resolution lookup
struct BoardInfo {
    const char* env_name;
    const char* display_name;
    const char* driver;
    const char* interface;
    int width;
    int height;
};

inline const BoardInfo BOARDS[] = {
    {"touch-amoled-241b", "AMOLED 2.41-B",  "RM690B0",   "QSPI", 450, 600},
    {"amoled-191m",       "AMOLED 1.91-M",  "RM67162",   "QSPI", 240, 536},
    {"touch-amoled-18",   "AMOLED 1.8",     "SH8601",    "QSPI", 368, 448},
    {"touch-lcd-35bc",    "LCD 3.5B-C",     "AXS15231B", "QSPI", 320, 480},
    {"touch-lcd-43c-box", "LCD 4.3C-BOX",   "ST7262",    "RGB",  800, 480},
    {"touch-lcd-349",     "LCD 3.49",       "AXS15231B", "QSPI", 172, 640},
};
inline constexpr int BOARD_COUNT = sizeof(BOARDS) / sizeof(BOARDS[0]);

inline const BoardInfo* find_board(const char* name) {
    for (int i = 0; i < BOARD_COUNT; i++) {
        if (strcmp(BOARDS[i].env_name, name) == 0) return &BOARDS[i];
    }
    return nullptr;
}
