#!/usr/bin/env python3
"""
Tritium-OS Visual UI Test
===========================
Automated UI testing with registry-driven element coverage,
randomized execution order, and OpenCV visual validation.

Every interactive element is declared in lib/registry.py with
its expected behavior. The test framework finds, exercises, and
validates each one. Unregistered elements trigger warnings.

Usage:
    python3 tools/ui_test_visual.py [device_ip]
    python3 tools/ui_test_visual.py 10.42.0.237 --soak 180   # 3 hour soak
    python3 tools/ui_test_visual.py --discover                 # find unregistered elements
"""

import argparse
import random
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
from lib.registry import (
    ELEMENT_REGISTRY, SETTINGS_TABS, ElementSpec,
    all_screens, elements_for_screen, match_widget,
)
from lib.actions import ACTION_MAP


# ── Navigation ──────────────────────────────────────────────────────────

def navigate_to(dev: TritiumDevice, screen: str) -> bool:
    """Navigate to a screen. Handles 'Settings/Display' or 'Map' etc.

    Returns True if navigation succeeded.
    """
    if "/" in screen:
        app_name, tab_name = screen.split("/", 1)

        result = dev.launch(app_name)
        time.sleep(1.5)
        if not result or not result.get("ok"):
            return False

        widgets = dev.ui_tree(flat=True)
        if not isinstance(widgets, list):
            return False

        buttons = extract_buttons(widgets)
        tab_btns = [b for b in buttons
                    if b.get("y", 999) < 80 and b.get("h", 0) > 30]

        # Try text-based match first (most reliable)
        for btn in tab_btns:
            if btn.get("text", "").strip() == tab_name:
                dev.click(btn["id"])
                time.sleep(1.2)
                return True

        # Fallback to index-based (for tabs without text labels)
        if tab_name in SETTINGS_TABS:
            tab_idx = SETTINGS_TABS.index(tab_name)
            if tab_idx < len(tab_btns):
                dev.click(tab_btns[tab_idx]["id"])
                time.sleep(1.2)
                return True
        return False
    else:
        result = dev.launch(screen)
        time.sleep(1.5)
        return bool(result and result.get("ok"))


# ── Registry-driven element sweep ──────────────────────────────────────

def _switch_settings_tab(dev: TritiumDevice, tab_name: str,
                         tab_btns: list) -> bool:
    """Click a settings tab button by index. tab_btns must already be populated."""
    if tab_name not in SETTINGS_TABS:
        return False
    tab_idx = SETTINGS_TABS.index(tab_name)
    if tab_idx >= len(tab_btns):
        return False
    dev.click(tab_btns[tab_idx]["id"])
    time.sleep(1.2)
    return True


