#!/usr/bin/env python3
"""
Tritium-OS UI Test Sweep
========================
Comprehensive automated UI testing via REST API.
Connects to a Tritium-OS device, sweeps through every app, every settings tab,
every interactive widget — clicking buttons, reading values, taking screenshots,
and generating a full test report.

Usage:
    python3 tools/ui_test_sweep.py [device_ip] [--loops N] [--output DIR]

Requires: pip install requests pillow (pillow optional for screenshots)
"""

import argparse
import json
import os
import sys
import time
import traceback
from datetime import datetime
from io import BytesIO
from pathlib import Path

try:
    import requests
except ImportError:
    print("ERROR: pip install requests")
    sys.exit(1)

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False


class TritiumDevice:
    """REST API client for a Tritium-OS device."""

    def __init__(self, host: str, port: int = 80, timeout: float = 5.0):
        self.base = f"http://{host}:{port}"
        self.timeout = timeout
        self.request_count = 0
        self.error_count = 0
        self.session = requests.Session()

    def _get(self, path: str) -> dict | list | None:
        self.request_count += 1
        try:
            r = self.session.get(f"{self.base}{path}", timeout=self.timeout)
            r.raise_for_status()
            return r.json()
        except Exception as e:
            self.error_count += 1
            return {"_error": str(e)}

    def _post(self, path: str, data: dict) -> dict | None:
        self.request_count += 1
        try:
            r = self.session.post(f"{self.base}{path}", json=data, timeout=self.timeout)
            r.raise_for_status()
            return r.json()
        except Exception as e:
            self.error_count += 1
            return {"_error": str(e)}

    def _get_raw(self, path: str) -> bytes | None:
        self.request_count += 1
        try:
            r = self.session.get(f"{self.base}{path}", timeout=self.timeout)
            r.raise_for_status()
            return r.content
        except Exception:
            self.error_count += 1
            return None

    # --- High-level API ---

    def info(self) -> dict:
        return self._get("/api/remote/info")

    def apps(self) -> dict:
        return self._get("/api/shell/apps")

    def launch(self, name: str) -> dict:
        return self._post("/api/shell/launch", {"name": name})

    def home(self) -> dict:
        return self._post("/api/shell/home", {})

    def ui_tree(self, flat: bool = True) -> list | dict:
        path = "/api/ui/tree?flat=1" if flat else "/api/ui/tree"
        return self._get(path)

    def click(self, widget_id: str) -> dict:
        return self._post("/api/ui/click", {"id": widget_id})

    def set_value(self, widget_id: str, **kwargs) -> dict:
        return self._post("/api/ui/set", {"id": widget_id, **kwargs})

    def screenshot(self) -> bytes | None:
        return self._get_raw("/api/remote/screenshot")

    def lvgl_debug(self) -> dict:
        return self._get("/api/debug/lvgl")

    def wifi_status(self) -> dict:
        return self._get("/api/wifi/status")

    def ble_status(self) -> dict:
        return self._get("/api/ble")

    def diag(self) -> dict:
        return self._get("/api/diag")

    def diag_health(self) -> dict:
        return self._get("/api/diag/health")


