# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Bulk sighting upload endpoint for ESP32 sensor nodes.

Accepts batched BLE and WiFi sightings from the ESP32 firmware's
``sync_to_server()`` function (hal_sighting_buffer) and records them
into the shared BleStore (SQLite).

Supports two payload formats:

1. **Separated arrays** (primary) — ``ble_sightings`` and ``wifi_sightings``
   as top-level arrays.
2. **Flat sightings** (firmware native) — a single ``sightings`` array where
   each entry has a ``"t"`` field of ``"ble"`` or ``"wifi"``.
"""

from __future__ import annotations

import logging
from datetime import datetime, timezone
from typing import Optional

from fastapi import APIRouter, HTTPException, Request
from pydantic import BaseModel, Field

# Shared BLE store from tritium-lib (SQLite)
try:
    from tritium_lib.store.ble import BleStore
except ImportError:
    BleStore = None  # type: ignore[assignment,misc]

logger = logging.getLogger("tritium.sightings")

router = APIRouter(prefix="/api", tags=["sightings"])


# ---------------------------------------------------------------------------
# Request models
# ---------------------------------------------------------------------------

class BleSightingItem(BaseModel):
    mac: str
    rssi: int
    name: str = ""
    timestamp: Optional[int] = None
    known: Optional[bool] = None
    is_known: Optional[bool] = None
    seen: Optional[int] = None
    seen_count: Optional[int] = None
    ntp: Optional[bool] = None


class WifiSightingItem(BaseModel):
    bssid: str
    ssid: str = ""
    rssi: int
    channel: int = 0
    ch: Optional[int] = None
    auth: Optional[str] = None
    auth_type: Optional[str] = None
    timestamp: Optional[int] = None
    ntp: Optional[bool] = None


class BulkSightingUpload(BaseModel):
    device_id: str = ""
    ble_sightings: list[BleSightingItem] = Field(default_factory=list)
    wifi_sightings: list[WifiSightingItem] = Field(default_factory=list)
    sightings: list[dict] = Field(default_factory=list)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _get_ble_store(request: Request) -> Optional["BleStore"]:
    return getattr(request.app.state, "ble_store", None)


def _ts_to_iso(epoch: Optional[int]) -> Optional[str]:
    """Convert a Unix epoch timestamp to ISO 8601 string, or None."""
    if epoch is None or epoch <= 0:
        return None
    try:
        return datetime.fromtimestamp(epoch, tz=timezone.utc).isoformat()
    except (OSError, ValueError, OverflowError):
        return None


def _split_flat_sightings(
    flat: list[dict],
) -> tuple[list[BleSightingItem], list[WifiSightingItem]]:
    """Split a flat sightings array (firmware format) by type field."""
    ble: list[BleSightingItem] = []
    wifi: list[WifiSightingItem] = []
    for entry in flat:
        t = entry.get("t", "")
        if t == "ble":
            ble.append(BleSightingItem(
                mac=entry.get("mac", ""),
                rssi=entry.get("rssi", -100),
                name=entry.get("name", ""),
                timestamp=entry.get("ts"),
                known=bool(entry.get("known", False)),
                seen=entry.get("seen"),
                ntp=entry.get("ntp"),
            ))
        elif t == "wifi":
            wifi.append(WifiSightingItem(
                bssid=entry.get("bssid", ""),
                ssid=entry.get("ssid", ""),
                rssi=entry.get("rssi", -100),
                ch=entry.get("ch"),
                channel=entry.get("ch", 0),
                auth=entry.get("auth", ""),
                timestamp=entry.get("ts"),
                ntp=entry.get("ntp"),
            ))
    return ble, wifi


# ---------------------------------------------------------------------------
# Endpoint
# ---------------------------------------------------------------------------

@router.post("/devices/{device_id}/sightings")
async def bulk_sighting_upload(
    device_id: str, body: BulkSightingUpload, request: Request
):
    """Receive batched BLE and WiFi sightings from an ESP32 sensor node.

    Records sightings into the shared BleStore (SQLite) and returns a
    summary of what was recorded.
    """
    ble_store = _get_ble_store(request)

    # Merge separated arrays with any flat-format sightings
    ble_items = list(body.ble_sightings)
    wifi_items = list(body.wifi_sightings)

    if body.sightings:
        extra_ble, extra_wifi = _split_flat_sightings(body.sightings)
        ble_items.extend(extra_ble)
        wifi_items.extend(extra_wifi)

    ble_recorded = 0
    wifi_recorded = 0

    if ble_store is None:
        logger.warning(
            "BleStore not available — dropping %d BLE and %d WiFi sightings "
            "from %s",
            len(ble_items), len(wifi_items), device_id,
        )
        return {
            "status": "no_store",
            "device_id": device_id,
            "ble_recorded": 0,
            "wifi_recorded": 0,
            "ble_received": len(ble_items),
            "wifi_received": len(wifi_items),
            "message": "BleStore not available — sightings were not persisted",
        }

    # --- Record BLE sightings ---
    if ble_items:
        ble_rows = []
        for s in ble_items:
            ts = _ts_to_iso(s.timestamp)
            ble_rows.append({
                "mac": s.mac,
                "name": s.name,
                "rssi": s.rssi,
                "node_id": device_id,
                "node_ip": "",
                "is_known": s.known or s.is_known or False,
                "seen_count": s.seen or s.seen_count or 1,
                **({"timestamp": ts} if ts else {}),
            })
        try:
            ble_recorded = ble_store.record_sightings_batch(ble_rows)
        except Exception:
            logger.exception("Failed to record BLE sightings from %s", device_id)

    # --- Record WiFi sightings ---
    if wifi_items:
        wifi_rows = []
        for s in wifi_items:
            ts = _ts_to_iso(s.timestamp)
            channel = s.channel if s.channel else (s.ch or 0)
            auth = s.auth_type or s.auth or ""
            wifi_rows.append({
                "ssid": s.ssid,
                "bssid": s.bssid,
                "rssi": s.rssi,
                "channel": channel,
                "auth_type": auth,
                "node_id": device_id,
                **({"timestamp": ts} if ts else {}),
            })
        try:
            wifi_recorded = ble_store.record_wifi_sightings_batch(wifi_rows)
        except Exception:
            logger.exception("Failed to record WiFi sightings from %s", device_id)

    logger.info(
        "Recorded %d BLE + %d WiFi sightings from %s",
        ble_recorded, wifi_recorded, device_id,
    )

    return {
        "status": "ok",
        "device_id": device_id,
        "ble_recorded": ble_recorded,
        "wifi_recorded": wifi_recorded,
        "ble_received": len(ble_items),
        "wifi_received": len(wifi_items),
    }
