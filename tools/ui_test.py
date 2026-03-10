#!/usr/bin/env python3
"""
Tritium-OS Automated UI Test Harness

Exercises the shell UI remotely via touch injection + screenshot capture.
Uses tritium_lib.testing for visual analysis and device communication.
Serial monitoring for crash detection is handled locally (edge-specific).

Usage:
    python3 tools/ui_test.py [--host 10.42.0.237] [--serial /dev/ttyACM0]
    python3 tools/ui_test.py --visual-only  # just run visual checks on all apps
"""

import argparse
import cv2
import numpy as np
import re
import serial
import sys
import time
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from tritium_lib.testing import DeviceAPI, VisualCheck, LayoutIssue
from tritium_lib.testing.runner import UITestRunner
from tritium_lib.testing.visual import Severity

# ── Config ────────────────────────────────────────────────────────────────────

SCREENSHOT_DIR = Path("/tmp/tritium_ui_test")
SCREENSHOT_DIR.mkdir(exist_ok=True)

# ── Data types ────────────────────────────────────────────────────────────────

@dataclass
class TestResult:
    name: str
    passed: bool
    screenshot_path: Optional[str] = None
    details: str = ""
    warnings: list = field(default_factory=list)

# ── Serial Monitor Thread ─────────────────────────────────────────────────────

class SerialMonitor:
    """Background thread capturing serial output and watching for crashes."""

    def __init__(self, port: str, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.lines: list[str] = []
        self.crash_detected = False
        self.crash_text = ""
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._ser: Optional[serial.Serial] = None

    def start(self):
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=3)
        if self._ser and self._ser.is_open:
            self._ser.close()

    def get_lines_since(self, marker: int) -> list[str]:
        return self.lines[marker:]

    def get_marker(self) -> int:
        return len(self.lines)

    def _run(self):
        try:
            self._ser = serial.Serial(self.port, self.baud, timeout=0.5)
        except Exception as e:
            print(f"  [serial] Cannot open {self.port}: {e}")
            return

        crash_patterns = re.compile(
            r'(Guru Meditation|abort\(\)|Backtrace:|panic|assert.*failed|'
            r'LoadProhibited|StoreProhibited|InstrFetchProhibited|'
            r'Unhandled debug exception|rst:0x[0-9a-f]+.*boot:)',
            re.IGNORECASE
        )

        while self._running:
            try:
                raw = self._ser.readline()
                if raw:
                    line = raw.decode('utf-8', errors='replace').rstrip()
                    self.lines.append(line)
                    if crash_patterns.search(line):
                        self.crash_detected = True
                        self.crash_text = line
                        for _ in range(20):
                            raw2 = self._ser.readline()
                            if raw2:
                                l2 = raw2.decode('utf-8', errors='replace').rstrip()
                                self.lines.append(l2)
                                self.crash_text += "\n" + l2
            except Exception:
                pass

# ── Helpers ───────────────────────────────────────────────────────────────────

def save_screenshot(img: np.ndarray, name: str) -> str:
    path = str(SCREENSHOT_DIR / f"{name}.png")
    cv2.imwrite(path, img)
    return path

def detect_launcher_icons(img: np.ndarray) -> list[tuple]:
    """Detect launcher icon cell positions using contour analysis."""
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    _, thresh = cv2.threshold(gray, 15, 255, cv2.THRESH_BINARY)
    contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

    icons = []
    for c in contours:
        x, y, w, h = cv2.boundingRect(c)
        area = w * h
        aspect = w / max(h, 1)
        if 2000 < area < 25000 and 0.5 < aspect < 2.0 and y > 30:
            cx = x + w // 2
            cy = y + h // 2
            icons.append((cx, cy, f"icon_{cx}x{cy}"))

    icons.sort(key=lambda p: p[0])
    if icons:
        print(f"    Detected {len(icons)} launcher icons: "
              + ", ".join(f"({x},{y})" for x,y,_ in icons))
    return icons

def check_serial_errors(serial_mon: SerialMonitor, marker: int) -> list[str]:
    """Check serial output for crash indicators since marker."""
    new_lines = serial_mon.get_lines_since(marker)
    return [l for l in new_lines if any(
        kw in l.lower() for kw in ['abort', 'panic', 'assert', 'guru', 'backtrace']
    )]

# ── Test Sequences ────────────────────────────────────────────────────────────