class TestReport:
    """Collects test results and generates a report."""

    def __init__(self, output_dir: str):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.results = []
        self.screenshots = []
        self.start_time = time.time()
        self.warnings = []

    def add(self, category: str, name: str, passed: bool, detail: str = "",
            widgets: list = None):
        self.results.append({
            "category": category,
            "name": name,
            "passed": passed,
            "detail": detail,
            "timestamp": time.time(),
            "widget_count": len(widgets) if widgets else 0,
        })
        status = "PASS" if passed else "FAIL"
        print(f"  [{status}] {category}/{name}: {detail}")

    def add_warning(self, msg: str):
        self.warnings.append(msg)
        print(f"  [WARN] {msg}")

    def save_screenshot(self, name: str, data: bytes):
        if not data:
            return
        fname = f"{name}.bmp"
        path = self.output_dir / fname
        path.write_bytes(data)
        self.screenshots.append(str(path))

    def generate(self) -> str:
        elapsed = time.time() - self.start_time
        total = len(self.results)
        passed = sum(1 for r in self.results if r["passed"])
        failed = total - passed

        lines = []
        lines.append("=" * 72)
        lines.append("TRITIUM-OS UI TEST SWEEP REPORT")
        lines.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        lines.append(f"Duration: {elapsed:.1f}s")
        lines.append(f"Tests: {total} total, {passed} passed, {failed} failed")
        lines.append(f"Screenshots: {len(self.screenshots)}")
        if self.warnings:
            lines.append(f"Warnings: {len(self.warnings)}")
        lines.append("=" * 72)
        lines.append("")

        # Group by category
        categories = {}
        for r in self.results:
            cat = r["category"]
            if cat not in categories:
                categories[cat] = []
            categories[cat].append(r)

        for cat, tests in categories.items():
            cat_passed = sum(1 for t in tests if t["passed"])
            lines.append(f"## {cat} ({cat_passed}/{len(tests)})")
            for t in tests:
                status = "PASS" if t["passed"] else "FAIL"
                detail = f" — {t['detail']}" if t["detail"] else ""
                widgets = f" [{t['widget_count']} widgets]" if t["widget_count"] else ""
                lines.append(f"  [{status}] {t['name']}{detail}{widgets}")
            lines.append("")

        if self.warnings:
            lines.append("## WARNINGS")
            for w in self.warnings:
                lines.append(f"  - {w}")
            lines.append("")

        lines.append(f"Total API requests: varied")
        lines.append(f"Report saved to: {self.output_dir}")

        report = "\n".join(lines)

        # Save report
        report_path = self.output_dir / "report.txt"
        report_path.write_text(report)

        # Save JSON results
        json_path = self.output_dir / "results.json"
        json_path.write_text(json.dumps({
            "timestamp": datetime.now().isoformat(),
            "duration_s": elapsed,
            "total": total,
            "passed": passed,
            "failed": failed,
            "warnings": self.warnings,
            "results": self.results,
        }, indent=2))

        return report


def extract_interactive(widgets: list) -> list:
    """Filter to only interactive widgets from flat tree."""
    return [w for w in widgets if w.get("clickable")]


def extract_by_type(widgets: list, wtype: str) -> list:
    """Filter widgets by type name (matches with or without lv_ prefix)."""
    bare = wtype.removeprefix("lv_")
    return [w for w in widgets if w.get("type") in (wtype, bare, f"lv_{bare}")]


def _type_match(wtype: str, *names: str) -> bool:
    """Check if widget type matches any name (with or without lv_ prefix)."""
    bare = wtype.removeprefix("lv_")
    return bare in names or wtype in names


def extract_values(widgets: list) -> dict:
    """Extract all readable values from widget tree."""
    values = {}
    for w in widgets:
        wid = w.get("id", "")
        wtype = w.get("type", "")
        text = w.get("text", "")

        if _type_match(wtype, "label") and text:
            values[wid] = {"type": "label", "text": text}
        elif _type_match(wtype, "slider"):
            values[wid] = {"type": "slider", "value": w.get("value"),
                           "min": w.get("min"), "max": w.get("max")}
        elif _type_match(wtype, "bar"):
            values[wid] = {"type": "bar", "value": w.get("value"),
                           "min": w.get("min"), "max": w.get("max")}
        elif _type_match(wtype, "switch", "checkbox"):
            values[wid] = {"type": wtype.removeprefix("lv_"),
                           "checked": w.get("checked")}
        elif _type_match(wtype, "dropdown"):
            values[wid] = {"type": "dropdown",
                           "selected": w.get("selected"),
                           "selected_text": w.get("selected_text")}
        elif _type_match(wtype, "textarea"):
            values[wid] = {"type": "textarea", "value": w.get("value", "")}
        elif _type_match(wtype, "btn") and text:
            values[wid] = {"type": "button", "text": text}
        elif text:
            # Any widget with text is readable
            values[wid] = {"type": wtype, "text": text}
    return values


def test_connectivity(dev: TritiumDevice, report: TestReport):
    """Test basic device connectivity."""
    print("\n--- Connectivity ---")

    info = dev.info()
    has_error = "_error" in info
    report.add("connectivity", "device_reachable",
               not has_error,
               json.dumps(info) if has_error else
               f"w={info.get('width')} h={info.get('height')}")

    if has_error:
        return False

    lvgl = dev.lvgl_debug()
    report.add("connectivity", "lvgl_running",
               lvgl.get("initialized", False),
               f"flush_count={lvgl.get('flush_count', 0)}")

    wifi = dev.wifi_status()
    report.add("connectivity", "wifi_status",
               wifi.get("connected", False),
               f"ssid={wifi.get('ssid', '?')} rssi={wifi.get('rssi', '?')}")

    return True


