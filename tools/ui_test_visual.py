#!/usr/bin/env python3
"""
Tritium-OS Visual UI Test
===========================
Automated UI testing with OpenCV-based visual validation.
Detects visual regressions like element movement, misalignment,
corruption, and layout instability.

Tests every interactive UI element: buttons, sliders, switches,
dropdowns, and tab controls across all apps and settings tabs.

Usage:
    python3 tools/ui_test_visual.py [device_ip]
    python3 tools/ui_test_visual.py 10.42.0.237 --soak 180   # 3 hour soak
    python3 tools/ui_test_visual.py --buttons-only             # Just test button movement
"""

import argparse
import sys
import time
import traceback
from datetime import datetime
from pathlib import Path

import cv2
import numpy as np

# Add tools/ to path for lib imports
sys.path.insert(0, str(Path(__file__).parent))

from lib.device import TritiumDevice, auto_detect
from lib.visual import VisualValidator, BBox, VisualReport
from lib.report import TestReport
from lib.widgets import (
    extract_by_type, extract_interactive, extract_buttons,
    extract_labels, extract_sliders, extract_switches, extract_values,
    extract_bars, extract_dropdowns,
)


# ── Test suites ──────────────────────────────────────────────────────────────

def test_connectivity(dev: TritiumDevice, report: TestReport):
    """Basic device connectivity."""
    print("\n--- Connectivity ---")

    t0 = time.time()
    info = dev.info()
    dt = (time.time() - t0) * 1000
    has_error = "_error" in info
    report.add("connectivity", "device_reachable",
               not has_error,
               f"w={info.get('width')} h={info.get('height')}" if not has_error
               else info.get("_error", ""),
               duration_ms=dt)

    if has_error:
        return False

    t0 = time.time()
    lvgl = dev.lvgl_debug()
    dt = (time.time() - t0) * 1000
    report.add("connectivity", "lvgl_running",
               lvgl.get("initialized", False),
               f"flush={lvgl.get('flush_count', 0)}",
               duration_ms=dt)

    wifi = dev.wifi_status()
    report.add("connectivity", "wifi_connected",
               wifi.get("connected", False),
               f"ssid={wifi.get('ssid', '?')} rssi={wifi.get('rssi', '?')}")

    return True


def test_performance(dev: TritiumDevice, report: TestReport, vis: VisualValidator):
    """System performance and resource metrics."""
    print("\n--- Performance ---")

    # Use /api/status for reliable memory readings
    status = dev._get("/api/status")
    heap_free = status.get("free_heap", 0) if status and "_error" not in status else 0
    psram_free = status.get("psram_free", 0) if status and "_error" not in status else 0
    if heap_free == 0:
        lvgl = dev.lvgl_debug()
        heap_free = lvgl.get("heap_free", 0)
        psram_free = lvgl.get("psram_free", 0)
    report.record_memory(heap_free, psram_free)

    report.add("performance", "psram_available",
               psram_free > 1_000_000,
               f"{psram_free / 1024:.0f} KB free")
    report.add("performance", "heap_available",
               heap_free > 50_000,
               f"{heap_free / 1024:.0f} KB free")

    # Screenshot latency
    t0 = time.time()
    img = dev.screenshot_np()
    dt = (time.time() - t0) * 1000
    report.add("performance", "screenshot_latency",
               img is not None and dt < 3000,
               f"{dt:.0f}ms, {'ok' if img is not None else 'FAILED'}",
               duration_ms=dt)

    if img is not None:
        vis.save(img, "perf_screenshot")

    # UI tree latency
    t0 = time.time()
    tree = dev.ui_tree(flat=True)
    dt = (time.time() - t0) * 1000
    wcount = len(tree) if isinstance(tree, list) else 0
    report.add("performance", "ui_tree_latency",
               wcount > 0 and dt < 2000,
               f"{dt:.0f}ms, {wcount} widgets",
               duration_ms=dt)


def test_visual_stability(dev: TritiumDevice, report: TestReport, vis: VisualValidator):
    """Check that the current screen is visually stable (no flicker/corruption)."""
    print("\n--- Visual Stability ---")

    diff = vis.check_screen_stability(dev, duration_s=2.0, interval_s=0.5)
    report.add("visual", "screen_stable",
               not diff.changed,
               diff.description,
               visual_diff_pct=diff.diff_pct)


