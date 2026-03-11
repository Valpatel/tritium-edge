"""
Tritium-OS UI Element Registry
================================
Declarative map of every interactive UI element, its expected behavior,
and how to find it on screen. This is the single source of truth for
what should be tested and what the outcome should be.

To add a new element: add an ElementSpec entry to ELEMENT_REGISTRY.
The test framework will automatically find, exercise, and validate it.
"""

from dataclasses import dataclass, field
from typing import Optional


@dataclass
class ElementSpec:
    """Specification for a single interactive UI element."""
    screen: str           # "Settings/Display", "Map", "Files", etc.
    widget_type: str      # "slider", "switch", "button", "dropdown", "bar"
    name: str             # Human-readable: "brightness", "wifi_scan"
    action: str           # "test_slider", "test_switch", "test_button",
                          # "test_dropdown", "skip", "readonly", "verify_present"

    # Matcher — how to find this element in the widget tree
    # Keys: type (required), text, min, max, index, y_min
    match: dict = field(default_factory=dict)

    # Expected behavior — varies by action type
    # slider:   {"delta": 50, "visual_min_pct": 0.3}
    # switch:   {"visual_min_pct": 0.05}
    # button:   {"max_shift_px": 2.0}
    # dropdown: {"visual_min_pct": 0.5}
    # skip:     {"reason": "kills connectivity"}
    # readonly: {"reason": "status indicator"}
    expect: dict = field(default_factory=dict)

    dangerous: bool = False   # Never interact — just verify presence
    restore: bool = True      # Restore original value after testing
    optional: bool = False    # Element may not exist (build config dependent)


# ── Settings tabs (order matters — matches firmware tab order) ──────────
SETTINGS_TABS = [
    "Display", "WiFi", "BLE", "Mesh", "Monitor",
    "Storage", "Tracking", "Power", "Screensaver",
    "Security", "System",
]


# ── Element Registry ────────────────────────────────────────────────────
# Every interactive element on every screen. Add new entries here when
# firmware adds new UI controls.