def test_performance(dev: TritiumDevice, report: TestReport):
    """Test system performance metrics."""
    print("\n--- Performance ---")

    lvgl = dev.lvgl_debug()
    psram_free = lvgl.get("psram_free", 0)
    heap_free = lvgl.get("heap_free", 0)

    report.add("performance", "psram_available",
               psram_free > 1_000_000,
               f"{psram_free / 1024:.0f} KB free")

    report.add("performance", "heap_available",
               heap_free > 50_000,
               f"{heap_free / 1024:.0f} KB free")

    # Measure screenshot latency
    t0 = time.time()
    ss = dev.screenshot()
    latency = (time.time() - t0) * 1000
    report.add("performance", "screenshot_latency",
               ss is not None and latency < 3000,
               f"{latency:.0f}ms, {len(ss) if ss else 0} bytes")

    # Measure UI tree latency
    t0 = time.time()
    tree = dev.ui_tree(flat=True)
    latency = (time.time() - t0) * 1000
    widget_count = len(tree) if isinstance(tree, list) else 0
    report.add("performance", "ui_tree_latency",
               widget_count > 0 and latency < 2000,
               f"{latency:.0f}ms, {widget_count} widgets")


def test_app_launch(dev: TritiumDevice, report: TestReport):
    """Test launching each app and reading its widget tree."""
    print("\n--- App Launch ---")

    apps_data = dev.apps()
    if "_error" in apps_data:
        report.add("apps", "list_apps", False, apps_data["_error"])
        return

    apps = apps_data.get("apps", [])
    report.add("apps", "list_apps", len(apps) > 0, f"{len(apps)} apps registered")

    for app in apps:
        name = app.get("name", "unknown")
        result = dev.launch(name)
        time.sleep(1.0)  # Let LVGL render + mutex settle

        launched = result.get("ok", False) if result else False

        # Get widget tree (retry once if LVGL was busy)
        widgets = dev.ui_tree(flat=True)
        if isinstance(widgets, dict) and widgets.get("_error"):
            time.sleep(0.5)
            widgets = dev.ui_tree(flat=True)
        widget_count = len(widgets) if isinstance(widgets, list) else 0
        interactive = extract_interactive(widgets) if isinstance(widgets, list) else []

        report.add("apps", f"launch_{name}",
                   launched and widget_count > 0,
                   f"{widget_count} widgets, {len(interactive)} interactive",
                   widgets=widgets if isinstance(widgets, list) else [])

        # Screenshot
        ss = dev.screenshot()
        if ss:
            report.save_screenshot(f"app_{name}", ss)

    # Return home
    dev.home()
    time.sleep(0.8)


def test_settings_tabs(dev: TritiumDevice, report: TestReport):
    """Sweep through all 11 settings tabs, clicking each one and reading all values."""
    print("\n--- Settings Tabs ---")

    # Launch settings app
    result = dev.launch("Settings")
    time.sleep(1.0)

    if not result or not result.get("ok"):
        report.add("settings", "launch", False, "Failed to launch Settings")
        return

    # Get initial widget tree to find tab buttons
    widgets = dev.ui_tree(flat=True)
    if not isinstance(widgets, list):
        report.add("settings", "initial_tree", False, "Failed to get widget tree")
        return

    # Find tab buttons (lv_btn inside the tab bar area, typically near top)
    buttons = extract_by_type(widgets, "lv_btn")
    tab_buttons = [b for b in buttons if b.get("y", 999) < 80 and b.get("h", 0) > 30]

    tab_names = ["Display", "WiFi", "BLE", "Mesh", "Monitor",
                 "Storage", "Tracking", "Power", "Screensaver",
                 "Security", "System"]

    report.add("settings", "tab_buttons_found",
               len(tab_buttons) >= 11,
               f"Found {len(tab_buttons)} tab buttons (expected 11)")

    # Click each tab and inspect
    for i, tab_name in enumerate(tab_names):
        if i >= len(tab_buttons):
            report.add("settings", f"tab_{tab_name}", False, "Tab button not found")
            continue

        btn = tab_buttons[i]
        click_result = dev.click(btn["id"])
        time.sleep(0.8)

        clicked = click_result and click_result.get("ok", False)

        # Read the widget tree after clicking
        tab_widgets = dev.ui_tree(flat=True)
        if not isinstance(tab_widgets, list):
            report.add("settings", f"tab_{tab_name}", False, "Tree read failed after click")
            continue

        # Extract all values
        values = extract_values(tab_widgets)
        interactive = extract_interactive(tab_widgets)
        labels = extract_by_type(tab_widgets, "lv_label")
        sliders = extract_by_type(tab_widgets, "lv_slider")
        switches = extract_by_type(tab_widgets, "lv_switch")
        bars = extract_by_type(tab_widgets, "lv_bar")
        dropdowns = extract_by_type(tab_widgets, "lv_dropdown")

        detail_parts = []
        if labels:
            detail_parts.append(f"{len(labels)} labels")
        if sliders:
            detail_parts.append(f"{len(sliders)} sliders")
        if switches:
            detail_parts.append(f"{len(switches)} switches")
        if bars:
            detail_parts.append(f"{len(bars)} bars")
        if dropdowns:
            detail_parts.append(f"{len(dropdowns)} dropdowns")
        detail_parts.append(f"{len(interactive)} interactive")

        report.add("settings", f"tab_{tab_name}",
                   clicked and len(tab_widgets) > 3,
                   ", ".join(detail_parts),
                   widgets=tab_widgets)

        # Screenshot each tab
        ss = dev.screenshot()
        if ss:
            report.save_screenshot(f"settings_{tab_name}", ss)

        # Validate specific tab values
        if tab_name == "Display":
            for s in sliders:
                val = s.get("value", -1)
                report.add("settings", f"display_brightness_value",
                           0 <= val <= 255,
                           f"brightness={val}")

        elif tab_name == "Monitor":
            for b in bars:
                val = b.get("value", -1)
                report.add("settings", f"monitor_bar_value",
                           0 <= val <= 100,
                           f"bar value={val}%")

        elif tab_name == "Power":
            for s in sliders:
                val = s.get("value", -1)
                report.add("settings", f"power_slider_value",
                           val >= 0,
                           f"timeout={val}")

    # Return home
    dev.home()
    time.sleep(0.8)


