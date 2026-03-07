#include "battery_widget.h"
#include <cstdio>

// ── Public API ────────────────────────────────────────────────────────

void BatteryWidget::init(const BatteryWidgetConfig& config) {
    _config = config;
}

void BatteryWidget::setConfig(const BatteryWidgetConfig& config) {
    _config = config;
}

void BatteryWidget::draw(LGFX_Sprite& sprite, const BatteryStatus& status) {
    const int sw = sprite.width();
    const int sh = sprite.height();
    const int m  = _config.margin;
    const int iw = _config.icon_width;
    const int ih = _config.icon_height;
    const int nub = (iw >= 40) ? 4 : 2;  // terminal nub width, scales with icon
    const int total_icon_w = iw + nub;
    const auto anchor = _config.anchor;

    // Build the text strings we'll need
    char pct_buf[8] = {};
    char volt_buf[12] = {};
    char time_buf[12] = {};

    if (status.soc >= 0) {
        snprintf(pct_buf, sizeof(pct_buf), "%d%%", status.soc);
    } else if (status.charging) {
        snprintf(pct_buf, sizeof(pct_buf), "CHG");
    } else {
        snprintf(pct_buf, sizeof(pct_buf), "---");
    }

    if (_config.style >= BatteryWidgetStyle::ICON_PERCENT_VOLT && status.voltage > 0.5f) {
        snprintf(volt_buf, sizeof(volt_buf), "%.2fV", status.voltage);
    }

    if (_config.style == BatteryWidgetStyle::FULL && status.minutes_remaining > 0) {
        int mins = status.minutes_remaining;
        if (mins >= 60) {
            snprintf(time_buf, sizeof(time_buf), "~%dh%dm", mins / 60, mins % 60);
        } else {
            snprintf(time_buf, sizeof(time_buf), "~%dm", mins);
        }
    }

    // Measure total widget size for centering
    // Layout: [icon] [gap] [pct_text]   (all on one line)
    //         [volt_text]               (line below, if present)
    //         [time_text]               (line below that, if present)
    const int ts = _config.text_size;
    int gap = 3 * ts;
    int text_h = 8 * ts;  // base font is 6x8
    int line_spacing = text_h + 2 * ts;
    int total_h = (ih > text_h) ? ih : text_h;
    if (volt_buf[0]) total_h += line_spacing;
    if (time_buf[0]) total_h += line_spacing;

    // Calculate icon position based on anchor
    int ix = 0, iy = 0;

    bool is_right  = (anchor == BatteryWidgetAnchor::TOP_RIGHT ||
                      anchor == BatteryWidgetAnchor::BOTTOM_RIGHT);
    bool is_bottom = (anchor == BatteryWidgetAnchor::BOTTOM_LEFT ||
                      anchor == BatteryWidgetAnchor::BOTTOM_RIGHT ||
                      anchor == BatteryWidgetAnchor::BOTTOM_CENTER);
    bool is_center_x = (anchor == BatteryWidgetAnchor::TOP_CENTER ||
                        anchor == BatteryWidgetAnchor::BOTTOM_CENTER ||
                        anchor == BatteryWidgetAnchor::CENTER);
    bool is_center_y = (anchor == BatteryWidgetAnchor::CENTER);

    // X position
    if (is_center_x) {
        ix = (sw - total_icon_w) / 2;
    } else if (is_right) {
        ix = sw - m - total_icon_w;
    } else {
        ix = m;
    }

    // Y position
    if (is_center_y) {
        iy = (sh - total_h) / 2;
    } else if (is_bottom) {
        iy = sh - m - total_h;
    } else {
        iy = m;
    }

    // ── Draw icon ──────────────────────────────────────────────────
    drawIcon(sprite, ix, iy, status);

    if (_config.style == BatteryWidgetStyle::ICON_ONLY) return;

    // ── Draw percentage text beside the icon ───────────────────────
    const uint16_t tc = _config.text_color;
    sprite.setTextSize(ts);
    sprite.setTextColor(tc);

    if (is_center_x) {
        // Text to the right of icon
        sprite.setTextDatum(middle_left);
        sprite.drawString(pct_buf, ix + total_icon_w + gap, iy + ih / 2);
    } else if (is_right) {
        // Text to the left of icon
        sprite.setTextDatum(middle_right);
        sprite.drawString(pct_buf, ix - gap, iy + ih / 2);
    } else {
        // Text to the right of icon
        sprite.setTextDatum(middle_left);
        sprite.drawString(pct_buf, ix + total_icon_w + gap, iy + ih / 2);
    }

    // ── Draw sub-lines (voltage, time) below the icon ──────────────
    int line_y = iy + line_spacing;

    if (is_center_x) {
        sprite.setTextDatum(top_center);
        int cx = sw / 2;
        if (volt_buf[0]) {
            sprite.drawString(volt_buf, cx, line_y);
            line_y += line_spacing;
        }
        if (time_buf[0]) {
            sprite.drawString(time_buf, cx, line_y);
        }
    } else if (is_right) {
        sprite.setTextDatum(top_right);
        int rx = ix + total_icon_w;
        if (volt_buf[0]) {
            sprite.drawString(volt_buf, rx, line_y);
            line_y += line_spacing;
        }
        if (time_buf[0]) {
            sprite.drawString(time_buf, rx, line_y);
        }
    } else {
        sprite.setTextDatum(top_left);
        if (volt_buf[0]) {
            sprite.drawString(volt_buf, ix, line_y);
            line_y += line_spacing;
        }
        if (time_buf[0]) {
            sprite.drawString(time_buf, ix, line_y);
        }
    }
}

// ── Icon rendering ───────────────────────────────────────────────────

void BatteryWidget::drawIcon(LGFX_Sprite& spr, int x, int y,
                              const BatteryStatus& status) {
    const int w = _config.icon_width;
    const int h = _config.icon_height;
    const uint16_t outline = _config.text_color;

    // Battery body outline
    spr.drawRect(x, y, w, h, outline);

    // Terminal nub on the right side
    int nub_w = (w >= 40) ? 4 : 2;
    int nub_inset = h / 4;
    spr.fillRect(x + w, y + nub_inset, nub_w, h - nub_inset * 2, outline);

    // Fill interior proportional to SOC
    const int inner_x = x + 1;
    const int inner_y = y + 1;
    const int inner_w = w - 2;
    const int inner_h = h - 2;

    int soc = status.soc;

    if (soc < 0) {
        if (status.charging) {
            spr.fillRect(inner_x, inner_y, inner_w, inner_h, 0x07FF);
        }
    } else {
        if (soc > 100) soc = 100;
        int fill_w = (inner_w * soc) / 100;
        if (fill_w > 0) {
            uint16_t color = socColor(soc, status.charging);
            spr.fillRect(inner_x, inner_y, fill_w, inner_h, color);
        }
    }
}

// ── SOC color lookup ─────────────────────────────────────────────────

uint16_t BatteryWidget::socColor(int soc, bool charging) {
    if (charging) return 0x07FF;  // cyan
    if (soc > 50) return 0x07E0;  // green
    if (soc > 20) return 0xFFE0;  // yellow
    return 0xF800;                 // red
}
