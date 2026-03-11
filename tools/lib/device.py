"""
Tritium-OS Device API Client
=============================
REST API client for communicating with Tritium-OS devices.
Handles connection management, retries, and BMP screenshot decoding.
"""

import io
import struct
import time
from typing import Optional

import numpy as np
import requests


class TritiumDevice:
    """REST API client for a Tritium-OS device."""

    def __init__(self, host: str, port: int = 80, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.base = f"http://{host}:{port}"
        self.timeout = timeout
        self.request_count = 0
        self.error_count = 0
        self.session = requests.Session()

    def _get(self, path: str, retries: int = 1) -> dict | list | None:
        for attempt in range(1 + retries):
            self.request_count += 1
            try:
                r = self.session.get(f"{self.base}{path}", timeout=self.timeout)
                r.raise_for_status()
                return r.json()
            except Exception as e:
                self.error_count += 1
                if attempt < retries:
                    time.sleep(1.0)  # Brief backoff before retry
                    continue
                return {"_error": str(e)}

    def _post(self, path: str, data: dict, retries: int = 1) -> dict | None:
        for attempt in range(1 + retries):
            self.request_count += 1
            try:
                r = self.session.post(f"{self.base}{path}", json=data, timeout=self.timeout)
                r.raise_for_status()
                return r.json()
            except Exception as e:
                self.error_count += 1
                if attempt < retries:
                    time.sleep(1.0)
                    continue
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

    def screenshot_bmp(self) -> bytes | None:
        """Get raw BMP screenshot bytes from the device."""
        return self._get_raw("/api/screenshot")

    def screenshot_np(self) -> Optional[np.ndarray]:
        """Get screenshot as a BGR numpy array (OpenCV format)."""
        bmp = self.screenshot_bmp()
        if not bmp:
            return None
        return bmp_to_numpy(bmp)

    def touch(self, x: int, y: int, pressed: bool = True) -> dict:
        """Send touch event. Use pressed=True for press, False for release."""
        return self._post("/api/remote/touch", {"x": x, "y": y, "pressed": pressed})

    def tap(self, x: int, y: int, hold_ms: int = 80) -> dict:
        """Tap at position (press + delay + release)."""
        self.touch(x, y, pressed=True)
        time.sleep(hold_ms / 1000.0)
        return self.touch(x, y, pressed=False)

    def touch_hold(self, x: int, y: int) -> dict:
        """Press and hold at position (no release). Call touch_release() after."""
        return self.touch(x, y, pressed=True)

    def touch_release(self, x: int, y: int) -> dict:
        """Release a held touch."""
        return self.touch(x, y, pressed=False)

    def lvgl_debug(self) -> dict:
        return self._get("/api/debug/lvgl")

    def wifi_status(self) -> dict:
        return self._get("/api/wifi/status")

    def diag(self) -> dict:
        return self._get("/api/diag")

    def diag_health(self) -> dict:
        return self._get("/api/diag/health")

    def is_reachable(self) -> bool:
        try:
            r = self.session.get(f"{self.base}/api/remote/info", timeout=2)
            return r.status_code == 200
        except Exception:
            return False


def bmp_to_numpy(bmp_bytes: bytes) -> Optional[np.ndarray]:
    """Decode a BMP byte stream into a BGR numpy array (OpenCV format).

    Handles top-down BMPs (negative height) as produced by the Tritium
    screenshot endpoint — 24-bit uncompressed, rows padded to 4 bytes.
    """
    if len(bmp_bytes) < 54 or bmp_bytes[0:2] != b'BM':
        return None

    offset = struct.unpack_from('<I', bmp_bytes, 10)[0]
    width = struct.unpack_from('<i', bmp_bytes, 18)[0]
    height = struct.unpack_from('<i', bmp_bytes, 22)[0]
    bpp = struct.unpack_from('<H', bmp_bytes, 28)[0]

    if bpp != 24:
        return None

    top_down = height < 0
    abs_height = abs(height)

    row_bytes = width * 3
    row_pad = (4 - (row_bytes % 4)) % 4
    padded_row = row_bytes + row_pad

    img = np.zeros((abs_height, width, 3), dtype=np.uint8)

    for y in range(abs_height):
        src_y = y if top_down else (abs_height - 1 - y)
        row_start = offset + src_y * padded_row
        row_end = row_start + row_bytes
        if row_end > len(bmp_bytes):
            break
        row_data = np.frombuffer(bmp_bytes[row_start:row_end], dtype=np.uint8)
        img[y] = row_data.reshape(width, 3)

    # BMP stores BGR natively — same as OpenCV convention
    return img


def auto_detect(port: int = 80) -> Optional[str]:
    """Try common device addresses and return the first reachable one."""
    for h in ["tritium.local", "192.168.86.50", "192.168.4.1",
              "192.168.1.100", "10.42.0.2", "10.42.0.237"]:
        try:
            r = requests.get(f"http://{h}:{port}/api/remote/info", timeout=1.5)
            if r.status_code == 200:
                return h
        except Exception:
            pass
    return None