def test_element_sweep(dev: TritiumDevice, report: TestReport,
                       vis: VisualValidator, randomize: bool = False):
    """Exercise every registered UI element.

    Navigates to each screen, finds all registered elements via the
    widget tree, and dispatches to the appropriate test action.
    Warns about unregistered interactive widgets.
    """
    print("\n--- Element Sweep (Registry-Driven) ---")

    screens = all_screens()
    if randomize:
        random.shuffle(screens)

    # Group settings screens vs standalone app screens
    settings_screens = [s for s in screens if s.startswith("Settings/")]
    app_screens = [s for s in screens if not s.startswith("Settings/")]

    total = 0
    passed_count = 0
    not_found = 0

    # --- Settings screens: launch once, switch tabs ---
    if settings_screens:
        settings_ok = False
        for attempt in range(2):
            dev.home()
            time.sleep(1.0)
            result = dev.launch("Settings")
            time.sleep(2.0)
            if result and result.get("ok"):
                # Verify UI actually loaded
                widgets = dev.ui_tree(flat=True)
                if isinstance(widgets, list) and len(widgets) > 10:
                    settings_ok = True
                    break
            time.sleep(2.0)

        if not settings_ok:
            for screen in settings_screens:
                specs = elements_for_screen(screen)
                for spec in specs:
                    report.add("elements", f"{screen}/{spec.name}", False,
                               "Skipped: Settings launch failed")
                    total += 1
        else:
            # Extract tab buttons from already-fetched widget tree
            buttons = extract_buttons(widgets)
            tab_btns = [b for b in buttons
                        if b.get("y", 999) < 80 and b.get("h", 0) > 30]
            if len(tab_btns) < len(SETTINGS_TABS):
                print(f"  [WARN] Only {len(tab_btns)} tab buttons"
                      f" found (need {len(SETTINGS_TABS)})")

            for screen in settings_screens:
                tab_name = screen.split("/", 1)[1]
                specs = elements_for_screen(screen)
                if randomize:
                    random.shuffle(specs)

                print(f"\n  Screen: {screen} ({len(specs)} elements)")

                if not _switch_settings_tab(dev, tab_name, tab_btns):
                    report.add("elements", f"{screen}_nav", False,
                               f"Failed to switch to tab {tab_name}")
                    for spec in specs:
                        report.add("elements", f"{screen}/{spec.name}", False,
                                   "Skipped: tab switch failed")
                        total += 1
                    continue

                # Get fresh widget tree for this tab
                tab_widgets = dev.ui_tree(flat=True)
                if not isinstance(tab_widgets, list):
                    for spec in specs:
                        report.add("elements", f"{screen}/{spec.name}", False,
                                   "Skipped: widget tree failed")
                        total += 1
                    continue

                for spec in specs:
                    widget = match_widget(spec, tab_widgets)

                    if widget is None:
                        detail = f"{spec.name}: not found (match={spec.match})"
                        report.add("elements", f"{screen}/{spec.name}", False,
                                   detail)
                        print(f"  [MISS] {screen}/{spec.name}: not found")
                        not_found += 1
                        total += 1
                        continue

                    action_fn = ACTION_MAP.get(spec.action)
                    if action_fn is None:
                        report.add_warning(
                            f"Unknown action '{spec.action}' for {spec.name}")
                        total += 1
                        continue

                    try:
                        action_fn(dev, vis, report, spec, widget, screen)
                    except Exception as e:
                        report.add("elements", f"{screen}/{spec.name}", False,
                                   f"Error: {e}")
                        print(f"  [ERR ] {screen}/{spec.name}: {e}")

                    total += 1
                    time.sleep(1.0)

                _discover_unregistered(dev, report, screen, tab_widgets, specs)

        dev.home()
        time.sleep(0.8)

    # --- Standalone app screens ---
    for screen in app_screens:
        specs = elements_for_screen(screen)
        if randomize:
            random.shuffle(specs)

        print(f"\n  Screen: {screen} ({len(specs)} elements)")

        dev.home()
        time.sleep(0.5)

        if not navigate_to(dev, screen):
            report.add("elements", f"{screen}_nav", False,
                       f"Failed to navigate to {screen}")
            for spec in specs:
                report.add("elements", f"{screen}/{spec.name}", False,
                           f"Skipped: navigation failed")
                total += 1
            dev.home()
            time.sleep(0.8)
            continue

        widgets = dev.ui_tree(flat=True)
        if not isinstance(widgets, list):
            report.add("elements", f"{screen}_tree", False,
                       "Failed to get widget tree")
            dev.home()
            time.sleep(0.8)
            continue

        for spec in specs:
            widget = match_widget(spec, widgets)

            if widget is None:
                detail = f"{spec.name}: not found (match={spec.match})"
                report.add("elements", f"{screen}/{spec.name}", False, detail)
                print(f"  [MISS] {screen}/{spec.name}: not found")
                not_found += 1
                total += 1
                continue

            action_fn = ACTION_MAP.get(spec.action)
            if action_fn is None:
                report.add_warning(
                    f"Unknown action '{spec.action}' for {spec.name}")
                total += 1
                continue

            try:
                action_fn(dev, vis, report, spec, widget, screen)
            except Exception as e:
                report.add("elements", f"{screen}/{spec.name}", False,
                           f"Error: {e}")
                print(f"  [ERR ] {screen}/{spec.name}: {e}")

            total += 1
            time.sleep(1.0)

        _discover_unregistered(dev, report, screen, widgets, specs)

        dev.home()
        time.sleep(0.8)

    # Count passed from report
    passed_count = sum(1 for r in report.results
                       if r.category == "elements" and r.passed
                       and r.run_number == report._run_number)
    total_this_run = sum(1 for r in report.results
                         if r.category == "elements"
                         and r.run_number == report._run_number)

    report.add("coverage", "element_coverage",
               not_found == 0,
               f"{passed_count}/{total_this_run} passed, {not_found} not found")