def test_widget_interactions(dev: TritiumDevice, report: TestReport):
    """Test interacting with widgets — set slider values, toggle switches, etc."""
    print("\n--- Widget Interactions ---")

    # Launch settings, go to Display tab
    dev.launch("Settings")
    time.sleep(0.5)

    widgets = dev.ui_tree(flat=True)
    if not isinstance(widgets, list):
        report.add("interactions", "setup", False, "Failed to get widget tree")
        return

    # Find and test sliders
    sliders = extract_by_type(widgets, "lv_slider")
    for slider in sliders[:2]:  # Test first 2 sliders
        sid = slider["id"]
        original = slider.get("value", 0)

        # Set to a test value
        test_val = 128 if original != 128 else 64
        result = dev.set_value(sid, value=test_val)
        time.sleep(0.2)

        # Read back
        new_widgets = dev.ui_tree(flat=True)
        new_sliders = [w for w in new_widgets if w.get("id") == sid]
        new_val = new_sliders[0].get("value", -1) if new_sliders else -1

        report.add("interactions", f"slider_set_{sid[-6:]}",
                   new_val == test_val,
                   f"set {original} -> {test_val}, read back {new_val}")

        # Restore original
        dev.set_value(sid, value=original)
        time.sleep(0.1)

    # Find and test switches (if any visible)
    switches = extract_by_type(widgets, "lv_switch")
    for sw in switches[:1]:  # Test first switch only (be conservative)
        sid = sw["id"]
        original = sw.get("checked", False)

        # Toggle
        result = dev.set_value(sid, checked=not original)
        time.sleep(0.2)

        # Read back
        new_widgets = dev.ui_tree(flat=True)
        new_sws = [w for w in new_widgets if w.get("id") == sid]
        new_checked = new_sws[0].get("checked") if new_sws else None

        report.add("interactions", f"switch_toggle_{sid[-6:]}",
                   new_checked == (not original),
                   f"toggled {original} -> {not original}, read {new_checked}")

        # Restore
        dev.set_value(sid, checked=original)
        time.sleep(0.1)

    dev.home()
    time.sleep(0.8)


def test_all_values_readable(dev: TritiumDevice, report: TestReport):
    """Verify we can read values from every widget on every screen."""
    print("\n--- Value Readability ---")

    apps_data = dev.apps()
    apps = apps_data.get("apps", []) if not "_error" in apps_data else []

    total_widgets = 0
    total_with_values = 0

    for app in apps:
        name = app.get("name", "?")
        dev.launch(name)
        time.sleep(0.4)

        widgets = dev.ui_tree(flat=True)
        if not isinstance(widgets, list):
            continue

        values = extract_values(widgets)
        total_widgets += len(widgets)
        total_with_values += len(values)

        report.add("readability", f"app_{name}_values",
                   len(values) > 0 or len(widgets) < 5,
                   f"{len(values)} values from {len(widgets)} widgets")

    dev.home()
    time.sleep(0.8)

    report.add("readability", "total_coverage",
               total_with_values > 0,
               f"{total_with_values} readable values across {total_widgets} total widgets")


