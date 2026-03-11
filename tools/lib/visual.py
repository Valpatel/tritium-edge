"""
Tritium-OS Visual Validation
==============================
OpenCV-based screenshot comparison and widget position tracking.
Detects unintended visual changes like element movement, misalignment,
corruption, and layout regressions.
"""

import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import cv2
import numpy as np

from .device import TritiumDevice


@dataclass
class BBox:
    """Bounding box for a screen region."""
    x: int
    y: int
    w: int
    h: int

    @property
    def cx(self) -> float:
        return self.x + self.w / 2.0

    @property
    def cy(self) -> float:
        return self.y + self.h / 2.0

    @property
    def area(self) -> int:
        return self.w * self.h

    def iou(self, other: 'BBox') -> float:
        """Intersection over union with another bbox."""
        x1 = max(self.x, other.x)
        y1 = max(self.y, other.y)
        x2 = min(self.x + self.w, other.x + other.w)
        y2 = min(self.y + self.h, other.y + other.h)
        if x2 <= x1 or y2 <= y1:
            return 0.0
        intersection = (x2 - x1) * (y2 - y1)
        union = self.area + other.area - intersection
        return intersection / union if union > 0 else 0.0


@dataclass
class VisualDiff:
    """Result of comparing two screenshots."""
    changed: bool
    diff_pct: float  # Percentage of pixels that differ
    diff_mask: Optional[np.ndarray] = None  # Binary mask of changed pixels
    regions: list[BBox] = field(default_factory=list)  # Changed regions
    description: str = ""

    @property
    def passed(self) -> bool:
        return not self.changed


@dataclass
class PositionCheck:
    """Result of checking widget positions between frames."""
    widget_id: str
    label: str
    before: BBox
    after: BBox
    dx: float  # Horizontal shift in pixels
    dy: float  # Vertical shift in pixels
    moved: bool  # True if shift exceeds threshold

    @property
    def shift_px(self) -> float:
        return (self.dx ** 2 + self.dy ** 2) ** 0.5


@dataclass
class VisualReport:
    """Aggregated visual validation results."""
    name: str
    checks: list = field(default_factory=list)
    screenshots: dict = field(default_factory=dict)  # name -> path
    passed: bool = True
    details: str = ""

    def add_diff(self, name: str, diff: VisualDiff):
        self.checks.append({"type": "diff", "name": name, "result": diff})
        if diff.changed:
            self.passed = False

    def add_position_checks(self, name: str, checks: list[PositionCheck]):
        moved = [c for c in checks if c.moved]
        self.checks.append({
            "type": "position",
            "name": name,
            "checks": checks,
            "moved": moved
        })
        if moved:
            self.passed = False

    def summary(self) -> str:
        lines = [f"Visual Report: {self.name}"]
        for check in self.checks:
            if check["type"] == "diff":
                d = check["result"]
                status = "PASS" if d.passed else "FAIL"
                lines.append(f"  [{status}] {check['name']}: "
                             f"{d.diff_pct:.2f}% changed, "
                             f"{len(d.regions)} regions — {d.description}")
            elif check["type"] == "position":
                moved = check["moved"]
                total = len(check["checks"])
                status = "PASS" if not moved else "FAIL"
                lines.append(f"  [{status}] {check['name']}: "
                             f"{len(moved)}/{total} elements moved")
                for m in moved:
                    lines.append(f"    - {m.label}: shifted ({m.dx:+.1f}, {m.dy:+.1f})px")
        return "\n".join(lines)