def _discover_unregistered(dev: TritiumDevice, report: TestReport,
                           screen: str, widgets: list, specs: list):
    """Warn about interactive widgets that aren't in the registry."""
    # Find which widgets the registry matched
    matched_ids = set()
    for spec in specs:
        w = match_widget(spec, widgets)
        if w:
            matched_ids.add(w.get("id"))

    # Find unregistered interactive widgets
    for w in widgets:
        if w.get("id") in matched_ids:
            continue
        wtype = w.get("type", "").removeprefix("lv_")
        if wtype not in ("btn", "slider", "switch", "dropdown", "checkbox"):
            continue
        # Skip nav buttons (bottom bar, y >= 400 with no text)
        if wtype == "btn" and w.get("y", 0) >= 400:
            if not w.get("text", "").strip():
                continue
        # Skip tab buttons (top bar, y < 80)
        if wtype == "btn" and w.get("y", 999) < 80:
            continue
        if not w.get("clickable", False):
            continue

        text = w.get("text", "").strip()
        report.add_warning(
            f"Unregistered {wtype} on {screen}: "
            f"text='{text}' y={w.get('y', 0)} id={w.get('id', '')}")


# ── System-level tests (not element-specific) ──────────────────────────

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
    """Check that the current screen is visually stable (no flicker)."""
    print("\n--- Visual Stability ---")

    diff = vis.check_screen_stability(dev, duration_s=2.0, interval_s=0.5)
    report.add("visual", "screen_stable",
               not diff.changed,
               diff.description,
               visual_diff_pct=diff.diff_pct)


def test_app_launch(dev: TritiumDevice, report: TestReport, vis: VisualValidator,
                    randomize: bool = False):
    """Launch each app, verify it renders distinct content."""
    print("\n--- App Launch ---")

    apps_data = dev.apps()
    if "_error" in apps_data:
        report.add("apps", "list_apps", False, str(apps_data.get("_error", "")))
        return

    apps = apps_data.get("apps", [])
    available = [a for a in apps if a.get("available", True)]
    unavailable = [a for a in apps if not a.get("available", True)]

    report.add("apps", "list_apps", len(available) > 0,
               f"{len(available)} available, {len(unavailable)} unavailable")

    for app in unavailable:
        report.add_warning(f"App '{app['name']}' — unavailable (not compiled)")

    if randomize:
        random.shuffle(available)

    launcher_img = dev.screenshot_np()
    if launcher_img is not None:
        vis.save(launcher_img, "launcher_baseline")

    for app in available:
        name = app.get("name", "unknown")
        t0 = time.time()
        result = dev.launch(name)
        time.sleep(1.2)
        dt = (time.time() - t0) * 1000

        launched = result.get("ok", False) if result else False
        widgets = dev.ui_tree(flat=True)
        wcount = len(widgets) if isinstance(widgets, list) else 0

        app_img = dev.screenshot_np()
        diff_pct = 0.0
        if app_img is not None:
            vis.save(app_img, f"app_{name}")
            if launcher_img is not None:
                diff = vis.compare(launcher_img, app_img)
                diff_pct = diff.diff_pct

        report.add("apps", f"launch_{name}",
                   launched and wcount > 0,
                   f"{wcount} widgets, {dt:.0f}ms",
                   duration_ms=dt, visual_diff_pct=diff_pct)

    dev.home()
    time.sleep(0.8)