def _test_button_movement(dev: TritiumDevice, report: TestReport, vis: VisualValidator,
                           buttons: list, context: str = "", app_name: str = ""):
    """Test a set of buttons for press movement. Shared by multiple test suites."""
    moved_buttons = []
    tested = 0

    for btn in buttons:
        bx = btn.get("x", 0) + btn.get("w", 0) // 2
        by = btn.get("y", 0) + btn.get("h", 0) // 2
        bw = btn.get("w", 40)
        bh = btn.get("h", 30)
        label = btn.get("text", btn.get("id", "")[-8:])

        margin = 4
        roi = BBox(
            max(0, btn.get("x", 0) - margin),
            max(0, btn.get("y", 0) - margin),
            bw + margin * 2,
            bh + margin * 2
        )

        check = vis.check_button_press_movement(dev, bx, by, label, roi)
        tested += 1

        passed = not check.moved
        report.add_element(app_name or context, "button", btn.get("id", ""),
                          label, "press_hold", "stable" if passed else "moved",
                          passed, f"shift=({check.dx:+.1f},{check.dy:+.1f})px")

        if check.moved:
            moved_buttons.append(check)
            print(f"    MOVED: '{label}' shifted ({check.dx:+.1f}, {check.dy:+.1f})px")

        time.sleep(0.15)

    return tested, moved_buttons


def test_button_press_movement(dev: TritiumDevice, report: TestReport,
                                vis: VisualValidator):
    """Test ALL buttons for visual movement on press."""
    print("\n--- Button Press Movement ---")

    widgets = dev.ui_tree(flat=True)
    if not isinstance(widgets, list):
        report.add("visual", "button_press_movement", False, "Failed to get widget tree")
        return

    buttons = extract_buttons(widgets)
    clickable_buttons = [b for b in buttons if b.get("clickable")]

    if not clickable_buttons:
        report.add("visual", "button_press_movement", True,
                   "No clickable buttons on current screen")
        return

    print(f"  Testing {len(clickable_buttons)} buttons for press movement...")

    tested, moved = _test_button_movement(dev, report, vis, clickable_buttons,
                                           context="launcher")

    all_passed = len(moved) == 0
    report.add("visual", "button_press_movement",
               all_passed,
               f"{tested} tested, {len(moved)} moved",
               positions_moved=len(moved))