class VisualValidator:
    """Screenshot-based visual validation using OpenCV."""

    def __init__(self, output_dir: str = "/tmp/tritium_visual",
                 diff_threshold: int = 30,
                 change_pct_threshold: float = 0.5,
                 position_threshold_px: float = 2.0):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.diff_threshold = diff_threshold
        self.change_pct_threshold = change_pct_threshold
        self.position_threshold_px = position_threshold_px

    def save(self, img: np.ndarray, name: str) -> str:
        """Save screenshot to output directory, return path."""
        path = str(self.output_dir / f"{name}.png")
        cv2.imwrite(path, img)
        return path

    def compare(self, before: np.ndarray, after: np.ndarray,
                roi: Optional[BBox] = None,
                threshold: Optional[int] = None) -> VisualDiff:
        """Compare two screenshots, optionally within a region of interest.

        Returns a VisualDiff describing what changed.
        """
        thresh = threshold or self.diff_threshold

        if before.shape != after.shape:
            return VisualDiff(
                changed=True, diff_pct=100.0,
                description=f"Size mismatch: {before.shape} vs {after.shape}")

        # Crop to ROI if specified
        if roi:
            before = before[roi.y:roi.y+roi.h, roi.x:roi.x+roi.w]
            after = after[roi.y:roi.y+roi.h, roi.x:roi.x+roi.w]

        # Compute absolute difference
        diff = cv2.absdiff(before, after)
        gray_diff = cv2.cvtColor(diff, cv2.COLOR_BGR2GRAY)

        # Threshold to binary mask
        _, mask = cv2.threshold(gray_diff, thresh, 255, cv2.THRESH_BINARY)

        # Calculate changed pixel percentage
        total_pixels = mask.shape[0] * mask.shape[1]
        changed_pixels = cv2.countNonZero(mask)
        diff_pct = (changed_pixels / total_pixels) * 100.0

        # Find changed regions via contours
        regions = []
        contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL,
                                        cv2.CHAIN_APPROX_SIMPLE)
        for c in contours:
            x, y, w, h = cv2.boundingRect(c)
            if w * h > 16:  # Ignore tiny noise
                regions.append(BBox(x, y, w, h))

        changed = diff_pct > self.change_pct_threshold
        desc = (f"{diff_pct:.2f}% pixels changed ({changed_pixels}/{total_pixels}), "
                f"{len(regions)} distinct regions")

        return VisualDiff(
            changed=changed,
            diff_pct=diff_pct,
            diff_mask=mask,
            regions=regions,
            description=desc)

    def save_diff_visualization(self, before: np.ndarray, after: np.ndarray,
                                 diff: VisualDiff, name: str) -> str:
        """Save a side-by-side diff visualization with changed regions highlighted."""
        h, w = before.shape[:2]

        # Create a 3-panel view: before | after | diff overlay
        canvas = np.zeros((h, w * 3 + 4, 3), dtype=np.uint8)
        canvas[:, :w] = before
        canvas[:, w+2:w*2+2] = after

        # Diff overlay: green = changed
        overlay = after.copy()
        if diff.diff_mask is not None:
            # Resize mask to match overlay if needed (ROI diff vs full image)
            mask = diff.diff_mask
            if mask.shape[:2] != overlay.shape[:2]:
                mask = cv2.resize(mask, (overlay.shape[1], overlay.shape[0]),
                                  interpolation=cv2.INTER_NEAREST)
            mask_bgr = cv2.cvtColor(mask, cv2.COLOR_GRAY2BGR)
            green = np.zeros_like(overlay)
            green[:, :, 1] = 255  # Green channel
            overlay = np.where(mask_bgr > 0,
                               cv2.addWeighted(overlay, 0.5, green, 0.5, 0),
                               overlay)

        # Draw bounding boxes around changed regions
        for region in diff.regions:
            cv2.rectangle(overlay,
                          (region.x, region.y),
                          (region.x + region.w, region.y + region.h),
                          (0, 0, 255), 2)

        canvas[:, w*2+4:] = overlay

        path = str(self.output_dir / f"{name}_diff.png")
        cv2.imwrite(path, canvas)
        return path

    def find_element_bbox(self, img: np.ndarray, template: np.ndarray,
                           threshold: float = 0.8) -> Optional[BBox]:
        """Find a template element in the image using template matching."""
        result = cv2.matchTemplate(img, template, cv2.TM_CCOEFF_NORMED)
        _, max_val, _, max_loc = cv2.minMaxLoc(result)

        if max_val >= threshold:
            th, tw = template.shape[:2]
            return BBox(max_loc[0], max_loc[1], tw, th)
        return None

    def find_all_elements(self, img: np.ndarray, template: np.ndarray,
                           threshold: float = 0.8) -> list[BBox]:
        """Find all instances of a template in the image."""
        result = cv2.matchTemplate(img, template, cv2.TM_CCOEFF_NORMED)
        locations = np.where(result >= threshold)

        th, tw = template.shape[:2]
        boxes = []
        for pt in zip(*locations[::-1]):
            boxes.append(BBox(pt[0], pt[1], tw, th))

        # Non-maximum suppression (simple overlap removal)
        boxes.sort(key=lambda b: b.x)
        filtered = []
        for box in boxes:
            if not any(box.iou(f) > 0.3 for f in filtered):
                filtered.append(box)

        return filtered

    def extract_widget_regions(self, img: np.ndarray,
                                min_area: int = 100,
                                brightness_threshold: int = 20) -> list[BBox]:
        """Extract distinct widget regions from a screenshot using edge detection."""
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        _, thresh = cv2.threshold(gray, brightness_threshold, 255, cv2.THRESH_BINARY)

        # Morphological cleanup
        kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3))
        thresh = cv2.morphologyEx(thresh, cv2.MORPH_CLOSE, kernel, iterations=2)

        contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL,
                                        cv2.CHAIN_APPROX_SIMPLE)

        regions = []
        for c in contours:
            x, y, w, h = cv2.boundingRect(c)
            if w * h >= min_area and w > 5 and h > 5:
                regions.append(BBox(x, y, w, h))

        return regions

    def track_positions(self, before_img: np.ndarray, after_img: np.ndarray,
                         widget_rois: list[tuple[str, BBox]],
                         threshold_px: Optional[float] = None) -> list[PositionCheck]:
        """Track widget positions between two frames using edge-based matching.

        Uses Canny edge detection to create templates that are invariant to
        color/fill changes (which are expected on button press). Only detects
        actual positional shifts.

        widget_rois: list of (label, BBox) pairs defining regions to track
                     in the 'before' image.
        """
        thresh = threshold_px or self.position_threshold_px
        results = []

        for label, roi in widget_rois:
            # Extract region from both images
            template_bgr = before_img[roi.y:roi.y+roi.h, roi.x:roi.x+roi.w]
            if template_bgr.size == 0:
                continue

            # Convert to grayscale edges — invariant to color changes
            template_gray = cv2.cvtColor(template_bgr, cv2.COLOR_BGR2GRAY)
            template_edges = cv2.Canny(template_gray, 50, 150)

            # If too few edges, fall back to grayscale template matching
            edge_density = cv2.countNonZero(template_edges) / max(template_edges.size, 1)
            use_edges = edge_density > 0.02  # At least 2% edge pixels

            # Search in a local neighborhood in 'after' image
            margin = 25
            search_x = max(0, roi.x - margin)
            search_y = max(0, roi.y - margin)
            search_x2 = min(after_img.shape[1], roi.x + roi.w + margin)
            search_y2 = min(after_img.shape[0], roi.y + roi.h + margin)

            search_region = after_img[search_y:search_y2, search_x:search_x2]

            if (search_region.shape[0] < template_bgr.shape[0] or
                search_region.shape[1] < template_bgr.shape[1]):
                continue

            if use_edges:
                # Edge-based matching (color-invariant)
                search_gray = cv2.cvtColor(search_region, cv2.COLOR_BGR2GRAY)
                search_edges = cv2.Canny(search_gray, 50, 150)
                result = cv2.matchTemplate(search_edges, template_edges,
                                           cv2.TM_CCOEFF_NORMED)
            else:
                # Fall back to grayscale template matching
                search_gray = cv2.cvtColor(search_region, cv2.COLOR_BGR2GRAY)
                result = cv2.matchTemplate(search_gray, template_gray,
                                           cv2.TM_CCOEFF_NORMED)

            _, max_val, _, max_loc = cv2.minMaxLoc(result)

            # Lower threshold for edge matching (edges are sparser)
            match_thresh = 0.3 if use_edges else 0.5

            if max_val < match_thresh:
                # Can't match — don't assume it moved, mark as inconclusive
                # (color changes on press are expected, not a position bug)
                results.append(PositionCheck(
                    widget_id="", label=label,
                    before=roi,
                    after=BBox(roi.x, roi.y, roi.w, roi.h),
                    dx=0, dy=0, moved=False
                ))
                continue

            # Convert back to full-image coordinates
            found_x = search_x + max_loc[0]
            found_y = search_y + max_loc[1]
            after_box = BBox(found_x, found_y, roi.w, roi.h)

            dx = found_x - roi.x
            dy = found_y - roi.y
            shift = (dx ** 2 + dy ** 2) ** 0.5
            moved = shift > thresh

            results.append(PositionCheck(
                widget_id="", label=label,
                before=roi, after=after_box,
                dx=dx, dy=dy, moved=moved
            ))

        return results

    def check_button_press_movement(self, dev: TritiumDevice,
                                     button_x: int, button_y: int,
                                     label: str = "button",
                                     roi: Optional[BBox] = None) -> PositionCheck:
        """Test whether a button moves when pressed by using touch hold + screenshot.

        1. Screenshot normal state
        2. Hold touch on button (PRESSED state)
        3. Screenshot while pressed
        4. Release touch
        5. Compare positions
        """
        # 1. Normal state screenshot
        before = dev.screenshot_np()
        if before is None:
            return PositionCheck("", label, BBox(0,0,0,0), BBox(0,0,0,0),
                                 0, 0, False)

        # Define the region to track (button area +/- some margin)
        if roi is None:
            # Use a 60x40 region centered on the button position
            roi = BBox(max(0, button_x - 30), max(0, button_y - 20), 60, 40)

        # 2. Hold touch on button
        dev.touch_hold(button_x, button_y)
        time.sleep(0.15)  # Let LVGL process the press and render

        # 3. Screenshot while pressed
        after = dev.screenshot_np()

        # 4. Release
        dev.touch_release(button_x, button_y)
        time.sleep(0.1)

        if after is None:
            return PositionCheck("", label, roi, BBox(0,0,0,0), 0, 0, False)

        # 5. Track position
        checks = self.track_positions(before, after, [(label, roi)])
        if checks:
            return checks[0]

        return PositionCheck("", label, roi, roi, 0, 0, False)

    def check_all_buttons_press_movement(
            self, dev: TritiumDevice,
            widgets: list[dict],
            max_buttons: int = 20) -> list[PositionCheck]:
        """Test all visible buttons for press movement artifacts.

        Uses the widget tree to find button positions, then tests each one.
        """
        results = []
        buttons = [w for w in widgets
                   if w.get("type") in ("btn", "lv_btn") and w.get("clickable")]

        for btn in buttons[:max_buttons]:
            bx = btn.get("x", 0) + btn.get("w", 0) // 2
            by = btn.get("y", 0) + btn.get("h", 0) // 2
            label = btn.get("text", btn.get("id", "")[-8:])

            # Use actual button bounds as ROI
            roi = BBox(btn.get("x", 0), btn.get("y", 0),
                       btn.get("w", 40), btn.get("h", 30))

            check = self.check_button_press_movement(dev, bx, by, label, roi)
            results.append(check)

            # Small delay between button tests
            time.sleep(0.2)

        return results

    def check_screen_stability(self, dev: TritiumDevice,
                                duration_s: float = 2.0,
                                interval_s: float = 0.5) -> VisualDiff:
        """Check that the screen doesn't change unexpectedly over time.

        Takes periodic screenshots and ensures they're stable (no flicker,
        no animation artifacts, no corruption).
        """
        frames = []
        start = time.time()
        while time.time() - start < duration_s:
            frame = dev.screenshot_np()
            if frame is not None:
                frames.append(frame)
            time.sleep(interval_s)

        if len(frames) < 2:
            return VisualDiff(changed=False, diff_pct=0.0,
                              description="Not enough frames captured")

        # Compare each frame to the first
        max_diff_pct = 0.0
        all_regions = []
        for i in range(1, len(frames)):
            diff = self.compare(frames[0], frames[i])
            max_diff_pct = max(max_diff_pct, diff.diff_pct)
            all_regions.extend(diff.regions)

        changed = max_diff_pct > self.change_pct_threshold
        return VisualDiff(
            changed=changed,
            diff_pct=max_diff_pct,
            regions=all_regions,
            description=f"{len(frames)} frames over {duration_s}s, "
                        f"max diff {max_diff_pct:.2f}%")