def test_initial_state(api: DeviceAPI, checker: VisualCheck,
                       serial_mon: SerialMonitor) -> TestResult:
    """Test 1: Verify initial boot state — launcher should be visible."""
    print("  [1/9] Initial state check...")
    time.sleep(1)
    img = api.screenshot_raw()
    if img is None:
        return TestResult("initial_state", False, details="Cannot capture screenshot")

    path = save_screenshot(img, "01_initial_state")
    report = checker.run_all(img, app_name="launcher", has_nav_bar=False)

    details = (f"brightness={report.metrics.get('mean_brightness', 0):.1f} "
               f"edges={report.metrics.get('edge_density', 0):.3f} "
               f"colors={report.metrics.get('unique_colors', 0)}")

    return TestResult(
        "initial_state",
        passed=report.passed,
        screenshot_path=path,
        details=details,
        warnings=[str(i) for i in report.warnings]
    )

def test_touch_responsiveness(api: DeviceAPI, checker: VisualCheck,
                              serial_mon: SerialMonitor) -> TestResult:
    """Test 2: Inject a touch and verify device registers it."""
    print("  [2/9] Touch responsiveness...")
    before = api.touch_debug()

    before_inject = before.inject_count

    api.tap(400, 240)
    time.sleep(0.5)

    after = api.touch_debug()
    injected = after.inject_count - before_inject
    details = f"injected={injected} last_xy=({after.last_raw_x},{after.last_raw_y})"

    img = api.screenshot_raw()
    path = save_screenshot(img, "02_after_touch") if img is not None else None

    return TestResult(
        "touch_responsiveness",
        passed=injected > 0,
        screenshot_path=path,
        details=details
    )

def test_launcher_grid(api: DeviceAPI, checker: VisualCheck,
                       serial_mon: SerialMonitor) -> TestResult:
    """Test 3: Verify launcher grid renders correctly with app icons."""
    print("  [3/9] Launcher grid rendering...")
    img = api.screenshot_raw()
    if img is None:
        return TestResult("launcher_grid", False, details="Screenshot failed")

    path = save_screenshot(img, "03_launcher_grid")
    report = checker.run_all(img, app_name="launcher", has_nav_bar=False)

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    _, thresh = cv2.threshold(gray, 30, 255, cv2.THRESH_BINARY)
    contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    large_contours = [c for c in contours if cv2.contourArea(c) > 1000]

    details = (f"contours={len(large_contours)} "
               f"edges={report.metrics.get('edge_density', 0):.3f} "
               f"colors={report.metrics.get('unique_colors', 0)}")

    return TestResult(
        "launcher_grid",
        passed=report.passed and len(large_contours) > 3,
        screenshot_path=path,
        details=details,
        warnings=[str(i) for i in report.issues]
    )

def test_app_navigation(api: DeviceAPI, checker: VisualCheck,
                        serial_mon: SerialMonitor) -> TestResult:
    """Test 4: Launch each app via API, screenshot, check visuals, go home."""
    print("  [4/9] App navigation...")
    all_issues = []
    navigated = []
    crashed = False

    apps = api.apps()
    if not apps:
        # Fallback: use launcher icon detection
        launcher_img = api.screenshot_raw()
        if launcher_img is None:
            return TestResult("app_navigation", False, details="Cannot capture launcher")
        save_screenshot(launcher_img, "04_launcher_ref")
        return TestResult("app_navigation", False, details="No apps returned from API")

    for app in apps:
        if serial_mon.crash_detected:
            crashed = True
            break

        marker = serial_mon.get_marker()
        print(f"    Launching {app.name} (index={app.index})...")
        ok = api.launch(app.index)
        if not ok:
            all_issues.append(f"{app.name}: launch failed")
            continue

        time.sleep(1.0)

        if serial_mon.crash_detected:
            crashed = True
            all_issues.append(f"CRASH after launching {app.name}!")
            break

        img = api.screenshot_raw()
        if img is None:
            all_issues.append(f"{app.name}: screenshot failed")
            if not api.is_reachable():
                all_issues.append("Device unreachable — likely crashed")
                crashed = True
                break
            continue

        path = save_screenshot(img, f"04_nav_{app.name}")
        report = checker.run_all(img, app_name=app.name, has_nav_bar=True)

        if not report.passed:
            for issue in report.errors:
                all_issues.append(f"{app.name}: {issue}")
        navigated.append(app.name)

        errors = check_serial_errors(serial_mon, marker)
        if errors:
            all_issues.append(f"{app.name}: serial errors: {errors[0]}")
            crashed = True
            break

        api.home()
        time.sleep(0.5)

    details = f"navigated={len(navigated)}/{len(apps)}"
    if crashed:
        details += " CRASH_DETECTED"

    return TestResult(
        "app_navigation",
        passed=not crashed and len(all_issues) == 0,
        screenshot_path=str(SCREENSHOT_DIR / "04_nav_Settings.png"),
        details=details,
        warnings=all_issues
    )