def test_settings_tabs(dev: TritiumDevice, report: TestReport, vis: VisualValidator,
                       randomize: bool = False):
    """Verify each settings tab loads distinct content."""
    print("\n--- Settings Tabs ---")

    result = dev.launch("Settings")
    time.sleep(1.5)
    if not result or not result.get("ok"):
        report.add("settings", "launch", False, "Failed to launch Settings")
        return

    widgets = dev.ui_tree(flat=True)
    if not isinstance(widgets, list):
        return

    buttons = extract_buttons(widgets)
    tab_btns = [b for b in buttons
                if b.get("y", 999) < 80 and b.get("h", 0) > 30]

    report.add("settings", "tab_buttons_found",
               len(tab_btns) >= 11,
               f"Found {len(tab_btns)} tab buttons (expected 11)")

    tab_order = list(range(len(SETTINGS_TABS)))
    if randomize:
        random.shuffle(tab_order)

    prev_img = None
    for idx in tab_order:
        tab_name = SETTINGS_TABS[idx]
        if idx >= len(tab_btns):
            continue

        t0 = time.time()
        dev.click(tab_btns[idx]["id"])
        time.sleep(1.2)
        dt = (time.time() - t0) * 1000

        tab_img = dev.screenshot_np()
        tab_widgets = dev.ui_tree(flat=True)
        wcount = len(tab_widgets) if isinstance(tab_widgets, list) else 0

        diff_pct = 0.0
        if tab_img is not None and prev_img is not None:
            diff = vis.compare(prev_img, tab_img)
            diff_pct = diff.diff_pct
        if tab_img is not None:
            prev_img = tab_img.copy()

        report.add("settings", f"tab_{tab_name}",
                   wcount > 3,
                   f"{wcount} widgets, {dt:.0f}ms",
                   duration_ms=dt, visual_diff_pct=diff_pct)

    dev.home()
    time.sleep(0.8)


def test_map_content(dev: TritiumDevice, report: TestReport, vis: VisualValidator):
    """Verify map renders tile content."""
    print("\n--- Map Content ---")

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
    nonblack = (img.sum(axis=2) > 30).sum()
    total_px = img.shape[0] * img.shape[1]
    content_pct = nonblack / total_px * 100

    report.add("map", "has_content",
               content_pct > 15,
               f"{content_pct:.1f}% non-dark pixels")

    dev.home()
    time.sleep(0.5)


def test_stability(dev: TritiumDevice, report: TestReport, vis: VisualValidator,
                   cycles: int = 3, randomize: bool = False):
    """App switching stability — verify no crashes under repeated navigation."""
    print("\n--- Stability ---")

    apps_data = dev.apps()
    apps = apps_data.get("apps", []) if "_error" not in apps_data else []
    app_names = [a["name"] for a in apps if a.get("available", True)]

    if randomize:
        random.shuffle(app_names)

    errors = 0
    start = time.time()

    for cycle in range(cycles):
        order = list(app_names)
        if randomize:
            random.shuffle(order)
        for name in order:
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
    total_ops = cycles * len(app_names) * 2

    report.add("stability", "app_switching",
               errors == 0,
               f"{cycles} cycles, {errors} errors, "
               f"{total_ops / elapsed:.1f} ops/s" if elapsed > 0 else "0 ops/s")

    # Memory check
    time.sleep(1.0)
    try:
        status = dev.info()
        heap = status.get("free_heap", 0)
        psram = status.get("psram_free", 0)
        report.record_memory(heap, psram)
        report.add("stability", "post_stress_memory",
                   heap > 30_000,
                   f"heap={heap / 1024:.0f}KB psram={psram / 1024:.0f}KB")
    except Exception:
        pass


# ── Sweep orchestration ────────────────────────────────────────────────

def run_single_sweep(dev: TritiumDevice, report: TestReport,
                     vis: VisualValidator, randomize: bool = False):
    """Run one full sweep of all test suites."""

    # System tests (always run first)
    test_performance(dev, report, vis)
    test_visual_stability(dev, report, vis)

    # Ordering of remaining suites can be randomized
    suites = [
        ("app_launch", lambda: test_app_launch(dev, report, vis, randomize)),
        ("settings_tabs", lambda: test_settings_tabs(dev, report, vis, randomize)),
        ("element_sweep", lambda: test_element_sweep(dev, report, vis, randomize)),
        ("map_content", lambda: test_map_content(dev, report, vis)),
        ("stability", lambda: test_stability(dev, report, vis, randomize=randomize)),
    ]

    if randomize:
        random.shuffle(suites)

    for name, fn in suites:
        try:
            fn()
        except Exception as e:
            report.add("error", f"suite_{name}", False, str(e))
            traceback.print_exc()
            # Ensure we're back at home after any error
            try:
                dev.home()
                time.sleep(0.5)
            except Exception:
                pass


