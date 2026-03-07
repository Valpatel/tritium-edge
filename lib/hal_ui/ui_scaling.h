#pragma once
#include <cstdint>

// Resolution-adaptive layout helpers.
// Boards range from 172x640 (tall narrow) to 800x480 (wide).

struct UiScale {
    int screen_w;
    int screen_h;
    int short_side;  // min(w, h)
    int long_side;   // max(w, h)
    bool is_portrait; // h > w

    // Scaled sizes
    int status_bar_h;
    int padding;
    int font_size_small;
    int font_size_normal;
    int font_size_large;
    int icon_size;
    int list_item_h;
    int btn_h;
};

inline UiScale ui_compute_scale(int w, int h) {
    UiScale s;
    s.screen_w = w;
    s.screen_h = h;
    s.short_side = (w < h) ? w : h;
    s.long_side = (w > h) ? w : h;
    s.is_portrait = (h > w);

    // Scale everything relative to short side
    // 172px (tiny) -> 800px (large)
    if (s.short_side >= 400) {
        // Large: 800x480, 600x450
        s.status_bar_h = 36;
        s.padding = 12;
        s.font_size_small = 12;
        s.font_size_normal = 16;
        s.font_size_large = 24;
        s.icon_size = 24;
        s.list_item_h = 48;
        s.btn_h = 44;
    } else if (s.short_side >= 240) {
        // Medium: 320x480, 368x448, 240x536
        s.status_bar_h = 28;
        s.padding = 8;
        s.font_size_small = 10;
        s.font_size_normal = 14;
        s.font_size_large = 20;
        s.icon_size = 20;
        s.list_item_h = 40;
        s.btn_h = 36;
    } else {
        // Small: 172x640
        s.status_bar_h = 22;
        s.padding = 4;
        s.font_size_small = 10;
        s.font_size_normal = 12;
        s.font_size_large = 16;
        s.icon_size = 16;
        s.list_item_h = 32;
        s.btn_h = 28;
    }

    return s;
}