def test_rapid_taps(api: DeviceAPI, checker: VisualCheck,
                    serial_mon: SerialMonitor) -> TestResult:
    """Test 5: Rapid-fire taps to stress-test touch handling."""
    print("  [5/9] Rapid tap stress test...")
    marker = serial_mon.get_marker()

    for i in range(20):
        x = 100 + (i * 30) % 600
        y = 100 + (i * 47) % 300
        api.tap(x, y)
        time.sleep(0.05)

    time.sleep(1)

    crashed = serial_mon.crash_detected
    img = api.screenshot_raw()
    path = save_screenshot(img, "05_rapid_taps") if img is not None else None

    warnings = []
    details = f"20 rapid taps, alive={api.is_reachable()}"

    if crashed:
        warnings.append("Crash detected during rapid taps")
    if img is not None:
        report = checker.run_all(img, app_name="rapid_taps")
        warnings.extend(str(i) for i in report.issues)
        details += f" edges={report.metrics.get('edge_density', 0):.3f}"

    return TestResult(
        "rapid_taps",
        passed=not crashed and api.is_reachable(),
        screenshot_path=path,
        details=details,
        warnings=warnings
    )

def test_swipe_gesture(api: DeviceAPI, checker: VisualCheck,
                       serial_mon: SerialMonitor) -> TestResult:
    """Test 6: Swipe gestures (simulated via rapid sequential taps)."""
    print("  [6/9] Swipe gesture test...")
    marker = serial_mon.get_marker()

    # Simulate swipe left
    for i in range(15):
        x = 600 - int((400 / 15) * i)
        api.tap(x, 240)
        time.sleep(0.03)
    time.sleep(0.5)

    crashed = serial_mon.crash_detected
    img = api.screenshot_raw()
    path = save_screenshot(img, "06_swipe") if img is not None else None

    warnings = []
    if crashed:
        warnings.append("Crash during swipe")
    details = f"alive={api.is_reachable()}"

    return TestResult(
        "swipe_gesture",
        passed=not crashed and api.is_reachable(),
        screenshot_path=path,
        details=details,
        warnings=warnings
    )

def test_edge_touches(api: DeviceAPI, checker: VisualCheck,
                      serial_mon: SerialMonitor) -> TestResult:
    """Test 7: Touch at screen edges and corners."""
    print("  [7/9] Edge and corner touches...")
    corners = [
        (5, 5), (795, 5), (5, 475), (795, 475),
        (400, 2), (400, 478), (2, 240), (798, 240),
    ]

    for x, y in corners:
        api.tap(x, y)
        time.sleep(0.2)
        if serial_mon.crash_detected:
            break

    time.sleep(0.5)
    crashed = serial_mon.crash_detected
    img = api.screenshot_raw()
    path = save_screenshot(img, "07_edges") if img is not None else None

    return TestResult(
        "edge_touches",
        passed=not crashed and api.is_reachable(),
        screenshot_path=path,
        details=f"8 edge taps, alive={api.is_reachable()}",
        warnings=["Crash during edge touches"] if crashed else []
    )

def test_sustained_interaction(api: DeviceAPI, checker: VisualCheck,
                               serial_mon: SerialMonitor) -> TestResult:
    """Test 8: 10 seconds of continuous random interaction."""
    print("  [8/9] Sustained interaction (10s)...")

    screenshots = []
    start = time.time()
    tap_count = 0

    while time.time() - start < 10:
        x = np.random.randint(50, 750)
        y = np.random.randint(50, 430)
        api.tap(x, y)
        tap_count += 1
        time.sleep(0.3)

        if serial_mon.crash_detected:
            break

        if tap_count % 5 == 0:
            img = api.screenshot_raw()
            if img is not None:
                screenshots.append(img)
                save_screenshot(img, f"08_sustained_{tap_count}")

    crashed = serial_mon.crash_detected
    warnings = []
    if crashed:
        warnings.append("Crash during sustained interaction")

    for i, simg in enumerate(screenshots):
        report = checker.run_all(simg, app_name=f"sustained_{i}")
        for issue in report.errors:
            warnings.append(str(issue))

    details = f"taps={tap_count} screenshots={len(screenshots)} alive={api.is_reachable()}"

    return TestResult(
        "sustained_interaction",
        passed=not crashed and api.is_reachable(),
        screenshot_path=str(SCREENSHOT_DIR / f"08_sustained_{tap_count}.png"),
        details=details,
        warnings=warnings
    )