def test_all_interactive_elements(dev: TritiumDevice, report: TestReport,
                                   vis: VisualValidator):
    """Sweep every interactive element across all available apps.

    For each app:
    - Buttons: press/hold movement check
    - Sliders: visual stability during value change
    - Switches: toggle and verify visual change
    - Dropdowns: open/close stability
    """
    print("\n--- Interactive Element Sweep ---")

    apps_data = dev.apps()
    if "_error" in apps_data:
        report.add("elements", "get_apps", False, str(apps_data.get("_error", "")))
        return

    apps = [a for a in apps_data.get("apps", []) if a.get("available", True)]
    unavailable = [a for a in apps_data.get("apps", []) if not a.get("available", True)]

    if unavailable:
        for app in unavailable:
            report.add_warning(f"App '{app['name']}' — unavailable (not compiled)")

    total_elements = 0
    total_passed = 0

    for app in apps:
        name = app.get("name", "unknown")
        print(f"\n  App: {name}")

        result = dev.launch(name)
        time.sleep(1.2)

        if not result or not result.get("ok"):
            report.add("elements", f"{name}_launch", False, "Failed to launch")
            dev.home()
            time.sleep(0.5)
            continue

        widgets = dev.ui_tree(flat=True)
        if not isinstance(widgets, list):
            report.add("elements", f"{name}_tree", False, "Failed to get widget tree")
            dev.home()
            time.sleep(0.5)
            continue

        # --- Buttons ---
        buttons = [b for b in extract_buttons(widgets) if b.get("clickable")]
        # Only test content buttons (skip tab bar)
        content_buttons = [b for b in buttons if b.get("y", 0) >= 80]
        if content_buttons:
            tested, moved = _test_button_movement(dev, report, vis,
                                                   content_buttons[:8],
                                                   app_name=name)
            total_elements += tested
            total_passed += (tested - len(moved))
            report.add("elements", f"{name}_buttons",
                       len(moved) == 0,
                       f"{tested} buttons, {len(moved)} moved",
                       positions_moved=len(moved))

        # --- Sliders (small safe delta, immediate restore) ---
        sliders = extract_sliders(widgets)
        for sl in sliders[:4]:
            sl_id = sl.get("id", "")
            sl_val = sl.get("value", 50)
            sl_min = sl.get("min", 0)
            sl_max = sl.get("max", 100)
            sl_text = sl.get("text", sl_id[-8:])

            before = dev.screenshot_np()
            time.sleep(0.2)

            delta = max(1, (sl_max - sl_min) // 10)
            new_val = min(sl_max, sl_val + delta) if sl_val < sl_max - delta else max(sl_min, sl_val - delta)
            dev.set_value(sl_id, value=new_val)
            time.sleep(0.5)

            after = dev.screenshot_np()

            # IMMEDIATELY restore
            dev.set_value(sl_id, value=sl_val)
            time.sleep(0.3)

            passed = True
            detail = f"value {sl_val}→{new_val}→{sl_val}"
            if before is not None and after is not None:
                diff = vis.compare(before, after)
                if diff.diff_pct < 0.1:
                    detail += " (no visual change)"
                    passed = False
                else:
                    detail += f" (diff={diff.diff_pct:.1f}%)"

            report.add_element(name, "slider", sl_id, sl_text, "set_value",
                              "changed" if passed else "failed", passed, detail)
            total_elements += 1
            if passed:
                total_passed += 1

        # --- Switches (skip in Settings — handled by settings_interactive) ---
        if name != "Settings":
            switches = extract_switches(widgets)
            for sw in switches[:4]:
                sw_id = sw.get("id", "")
                sw_text = sw.get("text", sw_id[-8:])
                checked = sw.get("checked", False)

                before = dev.screenshot_np()
                dev.click(sw_id)
                time.sleep(0.5)
                after = dev.screenshot_np()

                passed = True
                detail = f"{'on→off' if checked else 'off→on'}"
                if before is not None and after is not None:
                    diff = vis.compare(before, after)
                    if diff.diff_pct < 0.05:
                        detail += " (no visual change)"
                        passed = False
                    else:
                        detail += f" (diff={diff.diff_pct:.1f}%)"

                report.add_element(name, "switch", sw_id, sw_text, "toggle",
                                  "toggled" if passed else "stuck", passed, detail)
                total_elements += 1
                if passed:
                    total_passed += 1

                dev.click(sw_id)
                time.sleep(0.3)

        # --- Dropdowns ---
        dropdowns = extract_dropdowns(widgets)
        for dd in dropdowns[:3]:
            dd_id = dd.get("id", "")
            dd_text = dd.get("selected_text", dd_id[-8:])

            before = dev.screenshot_np()

            # Open dropdown
            dev.click(dd_id)
            time.sleep(0.5)

            opened = dev.screenshot_np()

            passed = True
            detail = "open/close"
            if before is not None and opened is not None:
                diff = vis.compare(before, opened)
                if diff.diff_pct < 0.5:
                    detail += " (dropdown didn't open)"
                    passed = False
                else:
                    detail += f" (opened: diff={diff.diff_pct:.1f}%)"

            # Close by clicking elsewhere
            dev.tap(400, 100)
            time.sleep(0.3)

            report.add_element(name, "dropdown", dd_id, dd_text, "open_close",
                              "ok" if passed else "stuck", passed, detail)
            total_elements += 1
            if passed:
                total_passed += 1

        dev.home()
        time.sleep(0.5)

    report.add("elements", "total_coverage",
               total_passed == total_elements,
               f"{total_passed}/{total_elements} elements passed")


def test_app_launch_visual(dev: TritiumDevice, report: TestReport,
                            vis: VisualValidator):
    """Launch each app, take screenshot, verify visual integrity."""
    print("\n--- App Launch (Visual) ---")

    apps_data = dev.apps()
    if "_error" in apps_data:
        report.add("apps", "list_apps", False, str(apps_data.get("_error", "")))
        return

    apps = apps_data.get("apps", [])
    available = [a for a in apps if a.get("available", True)]
    unavailable = [a for a in apps if not a.get("available", True)]

    report.add("apps", "list_apps", len(available) > 0,
               f"{len(available)} available, {len(unavailable)} unavailable "
               f"({', '.join(a['name'] for a in unavailable) if unavailable else 'none'})")

    if unavailable:
        for app in unavailable:
            report.add_warning(f"App '{app['name']}' ({app.get('description', '')}) "
                               f"— unavailable (backing service not compiled)")

    apps = available

    launcher_img = dev.screenshot_np()
    if launcher_img is not None:
        vis.save(launcher_img, "launcher_baseline")

    for app in apps:
        name = app.get("name", "unknown")
        t0 = time.time()
        result = dev.launch(name)
        time.sleep(1.0)
        dt = (time.time() - t0) * 1000

        launched = result.get("ok", False) if result else False

        widgets = dev.ui_tree(flat=True)
        wcount = len(widgets) if isinstance(widgets, list) else 0

        app_img = dev.screenshot_np()
        ss_path = ""
        diff_pct = 0.0

        if app_img is not None:
            ss_path = vis.save(app_img, f"app_{name}")

            if launcher_img is not None:
                diff = vis.compare(launcher_img, app_img)
                diff_pct = diff.diff_pct
                if diff.diff_pct < 1.0:
                    report.add_warning(f"App '{name}' looks identical to launcher "
                                       f"(only {diff.diff_pct:.2f}% different)")

        report.add("apps", f"launch_{name}",
                   launched and wcount > 0,
                   f"{wcount} widgets, {dt:.0f}ms",
                   widgets=widgets if isinstance(widgets, list) else [],
                   duration_ms=dt,
                   visual_diff_pct=diff_pct,
                   screenshot_path=ss_path)

    dev.home()
    time.sleep(0.8)


def test_settings_tabs_visual(dev: TritiumDevice, report: TestReport,
                               vis: VisualValidator):
    """Test settings tab switching with visual validation.

    Phase 1: Click each tab, screenshot, verify content changes.
    Phase 2: Re-enter settings, click each tab, test content buttons
             for press movement (separate pass to avoid state corruption).
    """
    print("\n--- Settings Tabs (Visual) ---")

    # --- Phase 1: Tab switching ---
    result = dev.launch("Settings")
    time.sleep(1.2)

    if not result or not result.get("ok"):
        report.add("settings", "launch", False, "Failed to launch Settings")
        return

    widgets = dev.ui_tree(flat=True)
    if not isinstance(widgets, list):
        report.add("settings", "initial_tree", False, "Failed to get widget tree")
        return

    buttons = extract_buttons(widgets)
    tab_buttons = [b for b in buttons if b.get("y", 999) < 80 and b.get("h", 0) > 30]

    tab_names = ["Display", "WiFi", "BLE", "Mesh", "Monitor",
                 "Storage", "Tracking", "Power", "Screensaver",
                 "Security", "System"]

    report.add("settings", "tab_buttons_found",
               len(tab_buttons) >= 11,
               f"Found {len(tab_buttons)} tab buttons (expected 11)")

    prev_img = None

    for i, tab_name in enumerate(tab_names):
        if i >= len(tab_buttons):
            report.add("settings", f"tab_{tab_name}", False, "Tab button not found")
            continue

        btn = tab_buttons[i]
        t0 = time.time()
        click_result = dev.click(btn["id"])
        time.sleep(1.2)
        dt = (time.time() - t0) * 1000

        clicked = click_result and click_result.get("ok", False)

        tab_img = dev.screenshot_np()
        ss_path = ""
        diff_pct = 0.0

        if tab_img is not None:
            ss_path = vis.save(tab_img, f"settings_{tab_name}")
            if prev_img is not None:
                diff = vis.compare(prev_img, tab_img)
                diff_pct = diff.diff_pct
                if diff.diff_pct < 0.5:
                    report.add_warning(
                        f"Settings tab '{tab_name}' looks same as previous")
            prev_img = tab_img.copy()

        tab_widgets = dev.ui_tree(flat=True)
        wcount = len(tab_widgets) if isinstance(tab_widgets, list) else 0

        report.add("settings", f"tab_{tab_name}",
                   clicked and wcount > 3,
                   f"{wcount} widgets, {dt:.0f}ms",
                   widgets=tab_widgets if isinstance(tab_widgets, list) else [],
                   duration_ms=dt,
                   visual_diff_pct=diff_pct,
                   screenshot_path=ss_path)

        # Record tab elements for coverage
        if isinstance(tab_widgets, list):
            for w in tab_widgets:
                wtype = w.get("type", "").removeprefix("lv_")
                if wtype in ("btn", "button", "slider", "switch", "dropdown", "checkbox"):
                    report.add_element("Settings", wtype,
                                      w.get("id", ""), w.get("text", ""),
                                      "discovered", "present", True,
                                      f"tab={tab_name}")

    dev.home()
    time.sleep(0.8)

    # --- Phase 2: Button stability per tab ---
    print("\n--- Settings Button Stability ---")
    result = dev.launch("Settings")
    time.sleep(1.2)

    widgets = dev.ui_tree(flat=True)
    if not isinstance(widgets, list):
        return
    buttons = extract_buttons(widgets)
    tab_buttons = [b for b in buttons if b.get("y", 999) < 80 and b.get("h", 0) > 30]

    for i, tab_name in enumerate(tab_names):
        if i >= len(tab_buttons):
            continue

        dev.click(tab_buttons[i]["id"])
        time.sleep(1.2)

        tab_widgets = dev.ui_tree(flat=True)
        if not isinstance(tab_widgets, list):
            continue

        tab_btns = extract_buttons(tab_widgets)
        content_btns = [b for b in tab_btns
                        if b.get("clickable") and b.get("y", 0) >= 100]

        if not content_btns:
            report.add("settings", f"tab_{tab_name}_button_stability",
                       True, "No content buttons to test")
            continue

        tested, moved = _test_button_movement(dev, report, vis,
                                               content_btns[:5],
                                               app_name=f"Settings/{tab_name}")

        report.add("settings", f"tab_{tab_name}_button_stability",
                   len(moved) == 0,
                   f"{len(content_btns)} content buttons, {len(moved)} moved",
                   positions_moved=len(moved))

    dev.home()
    time.sleep(0.8)


def test_settings_interactive(dev: TritiumDevice, report: TestReport,
                                vis: VisualValidator):
    """Deep interactive test of settings sliders, switches, dropdowns."""
    print("\n--- Settings Interactive Elements ---")

    tab_names = ["Display", "WiFi", "BLE", "Mesh", "Monitor",
                 "Storage", "Tracking", "Power", "Screensaver",
                 "Security", "System"]

    result = dev.launch("Settings")
    time.sleep(1.2)
    if not result or not result.get("ok"):
        return

    widgets = dev.ui_tree(flat=True)
    if not isinstance(widgets, list):
        return
    buttons = extract_buttons(widgets)
    tab_buttons = [b for b in buttons if b.get("y", 999) < 80 and b.get("h", 0) > 30]

    for i, tab_name in enumerate(tab_names):
        if i >= len(tab_buttons):
            continue

        dev.click(tab_buttons[i]["id"])
        time.sleep(1.0)

        tab_widgets = dev.ui_tree(flat=True)
        if not isinstance(tab_widgets, list):
            continue

        # Sliders — small safe adjustment only, immediately restore
        # Power tab sliders are read-only status indicators (battery voltage, etc.)
        READONLY_SLIDER_TABS = {"Power"}
        sliders = extract_sliders(tab_widgets)
        for sl in sliders[:3]:
            sl_id = sl.get("id", "")
            sl_val = sl.get("value", 50)
            sl_min = sl.get("min", 0)
            sl_max = sl.get("max", 100)
            sl_text = sl.get("text", sl_id[-8:])

            if tab_name in READONLY_SLIDER_TABS:
                report.add_element(f"Settings/{tab_name}", "slider", sl_id,
                                  sl_text, "read_only",
                                  "ok_readonly", True,
                                  f"{tab_name}: {sl_val} (read-only indicator)")
                report.add("settings_interactive",
                           f"{tab_name}_slider_{sl_text}_readonly",
                           True, f"{tab_name}: {sl_val} (read-only indicator)")
                continue

            before = dev.screenshot_np()

            # Small delta only (10% of range) to avoid breaking display brightness
            delta = max(1, (sl_max - sl_min) // 10)
            new_val = min(sl_max, sl_val + delta) if sl_val < sl_max - delta else max(sl_min, sl_val - delta)
            dev.set_value(sl_id, value=new_val)
            time.sleep(0.5)

            after = dev.screenshot_np()

            # IMMEDIATELY restore before any analysis
            dev.set_value(sl_id, value=sl_val)
            time.sleep(0.3)

            passed = True
            detail = f"{tab_name}: {sl_val}→{new_val}→{sl_val}"
            if before is not None and after is not None:
                diff = vis.compare(before, after)
                if diff.diff_pct < 0.1:
                    passed = False
                    detail += " no_change"
                else:
                    detail += f" diff={diff.diff_pct:.1f}%"

            report.add_element(f"Settings/{tab_name}", "slider", sl_id,
                              sl_text, "set_value",
                              "ok" if passed else "no_change", passed, detail)
            report.add("settings_interactive", f"{tab_name}_slider_{sl_text}",
                       passed, detail)

        # Switches — skip WiFi/BLE/Mesh tabs (toggling network switches kills connectivity)
        SKIP_SWITCH_TABS = {"WiFi", "BLE", "Mesh"}
        switches = extract_switches(tab_widgets)
        if tab_name not in SKIP_SWITCH_TABS:
            for sw in switches[:3]:
                sw_id = sw.get("id", "")
                sw_text = sw.get("text", sw_id[-8:])
                checked = sw.get("checked", False)

                before = dev.screenshot_np()
                dev.click(sw_id)
                time.sleep(0.5)
                after = dev.screenshot_np()

                passed = True
                detail = f"{tab_name}: {'on→off' if checked else 'off→on'}"
                if before is not None and after is not None:
                    diff = vis.compare(before, after)
                    if diff.diff_pct < 0.05:
                        passed = False
                        detail += " stuck"
                    else:
                        detail += f" diff={diff.diff_pct:.1f}%"

                report.add_element(f"Settings/{tab_name}", "switch", sw_id,
                                  sw_text, "toggle",
                                  "ok" if passed else "stuck", passed, detail)
                report.add("settings_interactive", f"{tab_name}_switch_{sw_text}",
                           passed, detail)

                # Toggle back
                dev.click(sw_id)
                time.sleep(0.3)
        else:
            for sw in switches[:3]:
                sw_text = sw.get("text", sw.get("id", "")[-8:])
                report.add_element(f"Settings/{tab_name}", "switch",
                                  sw.get("id", ""), sw_text, "skipped",
                                  "skip_network", True,
                                  f"{tab_name}: skipped (network toggle)")
                report.add("settings_interactive",
                           f"{tab_name}_switch_{sw_text}_skipped",
                           True, f"Skipped (toggling would drop network)")

    dev.home()
    time.sleep(0.8)


def test_navigation_consistency(dev: TritiumDevice, report: TestReport,
                                 vis: VisualValidator):
    """Launch same app twice and verify screenshots are visually identical."""
    print("\n--- Navigation Consistency ---")

    apps_data = dev.apps()
    apps = apps_data.get("apps", []) if "_error" not in apps_data else []
    apps = [a for a in apps if a.get("available", True)]

    for app in apps[:4]:
        name = app.get("name", "?")

        dev.launch(name)
        time.sleep(1.0)
        img1 = dev.screenshot_np()
        dev.home()
        time.sleep(0.5)

        dev.launch(name)
        time.sleep(1.0)
        img2 = dev.screenshot_np()
        dev.home()
        time.sleep(0.5)

        if img1 is None or img2 is None:
            report.add("consistency", f"app_{name}",
                       False, "Screenshot capture failed")
            continue

        diff = vis.compare(img1, img2)

        report.add("consistency", f"app_{name}_identical",
                   diff.diff_pct < 5.0,
                   f"{diff.diff_pct:.2f}% different across 2 launches",
                   visual_diff_pct=diff.diff_pct)

        if diff.changed:
            vis.save_diff_visualization(img1, img2, diff, f"consistency_{name}")


def test_stability(dev: TritiumDevice, report: TestReport, vis: VisualValidator,
                   rapid_cycles: int = 3):
    """App switching stability test with memory monitoring.

    Uses gentler pacing (1s between ops) to avoid overwhelming the ESP32 httpd.
    """
    print("\n--- Stability ---")

    apps_data = dev.apps()
    apps = apps_data.get("apps", []) if "_error" not in apps_data else []
    app_names = [a.get("name", "") for a in apps if a.get("name") and a.get("available", True)]

    errors = 0
    start = time.time()

    for cycle in range(rapid_cycles):
        for name in app_names:
            result = dev.launch(name)
            if not result or "_error" in result:
                errors += 1
                time.sleep(1.0)
                continue
            time.sleep(1.0)
            tree = dev.ui_tree(flat=True)
            if not isinstance(tree, list):
                errors += 1
            time.sleep(0.5)
        dev.home()
        time.sleep(0.8)

    elapsed = time.time() - start
    total_ops = rapid_cycles * len(app_names) * 2

    report.add("stability", "app_switching",
               errors == 0,
               f"{rapid_cycles} cycles, {errors} errors, "
               f"{total_ops / elapsed:.1f} ops/s")

    # Memory check via /api/status (more reliable than lvgl_debug)
    time.sleep(1.0)
    status = dev._get("/api/status")
    heap = status.get("free_heap", 0) if status and "_error" not in status else 0
    psram = status.get("psram_free", 0) if status and "_error" not in status else 0
    if heap == 0:
        # Fallback to lvgl_debug
        lvgl = dev.lvgl_debug()
        heap = lvgl.get("heap_free", 0)
        psram = lvgl.get("psram_free", 0)
    report.record_memory(heap, psram)

    report.add("stability", "post_stress_memory",
               heap > 30_000,
               f"heap={heap/1024:.0f}KB psram={psram/1024:.0f}KB")


def test_map_app(dev: TritiumDevice, report: TestReport, vis: VisualValidator):
    """Test map app renders tiles (not empty)."""
    print("\n--- Map App ---")

    # Go home first to reset state
    dev.home()
    time.sleep(0.5)

    result = dev.launch("Map")
    time.sleep(2.0)

    if not result or not result.get("ok"):
        report.add("map", "launch", False, "Failed to launch Map")
        dev.home()
        return

    img = dev.screenshot_np()
    if img is None:
        report.add("map", "render", False, "Screenshot failed")
        dev.home()
        return

    vis.save(img, "map_view")

    # Check for meaningful content (map tiles should have varied colors)
    mean = img.mean()
    nonblack = (img.sum(axis=2) > 30).sum()
    total_px = img.shape[0] * img.shape[1]
    content_pct = nonblack / total_px * 100

    report.add("map", "has_content",
               content_pct > 15,
               f"{content_pct:.1f}% non-dark pixels, mean={mean:.1f}")

    # Map zoom buttons test (if available)
    widgets = dev.ui_tree(flat=True)
    if isinstance(widgets, list):
        buttons = extract_buttons(widgets)
        zoom_btns = [b for b in buttons if b.get("text", "") in ("+", "-", "＋", "−")]
        if zoom_btns:
            before = dev.screenshot_np()
            dev.click(zoom_btns[0]["id"])
            time.sleep(1.5)
            after = dev.screenshot_np()
            if before is not None and after is not None:
                diff = vis.compare(before, after)
                report.add("map", "zoom_responsive",
                           diff.diff_pct > 0.3,
                           f"diff={diff.diff_pct:.1f}% after zoom")
        report.add("map", "widgets_present", len(widgets) > 5,
                   f"{len(widgets)} widgets in map view")

    dev.home()
    time.sleep(0.5)


# ── Soak test ────────────────────────────────────────────────────────────────

def run_soak(dev: TritiumDevice, report: TestReport, vis: VisualValidator,
             duration_min: float):
    """Run the full test suite repeatedly for the specified duration."""
    print(f"\n{'='*72}")
    print(f"SOAK TEST — {duration_min:.0f} minutes ({duration_min/60:.1f} hours)")
    print(f"{'='*72}")

    start = time.time()
    end_time = start + duration_min * 60
    run = 0

    while time.time() < end_time:
        run += 1
        elapsed_min = (time.time() - start) / 60
        remaining_min = duration_min - elapsed_min
        print(f"\n{'─'*72}")
        print(f"SOAK RUN {run} — {elapsed_min:.1f}min elapsed, "
              f"{remaining_min:.1f}min remaining")
        print(f"{'─'*72}")

        report.soak.total_runs = run
        report.set_run_number(run)

        try:
            # Check connectivity before each run (device may sleep)
            if not dev.is_reachable():
                print(f"  Device unreachable, waiting up to 30s...")
                for retry in range(6):
                    time.sleep(5)
                    if dev.is_reachable():
                        print(f"  Device back online after {(retry+1)*5}s")
                        break
                else:
                    report.add("soak", f"run_{run}_error", False,
                               "Device unreachable for 30s")
                    continue

            run_single_sweep(dev, report, vis, prefix=f"soak{run}_")
        except Exception as e:
            report.add("soak", f"run_{run}_error", False, str(e))
            traceback.print_exc()

        # Memory snapshot between runs (use /api/status, more reliable than lvgl debug)
        try:
            status = dev.info()
            report.record_memory(status.get("free_heap", 0), status.get("psram_free", 0))
        except Exception:
            pass

        # Periodic report generation (every 5 runs)
        if run % 5 == 0:
            report.generate()
            print(f"\n  [Intermediate report saved at run {run}]")

        if time.time() < end_time:
            time.sleep(2)


def run_single_sweep(dev: TritiumDevice, report: TestReport,
                      vis: VisualValidator, prefix: str = ""):
    """Run one full sweep of all test suites."""
    test_performance(dev, report, vis)
    test_visual_stability(dev, report, vis)
    test_button_press_movement(dev, report, vis)
    test_app_launch_visual(dev, report, vis)
    test_all_interactive_elements(dev, report, vis)
    test_settings_tabs_visual(dev, report, vis)
    test_settings_interactive(dev, report, vis)
    test_map_app(dev, report, vis)
    test_navigation_consistency(dev, report, vis)
    test_stability(dev, report, vis)


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Tritium-OS Visual UI Test — OpenCV-based validation")
    parser.add_argument("host", nargs="?", default=None,
                        help="Device IP (auto-detect if omitted)")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--soak", type=float, default=0,
                        help="Soak test duration in minutes (0 = single run)")
    parser.add_argument("--output", default="test_report/visual",
                        help="Output directory")
    parser.add_argument("--buttons-only", action="store_true",
                        help="Only test button press movement")
    parser.add_argument("--threshold", type=float, default=2.0,
                        help="Position change threshold in pixels")
    args = parser.parse_args()

    # Find device
    host = args.host or auto_detect(args.port)
    if not host:
        print("ERROR: No device found. Specify IP.")
        sys.exit(1)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = f"{args.output}/{timestamp}"

    dev = TritiumDevice(host, args.port, timeout=10.0)
    vis = VisualValidator(
        output_dir=output_dir,
        position_threshold_px=args.threshold)
    report = TestReport(output_dir)

    # Get device info for report metadata
    info = dev.info()
    board = info.get("board", "unknown") if "_error" not in info else "unknown"
    report.start_run(device_ip=host, device_board=board)

    print(f"\n{'='*72}")
    print(f"TRITIUM-OS STABILITY TEST")
    print(f"Device: {host}:{args.port} ({board})")
    print(f"Output: {output_dir}")
    print(f"Mode: {'soak ' + str(args.soak) + 'min' if args.soak else 'single sweep'}")
    print(f"Database: {report.db_path}")
    print(f"{'='*72}")

    # Verify connectivity (retry up to 30s for device that may be sleeping)
    connected = False
    for attempt in range(6):
        if test_connectivity(dev, report):
            connected = True
            break
        print(f"  Retry {attempt+1}/6 — waiting 5s...")
        time.sleep(5)

    if not connected:
        print("\nFATAL: Device not reachable after 30s. Aborting.")
        report.generate()
        sys.exit(1)

    # Disable sleep/screensaver during testing to prevent WiFi drops
    try:
        import requests
        r = requests.put(f"http://{host}:{args.port}/api/settings",
                        json={"system": {"auto_sleep_s": 0},
                              "display": {"timeout_s": 0},
                              "screensaver": {"timeout_s": 0}},
                        timeout=5)
        if r.status_code == 200:
            print("  [Setup] Disabled sleep/screensaver for testing")
    except Exception:
        print("  [Setup] Warning: could not disable sleep")

    try:
        if args.buttons_only:
            dev.home()
            time.sleep(0.5)
            test_button_press_movement(dev, report, vis)
            test_settings_tabs_visual(dev, report, vis)

        elif args.soak > 0:
            run_soak(dev, report, vis, args.soak)

        else:
            report.soak.total_runs = 1
            report.set_run_number(1)
            run_single_sweep(dev, report, vis)

    except KeyboardInterrupt:
        print("\nInterrupted by user")
    except Exception as e:
        report.add("error", "unhandled", False, traceback.format_exc())

    # Final report
    text = report.generate()
    print(f"\n{text}")
    print(f"\nAPI stats: {dev.request_count} requests, {dev.error_count} errors")
    print(f"Output: {output_dir}")
    print(f"Report: {output_dir}/report.html")
    print(f"Database: {report.db_path}")

    sys.exit(0 if report.soak.total_failed == 0 else 1)


if __name__ == "__main__":
    main()
