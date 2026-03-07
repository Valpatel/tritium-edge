#pragma once
// Reusable battery display widget for LGFX_Sprite.
// Reads from a BatteryStatus struct and draws an icon with optional text.
//
// Usage:
//   #include "battery_widget.h"
//   BatteryWidget widget;
//   widget.init();                              // default config
//   widget.draw(sprite, batt.getStatus());      // each frame

#include <LovyanGFX.hpp>
#include "battery_monitor.h"

// ── Style: controls how much information is displayed ─────────────────

enum class BatteryWidgetStyle : uint8_t {
    ICON_ONLY,          // Just the battery icon
    ICON_PERCENT,       // Icon + "85%"
    ICON_PERCENT_VOLT,  // Icon + "85%" + "3.92V"
    FULL,               // Icon + "85%" + "3.92V" + "~2h10m"
};

// ── Anchor: which corner or edge the widget is pinned to ─────────────

enum class BatteryWidgetAnchor : uint8_t {
    TOP_LEFT,
    TOP_RIGHT,
    TOP_CENTER,
    BOTTOM_LEFT,
    BOTTOM_RIGHT,
    BOTTOM_CENTER,
    CENTER,
};

// ── Configuration ─────────────────────────────────────────────────────

struct BatteryWidgetConfig {
    BatteryWidgetStyle  style  = BatteryWidgetStyle::ICON_PERCENT;
    BatteryWidgetAnchor anchor = BatteryWidgetAnchor::TOP_CENTER;
    int      margin      = 4;       // Pixels from edge
    uint16_t text_color  = 0x7BEF;  // Dim gray default
    int      icon_width  = 22;
    int      icon_height = 10;
    int      text_size   = 1;       // LovyanGFX text scale (1=6x8, 2=12x16, etc.)
};

// ── Widget class ──────────────────────────────────────────────────────

class BatteryWidget {
public:
    void init(const BatteryWidgetConfig& config = {});

    // Draw the widget onto a sprite.  Reads status from the provided
    // BatteryStatus.  This does NOT own the BatteryMonitor -- caller
    // manages its lifecycle.
    void draw(LGFX_Sprite& sprite, const BatteryStatus& status);

    // Update config at runtime
    void setConfig(const BatteryWidgetConfig& config);

private:
    BatteryWidgetConfig _config;

    void     drawIcon(LGFX_Sprite& spr, int x, int y, const BatteryStatus& status);
    void     drawText(LGFX_Sprite& spr, int x, int y, const BatteryStatus& status);
    uint16_t socColor(int soc, bool charging);
};