def test_stability(dev: TritiumDevice, report: TestReport, rapid_cycles: int = 5):
    """Rapid app switching and widget reading to test stability."""
    print("\n--- Stability ---")

    apps_data = dev.apps()
    apps = apps_data.get("apps", []) if "_error" not in apps_data else []
    app_names = [a.get("name", "") for a in apps if a.get("name")]

    errors = 0
    start = time.time()

    for cycle in range(rapid_cycles):
        for name in app_names:
            dev.launch(name)
            time.sleep(0.15)
            tree = dev.ui_tree(flat=True)
            if not isinstance(tree, list) or "_error" in (tree if isinstance(tree, dict) else {}):
                errors += 1
        dev.home()
        time.sleep(0.1)

    elapsed = time.time() - start
    total_ops = rapid_cycles * len(app_names) * 2  # launch + tree per app
    ops_per_sec = total_ops / elapsed if elapsed > 0 else 0

    report.add("stability", "rapid_app_switching",
               errors == 0,
               f"{rapid_cycles} cycles, {errors} errors, "
               f"{ops_per_sec:.1f} ops/s, {elapsed:.1f}s total")

    # Check device is still responsive
    info = dev.info()
    report.add("stability", "post_stress_responsive",
               "_error" not in info,
               "Device responsive after stress test")

    # Check memory didn't leak significantly
    lvgl = dev.lvgl_debug()
    heap = lvgl.get("heap_free", 0)
    psram = lvgl.get("psram_free", 0)
    report.add("stability", "post_stress_memory",
               heap > 30_000,
               f"heap={heap/1024:.0f}KB psram={psram/1024:.0f}KB")


def main():
    parser = argparse.ArgumentParser(description="Tritium-OS UI Test Sweep")
    parser.add_argument("host", nargs="?", default=None,
                        help="Device IP or hostname (auto-detect if omitted)")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--loops", type=int, default=1,
                        help="Number of full sweep loops")
    parser.add_argument("--output", default="test_report/ui_sweep",
                        help="Output directory for report and screenshots")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="HTTP request timeout in seconds")
    args = parser.parse_args()

    # Auto-detect device
    host = args.host
    if not host:
        # Try common addresses
        for candidate in ["tritium.local", "192.168.86.50", "192.168.4.1",
                          "192.168.1.100", "10.42.0.2"]:
            try:
                r = requests.get(f"http://{candidate}:{args.port}/api/remote/info",
                                 timeout=2)
                if r.status_code == 200:
                    host = candidate
                    print(f"Auto-detected device at {host}")
                    break
            except Exception:
                continue
        if not host:
            print("ERROR: No device found. Specify IP: python3 ui_test_sweep.py <IP>")
            sys.exit(1)

    dev = TritiumDevice(host, args.port, args.timeout)

    for loop in range(args.loops):
        loop_label = f"_loop{loop+1}" if args.loops > 1 else ""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_dir = f"{args.output}/{timestamp}{loop_label}"

        report = TestReport(output_dir)

        print(f"\n{'='*72}")
        print(f"TRITIUM-OS UI TEST SWEEP — Loop {loop+1}/{args.loops}")
        print(f"Device: {host}:{args.port}")
        print(f"Output: {output_dir}")
        print(f"{'='*72}")

        # Run test suites
        if not test_connectivity(dev, report):
            print("\nFATAL: Device not reachable. Aborting.")
            text = report.generate()
            print(f"\n{text}")
            sys.exit(1)

        try:
            test_performance(dev, report)
            test_app_launch(dev, report)
            test_settings_tabs(dev, report)
            test_widget_interactions(dev, report)
            test_all_values_readable(dev, report)
            test_stability(dev, report)
        except KeyboardInterrupt:
            print("\nInterrupted by user")
        except Exception as e:
            report.add("error", "unhandled_exception", False, traceback.format_exc())

        # Generate report
        text = report.generate()
        print(f"\n{text}")

        print(f"\nAPI stats: {dev.request_count} requests, {dev.error_count} errors")

        if loop < args.loops - 1:
            print(f"\nWaiting 2s before next loop...")
            time.sleep(2)


if __name__ == "__main__":
    main()
