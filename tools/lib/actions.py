"""
Tritium-OS Test Actions
========================
Reusable test actions for each widget type. Each action takes a matched
widget and its ElementSpec, exercises the element, and reports the result.
"""

import time
from lib.registry import ElementSpec
from lib.visual import VisualValidator, BBox


def action_test_slider(dev, vis: VisualValidator, report, spec: ElementSpec,
                       widget: dict, screen: str):
    """Change slider value, verify visual change, restore original."""
    sl_id = widget.get("id", "")
    sl_val = widget.get("value", 50)
    sl_min = widget.get("min", 0)
    sl_max = widget.get("max", 100)

    delta = spec.expect.get("delta", max(1, (sl_max - sl_min) // 5))
    min_pct = spec.expect.get("visual_min_pct", 0.1)

    # Calculate target value (move toward center of range)
    if sl_val + delta <= sl_max:
        new_val = sl_val + delta
    elif sl_val - delta >= sl_min:
        new_val = sl_val - delta
    else:
        new_val = (sl_min + sl_max) // 2

    before = dev.screenshot_np()

    dev.set_value(sl_id, value=new_val)
    time.sleep(0.6)

    after = dev.screenshot_np()

    # Restore IMMEDIATELY before analysis
    if spec.restore:
        dev.set_value(sl_id, value=sl_val)
        time.sleep(0.3)

    passed = True
    detail = f"{spec.name}: {sl_val}->{new_val}->{sl_val}"
    if before is not None and after is not None:
        diff = vis.compare(before, after)
        if diff.diff_pct < min_pct:
            passed = False
            detail += f" (no visual change, diff={diff.diff_pct:.2f}%)"
        else:
            detail += f" (diff={diff.diff_pct:.1f}%)"
    else:
        passed = False
        detail += " (screenshot failed)"

    report.add_element(screen, "slider", sl_id, spec.name, "set_value",
                       "changed" if passed else "no_change", passed, detail)
    report.add("elements", f"{screen}/{spec.name}", passed, detail)
    print(f"  [{'PASS' if passed else 'FAIL'}] {screen}/{spec.name}: {detail}")


def action_test_switch(dev, vis: VisualValidator, report, spec: ElementSpec,
                       widget: dict, screen: str):
    """Toggle switch, verify visual change, toggle back."""
    sw_id = widget.get("id", "")
    checked = widget.get("checked", False)
    min_pct = spec.expect.get("visual_min_pct", 0.05)

    before = dev.screenshot_np()
    dev.click(sw_id)
    time.sleep(0.8)
    after = dev.screenshot_np()

    passed = True
    toggle_dir = "on->off" if checked else "off->on"
    detail = f"{spec.name}: {toggle_dir}"

    if before is not None and after is not None:
        diff = vis.compare(before, after)
        if diff.diff_pct < min_pct:
            passed = False
            detail += f" (stuck, diff={diff.diff_pct:.2f}%)"
        else:
            detail += f" (diff={diff.diff_pct:.1f}%)"
    else:
        passed = False
        detail += " (screenshot failed)"

    # Toggle back
    if spec.restore:
        dev.click(sw_id)
        time.sleep(0.4)

    report.add_element(screen, "switch", sw_id, spec.name, "toggle",
                       "toggled" if passed else "stuck", passed, detail)
    report.add("elements", f"{screen}/{spec.name}", passed, detail)
    print(f"  [{'PASS' if passed else 'FAIL'}] {screen}/{spec.name}: {detail}")


def action_test_button(dev, vis: VisualValidator, report, spec: ElementSpec,
                       widget: dict, screen: str):
    """Press-hold button and verify it doesn't shift position."""
    btn_id = widget.get("id", "")
    bx = widget.get("x", 0) + widget.get("w", 0) // 2
    by = widget.get("y", 0) + widget.get("h", 0) // 2
    bw = widget.get("w", 40)
    bh = widget.get("h", 30)
    max_shift = spec.expect.get("max_shift_px", 2.0)

    margin = 4
    roi = BBox(
        max(0, widget.get("x", 0) - margin),
        max(0, widget.get("y", 0) - margin),
        bw + margin * 2,
        bh + margin * 2,
    )

    check = vis.check_button_press_movement(dev, bx, by, spec.name, roi)

    shift = (check.dx ** 2 + check.dy ** 2) ** 0.5
    passed = shift <= max_shift
    detail = f"{spec.name}: shift=({check.dx:+.1f},{check.dy:+.1f})px"

    report.add_element(screen, "button", btn_id, spec.name, "press_hold",
                       "stable" if passed else "moved", passed, detail)
    report.add("elements", f"{screen}/{spec.name}", passed, detail)

    status = "PASS" if passed else "FAIL"
    if not passed:
        print(f"  [{status}] {screen}/{spec.name}: MOVED {detail}")
    else:
        print(f"  [{status}] {screen}/{spec.name}: {detail}")


def action_test_dropdown(dev, vis: VisualValidator, report, spec: ElementSpec,
                         widget: dict, screen: str):
    """Open dropdown, verify visual change, close it."""
    dd_id = widget.get("id", "")
    min_pct = spec.expect.get("visual_min_pct", 0.5)

    before = dev.screenshot_np()
    dev.click(dd_id)
    time.sleep(1.0)
    opened = dev.screenshot_np()

    passed = True
    detail = f"{spec.name}: open/close"

    if before is not None and opened is not None:
        diff = vis.compare(before, opened)
        if diff.diff_pct < min_pct:
            # Retry once — click may not have registered
            dev.tap(400, 100)  # close if partially open
            time.sleep(0.5)
            before = dev.screenshot_np()
            dev.click(dd_id)
            time.sleep(1.2)
            opened = dev.screenshot_np()
            if before is not None and opened is not None:
                diff = vis.compare(before, opened)
            if diff.diff_pct < min_pct:
                passed = False
                detail += f" (didn't open, diff={diff.diff_pct:.2f}%)"
            else:
                detail += f" (opened on retry: diff={diff.diff_pct:.1f}%)"
        else:
            detail += f" (opened: diff={diff.diff_pct:.1f}%)"
    else:
        passed = False
        detail += " (screenshot failed)"

    # Close by tapping elsewhere
    dev.tap(400, 100)
    time.sleep(0.4)

    report.add_element(screen, "dropdown", dd_id, spec.name, "open_close",
                       "ok" if passed else "stuck", passed, detail)
    report.add("elements", f"{screen}/{spec.name}", passed, detail)
    print(f"  [{'PASS' if passed else 'FAIL'}] {screen}/{spec.name}: {detail}")


def action_skip(dev, vis: VisualValidator, report, spec: ElementSpec,
                widget: dict, screen: str):
    """Element found but intentionally skipped (dangerous)."""
    reason = spec.expect.get("reason", "dangerous")
    detail = f"{spec.name}: skipped ({reason})"
    report.add_element(screen, spec.widget_type, widget.get("id", ""),
                       spec.name, "skip", "skipped", True, detail)
    report.add("elements", f"{screen}/{spec.name}", True, detail)
    print(f"  [SKIP] {screen}/{spec.name}: {reason}")


def action_readonly(dev, vis: VisualValidator, report, spec: ElementSpec,
                    widget: dict, screen: str):
    """Element found, read-only — just verify it exists and has a value."""
    reason = spec.expect.get("reason", "read-only")
    val = widget.get("value", widget.get("checked", "?"))
    detail = f"{spec.name}: {val} ({reason})"
    report.add_element(screen, spec.widget_type, widget.get("id", ""),
                       spec.name, "readonly", "present", True, detail)
    report.add("elements", f"{screen}/{spec.name}", True, detail)
    print(f"  [READ] {screen}/{spec.name}: val={val} ({reason})")


def action_verify_present(dev, vis: VisualValidator, report, spec: ElementSpec,
                          widget: dict, screen: str):
    """Just verify the element exists on screen."""
    detail = f"{spec.name}: present"
    report.add_element(screen, spec.widget_type, widget.get("id", ""),
                       spec.name, "verify", "present", True, detail)
    report.add("elements", f"{screen}/{spec.name}", True, detail)
    print(f"  [ OK ] {screen}/{spec.name}: present")


# ── Action dispatch ─────────────────────────────────────────────────────

ACTION_MAP = {
    "test_slider": action_test_slider,
    "test_switch": action_test_switch,
    "test_button": action_test_button,
    "test_dropdown": action_test_dropdown,
    "skip": action_skip,
    "readonly": action_readonly,
    "verify_present": action_verify_present,
}