def test_recovery_check(api: DeviceAPI, checker: VisualCheck,
                        serial_mon: SerialMonitor) -> TestResult:
    """Test 9: Final state check — device should still be responsive."""
    print("  [9/9] Recovery and final state...")
    time.sleep(2)

    alive = api.is_reachable()
    img = api.screenshot_raw()
    path = save_screenshot(img, "09_final_state") if img is not None else None

    details = f"alive={alive}"
    warnings = []

    if img is not None:
        report = checker.run_all(img, app_name="final_state")
        for issue in report.errors:
            warnings.append(str(issue))
        details += f" edges={report.metrics.get('edge_density', 0):.3f}"

    if serial_mon.crash_detected:
        warnings.append(f"Crash detected: {serial_mon.crash_text[:200]}")

    return TestResult(
        "recovery_check",
        passed=alive and not serial_mon.crash_detected,
        screenshot_path=path,
        details=details,
        warnings=warnings
    )

# ── Main ──────────────────────────────────────────────────────────────────────

def run_visual_only(host: str):
    """Run just the shared visual checks on all apps (no serial, no stress)."""
    print(f"\n{'='*70}")
    print(f"  TRITIUM-OS VISUAL CHECK")
    print(f"  Target: {host}")
    print(f"  Screenshots: {SCREENSHOT_DIR}")
    print(f"{'='*70}\n")

    runner = UITestRunner(host, screenshot_dir=str(SCREENSHOT_DIR))
    result = runner.run_all()
    print(result.summary())
    return result.passed


def run_all_tests(host: str, serial_port: str) -> list[TestResult]:
    """Run the complete test suite with serial monitoring."""
    print(f"\n{'='*70}")
    print(f"  TRITIUM-OS UI TEST HARNESS")
    print(f"  Target: {host}  Serial: {serial_port}")
    print(f"  Screenshots: {SCREENSHOT_DIR}")
    print(f"{'='*70}\n")

    api = DeviceAPI(host)
    checker = VisualCheck()
    serial_mon = SerialMonitor(serial_port)

    serial_mon.start()
    time.sleep(0.5)

    print("  Checking device connectivity...")
    if not api.is_reachable():
        print("  ERROR: Device not reachable. Aborting.")
        serial_mon.stop()
        return [TestResult("connectivity", False, details=f"Cannot reach {host}")]

    print(f"  Device online. Starting tests...\n")

    tests = [
        test_initial_state,
        test_touch_responsiveness,
        test_launcher_grid,
        test_app_navigation,
        test_rapid_taps,
        test_swipe_gesture,
        test_edge_touches,
        test_sustained_interaction,
        test_recovery_check,
    ]

    results = []
    for test_fn in tests:
        try:
            result = test_fn(api, checker, serial_mon)
        except Exception as e:
            result = TestResult(test_fn.__name__, False, details=f"Exception: {e}")

        results.append(result)

        status = "PASS" if result.passed else "FAIL"
        print(f"    [{status}] {result.name}: {result.details}")
        for w in result.warnings:
            print(f"           ! {w}")

        if serial_mon.crash_detected:
            print("\n  *** CRASH DETECTED — waiting 10s for reboot... ***")
            print(f"  Crash: {serial_mon.crash_text[:300]}")
            time.sleep(10)
            if api.is_reachable():
                print("  Device recovered. Continuing tests...")
                serial_mon.crash_detected = False
                serial_mon.crash_text = ""
            else:
                print("  Device did not recover. Aborting remaining tests.")
                break
        print()

    serial_mon.stop()

    passed = sum(1 for r in results if r.passed)
    total = len(results)
    print(f"\n{'='*70}")
    print(f"  RESULTS: {passed}/{total} passed")
    print(f"{'='*70}")
    for r in results:
        s = "PASS" if r.passed else "FAIL"
        print(f"  [{s}] {r.name}")
        if r.screenshot_path:
            print(f"         screenshot: {r.screenshot_path}")
        if r.warnings:
            for w in r.warnings:
                print(f"         ! {w}")
    print(f"\n  All screenshots saved to: {SCREENSHOT_DIR}")
    print(f"{'='*70}\n")

    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Tritium-OS UI Test Harness")
    parser.add_argument("--host", default="http://10.42.0.237", help="Device URL")
    parser.add_argument("--serial", default="/dev/ttyACM0", help="Serial port")
    parser.add_argument("--visual-only", action="store_true",
                        help="Run only visual checks (no serial, no stress tests)")
    args = parser.parse_args()

    if args.visual_only:
        ok = run_visual_only(args.host)
        sys.exit(0 if ok else 1)
    else:
        results = run_all_tests(args.host, args.serial)
        sys.exit(0 if all(r.passed for r in results) else 1)