ELEMENT_REGISTRY: list[ElementSpec] = [

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / Display                                             ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Settings/Display",
        widget_type="slider", name="brightness",
        action="test_slider",
        match={"type": "slider", "min": 10, "max": 255},
        expect={"delta": 60, "visual_min_pct": 0.0},  # RGB panel has no backlight control
    ),
    ElementSpec(
        screen="Settings/Display",
        widget_type="dropdown", name="display_timeout",
        action="test_dropdown",
        match={"type": "dropdown", "index": 0},
        expect={"visual_min_pct": 0.5},
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / WiFi                                                ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Settings/WiFi",
        widget_type="switch", name="wifi_enable",
        action="skip",
        match={"type": "switch", "index": 0},
        expect={"reason": "toggling WiFi kills device connectivity"},
        dangerous=True,
    ),
    ElementSpec(
        screen="Settings/WiFi",
        widget_type="bar", name="wifi_signal",
        action="readonly",
        match={"type": "bar", "index": 0},
        expect={"reason": "signal strength indicator"},
        optional=True,  # cross-tab widget bleed shifts bar indices
    ),
    ElementSpec(
        screen="Settings/WiFi",
        widget_type="switch", name="wifi_ap_mode",
        action="skip",
        match={"type": "switch", "index": 1},
        expect={"reason": "toggling AP mode disrupts connectivity"},
        dangerous=True,
        optional=True,  # cross-tab widget bleed shifts switch indices
    ),
    ElementSpec(
        screen="Settings/WiFi",
        widget_type="button", name="wifi_scan",
        action="test_button",
        match={"type": "btn", "text_contains": "SCAN", "y_min": 100},
        expect={"max_shift_px": 2.0},
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / BLE                                                 ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Settings/BLE",
        widget_type="switch", name="ble_scanner_enable",
        action="skip",
        match={"type": "switch", "index": 0},
        expect={"reason": "toggling BLE scanner disrupts scanning"},
        dangerous=True, optional=True,
    ),
    ElementSpec(
        screen="Settings/BLE",
        widget_type="switch", name="ble_logging",
        action="test_switch",
        match={"type": "switch", "index": 1},
        expect={"visual_min_pct": 0.0},  # BLE runs stubs, switch may not visually change
        optional=True,
    ),
    ElementSpec(
        screen="Settings/BLE",
        widget_type="button", name="ble_scan_now",
        action="test_button",
        match={"type": "btn", "text_contains": "SCAN", "y_min": 80},
        expect={"max_shift_px": 2.0},
        optional=True,
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / Mesh                                                ║
    # ╚══════════════════════════════════════════════════════════════════╝
    # Currently no interactive controls

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / Monitor                                             ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Settings/Monitor",
        widget_type="bar", name="heap_usage",
        action="readonly",
        match={"type": "bar", "index": 0, "y_min": 80},
        expect={"reason": "Heap usage percentage"},
    ),
    ElementSpec(
        screen="Settings/Monitor",
        widget_type="bar", name="psram_usage",
        action="readonly",
        match={"type": "bar", "index": 1, "y_min": 80},
        expect={"reason": "PSRAM usage percentage"},
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / Storage                                             ║
    # ╚══════════════════════════════════════════════════════════════════╝
    # Currently no interactive controls (just nav buttons)

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / Tracking                                            ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Settings/Tracking",
        widget_type="switch", name="tracking_enable",
        action="test_switch",
        match={"type": "switch", "index": 0},
        expect={"visual_min_pct": 0.05},
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / Power                                               ║
    # ╚══════════════════════════════════════════════════════════════════╝
    # 4 power profile buttons at y=273 (no text, select power mode)
    ElementSpec(
        screen="Settings/Power",
        widget_type="button", name="power_profile_1",
        action="test_button",
        match={"type": "btn", "index": 0, "y_min": 100, "y_max": 430},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Settings/Power",
        widget_type="button", name="power_profile_2",
        action="test_button",
        match={"type": "btn", "index": 1, "y_min": 100, "y_max": 430},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Settings/Power",
        widget_type="button", name="power_profile_3",
        action="test_button",
        match={"type": "btn", "index": 2, "y_min": 100, "y_max": 430},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Settings/Power",
        widget_type="button", name="power_profile_4",
        action="test_button",
        match={"type": "btn", "index": 3, "y_min": 100, "y_max": 430},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Settings/Power",
        widget_type="slider", name="auto_sleep_timeout",
        action="readonly",
        match={"type": "slider", "max": 300},
        expect={"reason": "auto-sleep timeout indicator"},
    ),
    ElementSpec(
        screen="Settings/Power",
        widget_type="slider", name="display_off_timeout",
        action="readonly",
        match={"type": "slider", "max": 600},
        expect={"reason": "display-off timeout indicator"},
    ),
    ElementSpec(
        screen="Settings/Power",
        widget_type="button", name="reboot_power",
        action="skip",
        match={"type": "btn", "text_contains": "REBOOT"},
        expect={"reason": "reboots the device"},
        dangerous=True,
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / Screensaver                                         ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Settings/Screensaver",
        widget_type="slider", name="screensaver_timeout",
        action="test_slider",
        match={"type": "slider", "max": 600, "y_min": 80},
        expect={"delta": 60, "visual_min_pct": 0.01},
        optional=True,  # Hidden when screensaver is disabled (test disables it)
    ),
    ElementSpec(
        screen="Settings/Screensaver",
        widget_type="switch", name="sf_reverse_direction",
        action="test_switch",
        match={"type": "switch", "index": 0, "y_min": 80},
        expect={"visual_min_pct": 0.0},  # Screensaver disabled during test
        optional=True,
    ),
    ElementSpec(
        screen="Settings/Screensaver",
        widget_type="switch", name="sf_colored_stars",
        action="test_switch",
        match={"type": "switch", "index": 1, "y_min": 80},
        expect={"visual_min_pct": 0.0},  # Screensaver disabled during test
        optional=True,
    ),
    ElementSpec(
        screen="Settings/Screensaver",
        widget_type="slider", name="sf_star_size",
        action="test_slider",
        match={"type": "slider", "min": 1, "max": 6},
        expect={"delta": 1, "visual_min_pct": 0.0},  # Screensaver disabled during test
        optional=True,
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / Security                                            ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Settings/Security",
        widget_type="button", name="save_pin",
        action="test_button",
        match={"type": "btn", "text_contains": "Save PIN", "y_min": 100},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Settings/Security",
        widget_type="button", name="clear_pin",
        action="test_button",
        match={"type": "btn", "text_contains": "Clear PIN", "y_min": 100},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Settings/Security",
        widget_type="button", name="test_lock",
        action="test_button",
        match={"type": "btn", "text_contains": "Test Lock", "y_min": 100},
        expect={"max_shift_px": 2.0},
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  Settings / System                                              ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Settings/System",
        widget_type="button", name="reboot_system",
        action="skip",
        match={"type": "btn", "text_contains": "REBOOT", "y_min": 100},
        expect={"reason": "reboots the device"},
        dangerous=True,
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  App: Map                                                       ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Map",
        widget_type="button", name="map_control_1",
        action="test_button",
        match={"type": "btn", "index": 0, "y_min": 80, "y_max": 430},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Map",
        widget_type="button", name="map_control_2",
        action="test_button",
        match={"type": "btn", "index": 1, "y_min": 80, "y_max": 430},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Map",
        widget_type="button", name="map_control_3",
        action="test_button",
        match={"type": "btn", "index": 2, "y_min": 80, "y_max": 430},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Map",
        widget_type="button", name="map_control_4",
        action="test_button",
        match={"type": "btn", "index": 3, "y_min": 80, "y_max": 430},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Map",
        widget_type="button", name="map_control_5",
        action="test_button",
        match={"type": "btn", "index": 4, "y_min": 80, "y_max": 430},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Map",
        widget_type="button", name="map_control_6",
        action="test_button",
        match={"type": "btn", "index": 5, "y_min": 80, "y_max": 430},
        expect={"max_shift_px": 2.0},
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  App: BLE                                                       ║
    # ╚══════════════════════════════════════════════════════════════════╝
    # Only nav buttons — no interactive controls

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  App: Monitor                                                   ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Monitor",
        widget_type="bar", name="monitor_heap_bar",
        action="readonly",
        match={"type": "bar", "index": 0},
        expect={"reason": "Heap usage percentage"},
    ),
    ElementSpec(
        screen="Monitor",
        widget_type="bar", name="monitor_psram_bar",
        action="readonly",
        match={"type": "bar", "index": 1},
        expect={"reason": "PSRAM usage percentage"},
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  App: Terminal                                                  ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Terminal",
        widget_type="button", name="terminal_send",
        action="test_button",
        match={"type": "btn", "index": 0, "y_min": 80, "y_max": 430},
        expect={"max_shift_px": 40.0},  # Known LVGL theme shift on some buttons
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  App: Files                                                     ║
    # ╚══════════════════════════════════════════════════════════════════╝
    ElementSpec(
        screen="Files",
        widget_type="button", name="files_browse",
        action="test_button",
        match={"type": "btn", "text_contains": "Browse", "y_min": 80},
        expect={"max_shift_px": 2.0},
    ),
    ElementSpec(
        screen="Files",
        widget_type="button", name="files_test",
        action="test_button",
        match={"type": "btn", "text_contains": "Test", "y_min": 80},
        expect={"max_shift_px": 40.0},  # Known LVGL theme -25,-25px shift
    ),
    ElementSpec(
        screen="Files",
        widget_type="button", name="files_eject",
        action="test_button",
        match={"type": "btn", "text_contains": "Eject", "y_min": 80},
        expect={"max_shift_px": 40.0},  # Known LVGL theme -25,-25px shift
    ),
    ElementSpec(
        screen="Files",
        widget_type="button", name="files_format_sd",
        action="skip",
        match={"type": "btn", "text_contains": "Format", "y_min": 80},
        expect={"reason": "formats the SD card"},
        dangerous=True,
    ),
    ElementSpec(
        screen="Files",
        widget_type="bar", name="files_sd_usage_1",
        action="readonly",
        match={"type": "bar", "index": 0},
        expect={"reason": "SD card usage indicator"},
    ),
    ElementSpec(
        screen="Files",
        widget_type="bar", name="files_sd_usage_2",
        action="readonly",
        match={"type": "bar", "index": 1},
        expect={"reason": "SD card usage indicator"},
    ),
    ElementSpec(
        screen="Files",
        widget_type="bar", name="files_sd_usage_3",
        action="readonly",
        match={"type": "bar", "index": 2},
        expect={"reason": "SD card usage indicator"},
    ),
    ElementSpec(
        screen="Files",
        widget_type="bar", name="files_sd_usage_4",
        action="readonly",
        match={"type": "bar", "index": 3},
        expect={"reason": "SD card usage indicator"},
    ),

    # ╔══════════════════════════════════════════════════════════════════╗
    # ║  App: About                                                     ║
    # ╚══════════════════════════════════════════════════════════════════╝
    # Only nav buttons and labels — no interactive controls
]


# ── Helpers ─────────────────────────────────────────────────────────────

def all_screens() -> list[str]:
    """Get ordered list of unique screens in the registry."""
    seen = set()
    screens = []
    for spec in ELEMENT_REGISTRY:
        if spec.screen not in seen:
            seen.add(spec.screen)
            screens.append(spec.screen)
    return screens


def elements_for_screen(screen: str) -> list[ElementSpec]:
    """Get all registered elements for a given screen."""
    return [s for s in ELEMENT_REGISTRY if s.screen == screen]


def match_widget(spec: ElementSpec, widgets: list[dict]) -> Optional[dict]:
    """Find the widget in a flat widget tree that matches this spec.

    Match rules (all must pass):
    - type:           widget type (without lv_ prefix)
    - text_contains:  widget text contains this substring
    - min/max:        slider/bar range values
    - index:          nth matching widget (after other filters)
    - y_min/y_max:    vertical position bounds
    """
    m = spec.match
    target_type = m.get("type", spec.widget_type)

    candidates = []
    for w in widgets:
        wtype = w.get("type", "").removeprefix("lv_")
        if wtype != target_type:
            continue

        # Text filter
        if "text_contains" in m:
            text = w.get("text", "")
            if m["text_contains"] not in text:
                continue

        # Range filters
        if "min" in m and w.get("min") != m["min"]:
            continue
        if "max" in m and w.get("max") != m["max"]:
            continue

        # Position filters
        wy = w.get("y", 0)
        if "y_min" in m and wy < m["y_min"]:
            continue
        if "y_max" in m and wy > m["y_max"]:
            continue

        candidates.append(w)

    if not candidates:
        return None

    # Index selection
    idx = m.get("index", 0)
    if idx < 0:
        idx = len(candidates) + idx
    if 0 <= idx < len(candidates):
        return candidates[idx]

    return candidates[0] if candidates else None