# ── Soak test ───────────────────────────────────────────────────────────

def run_soak(dev: TritiumDevice, report: TestReport, vis: VisualValidator,
             duration_min: float):
    """Run the full test suite repeatedly with randomized ordering."""
    print(f"\n{'=' * 72}")
    print(f"SOAK TEST — {duration_min:.0f} minutes ({duration_min / 60:.1f} hours)")
    print(f"{'=' * 72}")

    start = time.time()
    end_time = start + duration_min * 60
    run = 0

    while time.time() < end_time:
        run += 1
        elapsed_min = (time.time() - start) / 60
        remaining_min = duration_min - elapsed_min
        print(f"\n{'─' * 72}")
        print(f"SOAK RUN {run} — {elapsed_min:.1f}min elapsed, "
              f"{remaining_min:.1f}min remaining (seed={run})")
        print(f"{'─' * 72}")

        # Seed RNG per run for reproducibility
        random.seed(run)

        report.soak.total_runs = run
        report.set_run_number(run)

        try:
            if not dev.is_reachable():
                print(f"  Device unreachable, waiting up to 30s...")
                for retry in range(6):
                    time.sleep(5)
                    if dev.is_reachable():
                        print(f"  Device back online after {(retry + 1) * 5}s")
                        break
                else:
                    report.add("soak", f"run_{run}_error", False,
                               "Device unreachable for 30s")
                    continue

            run_single_sweep(dev, report, vis, randomize=True)
        except Exception as e:
            report.add("soak", f"run_{run}_error", False, str(e))
            traceback.print_exc()

        # Memory snapshot
        try:
            status = dev.info()
            report.record_memory(
                status.get("free_heap", 0),
                status.get("psram_free", 0))
        except Exception:
            pass

        # Periodic report (every 5 runs)
        if run % 5 == 0:
            report.generate()
            print(f"\n  [Intermediate report saved at run {run}]")

        if time.time() < end_time:
            time.sleep(2)


# ── Main ────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Tritium-OS Visual UI Test — registry-driven element coverage")
    parser.add_argument("host", nargs="?", default=None,
                        help="Device IP (auto-detect if omitted)")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--soak", type=float, default=0,
                        help="Soak test duration in minutes (0 = single run)")
    parser.add_argument("--output", default="test_report/visual",
                        help="Output directory")
    parser.add_argument("--discover", action="store_true",
                        help="Run in discovery mode — find unregistered elements")
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

    # Device info
    info = dev.info()
    board = info.get("board", "unknown") if "_error" not in info else "unknown"
    report.start_run(device_ip=host, device_board=board)

    print(f"\n{'=' * 72}")
    print(f"TRITIUM-OS STABILITY TEST")
    print(f"Device: {host}:{args.port} ({board})")
    print(f"Output: {output_dir}")
    print(f"Registry: {len(ELEMENT_REGISTRY)} elements across "
          f"{len(all_screens())} screens")
    print(f"Mode: {'soak ' + str(args.soak) + 'min' if args.soak else 'single sweep'}")
    print(f"{'=' * 72}")

    # Verify connectivity
    connected = False
    for attempt in range(6):
        if test_connectivity(dev, report):
            connected = True
            break
        print(f"  Retry {attempt + 1}/6 — waiting 5s...")
        time.sleep(5)

    if not connected:
        print("\nFATAL: Device not reachable after 30s. Aborting.")
        report.generate()
        sys.exit(1)

    # Disable sleep/screensaver during testing
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
        if args.soak > 0:
            run_soak(dev, report, vis, args.soak)
        else:
            report.soak.total_runs = 1
            report.set_run_number(1)
            run_single_sweep(dev, report, vis, randomize=args.discover)

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
