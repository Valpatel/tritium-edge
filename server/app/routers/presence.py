# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""BLE presence aggregation — cross-node device tracking for triangulation.

Uses tritium-lib's shared BleStore for persistent sighting storage and
trilateration.  The JSON fleet store is still used for building the live
presence map from heartbeat snapshots, but every sighting is now recorded
in SQLite so tritium-sc (or any Tritium app) can query history, manage
tracked targets, and estimate positions.
"""

from datetime import datetime, timezone
from typing import Optional

from fastapi import APIRouter, Request
from pydantic import BaseModel

from tritium_lib.models.ble import (
    BleDevice,
    BleSighting,
    BlePresence,
    BlePresenceMap,
)

# Shared BLE store from tritium-lib (SQLite)
try:
    from tritium_lib.store.ble import BleStore
except ImportError:
    BleStore = None  # type: ignore[assignment,misc]

# Trilateration for position estimation
try:
    from tritium_lib.models.trilateration import estimate_position
except ImportError:
    estimate_position = None  # type: ignore[assignment]

router = APIRouter(prefix="/api/presence", tags=["presence"])


def _get_store(request: Request):
    return request.app.state.store


def _get_ble_store(request: Request) -> Optional["BleStore"]:
    return getattr(request.app.state, "ble_store", None)


def _build_presence_map(devices: list[dict]) -> BlePresenceMap:
    """Build a BlePresenceMap from raw device dicts using tritium-lib models."""
    presence_devices: dict[str, BlePresence] = {}
    reporting_nodes = 0

    for device in devices:
        ble_list = device.get("ble_devices", [])
        if not ble_list:
            continue

        reporting_nodes += 1
        node_id = device["device_id"]
        node_ip = device.get("ip", "")
        node_rssi = device.get("rssi") or 0

        for ble_raw in ble_list:
            mac = ble_raw.get("mac", "")
            if not mac:
                continue

            ble_dev = BleDevice(
                mac=mac,
                rssi=ble_raw.get("rssi", -100),
                name=ble_raw.get("name", ""),
                seen_count=ble_raw.get("seen", 1),
                is_known=ble_raw.get("known", False),
            )

            sighting = BleSighting(
                device=ble_dev,
                node_id=node_id,
                node_ip=node_ip,
                node_wifi_rssi=node_rssi,
            )

            if mac not in presence_devices:
                presence_devices[mac] = BlePresence(
                    mac=mac,
                    name=ble_dev.name,
                    sightings=[sighting],
                    strongest_rssi=ble_dev.rssi,
                    node_count=1,
                )
            else:
                entry = presence_devices[mac]
                entry.sightings.append(sighting)
                entry.node_count = len(entry.sightings)
                if ble_dev.rssi > entry.strongest_rssi:
                    entry.strongest_rssi = ble_dev.rssi
                if ble_dev.name and not entry.name:
                    entry.name = ble_dev.name

    return BlePresenceMap(
        devices=presence_devices,
        total_devices=len(presence_devices),
        total_nodes=reporting_nodes,
    )


def _record_presence_to_sqlite(
    ble_store: "BleStore", devices: list[dict]
) -> None:
    """Record BLE sightings from fleet device snapshots into SQLite."""
    sightings = []
    for device in devices:
        ble_list = device.get("ble_devices", [])
        if not ble_list:
            continue
        node_id = device["device_id"]
        node_ip = device.get("ip", "")
        for ble_raw in ble_list:
            mac = ble_raw.get("mac", "")
            if not mac:
                continue
            sightings.append({
                "mac": mac,
                "name": ble_raw.get("name", ""),
                "rssi": ble_raw.get("rssi", -100),
                "node_id": node_id,
                "node_ip": node_ip,
                "is_known": ble_raw.get("known", False),
                "seen_count": ble_raw.get("seen", 1),
            })
    if sightings:
        ble_store.record_sightings_batch(sightings)


@router.get("/ble")
async def ble_presence(request: Request):
    """Aggregate BLE device sightings across all sensor nodes.

    Uses tritium-lib BlePresenceMap for structured aggregation.
    Returns a list of unique BLE MACs with per-node RSSI readings,
    suitable for triangulation.

    Side effect: records all sightings to SQLite via BleStore (if available).
    """
    store = _get_store(request)
    devices = store.list_devices()
    presence_map = _build_presence_map(devices)

    # Record sightings to SQLite for persistence
    ble_store = _get_ble_store(request)
    if ble_store is not None:
        _record_presence_to_sqlite(ble_store, devices)

    # Convert to API response format (backward compatible)
    results = []
    for mac, presence in sorted(
        presence_map.devices.items(),
        key=lambda kv: (-kv[1].node_count, kv[0]),
    ):
        sightings = []
        is_known = False
        for s in presence.sightings:
            if s.device.is_known:
                is_known = True
            sightings.append({
                "node_id": s.node_id,
                "node_ip": s.node_ip,
                "node_wifi_rssi": s.node_wifi_rssi,
                "ble_rssi": s.device.rssi,
                "seen_count": s.device.seen_count,
            })
        results.append({
            "mac": mac,
            "name": presence.name,
            "known": is_known,
            "sightings": sightings,
        })

    return {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "total_ble_macs": presence_map.total_devices,
        "reporting_nodes": presence_map.total_nodes,
        "devices": results,
    }


@router.get("/ble/active")
async def ble_active_devices(request: Request):
    """Active BLE devices from the persistent SQLite store.

    Returns devices seen in the last 2 minutes with per-node RSSI,
    tracking status, and estimated position (if enough nodes have
    known positions).
    """
    ble_store = _get_ble_store(request)
    if ble_store is None:
        return {"devices": [], "count": 0, "error": "BleStore not available"}

    devices = ble_store.get_active_devices()

    # Enrich with position estimates and tracking status
    if estimate_position is not None:
        node_positions_raw = ble_store.get_node_positions()
        node_positions: dict[str, tuple[float, float]] = {}
        for nid, pos in node_positions_raw.items():
            lat = pos.get("lat")
            lon = pos.get("lon")
            if lat is not None and lon is not None:
                node_positions[nid] = (lat, lon)

        targets = {t["mac"]: t for t in ble_store.list_targets()}

        for dev in devices:
            if len(dev.get("nodes", [])) >= 2 and node_positions:
                sightings = [
                    {"node_id": n["node_id"], "ble_rssi": n["rssi"]}
                    for n in dev["nodes"]
                ]
                pos_est = estimate_position(sightings, node_positions)
                if pos_est is not None:
                    dev["position"] = pos_est

            target = targets.get(dev["mac"])
            if target:
                dev["tracked"] = True
                dev["target_label"] = target.get("label", "")
                dev["target_color"] = target.get("color", "")
            else:
                dev["tracked"] = False

    return {"devices": devices, "count": len(devices)}


@router.get("/ble/targets")
async def ble_targets(request: Request):
    """List tracked BLE targets."""
    ble_store = _get_ble_store(request)
    if ble_store is None:
        return {"targets": [], "count": 0}
    targets = ble_store.list_targets()
    return {"targets": targets, "count": len(targets)}


class AddTargetRequest(BaseModel):
    mac: str
    label: str
    color: str = ""


@router.post("/ble/targets")
async def add_ble_target(body: AddTargetRequest, request: Request):
    """Add a tracked BLE target."""
    ble_store = _get_ble_store(request)
    if ble_store is None:
        return {"error": "BleStore not available"}
    target = ble_store.add_target(body.mac, body.label, body.color)
    return {"target": target}


@router.delete("/ble/targets/{mac:path}")
async def remove_ble_target(mac: str, request: Request):
    """Remove a tracked BLE target."""
    ble_store = _get_ble_store(request)
    if ble_store is None:
        return {"error": "BleStore not available"}
    removed = ble_store.remove_target(mac)
    return {"removed": removed, "mac": mac}


@router.get("/ble/history/{mac:path}")
async def ble_device_history(mac: str, request: Request, limit: int = 200):
    """Sighting history for a specific MAC from SQLite."""
    ble_store = _get_ble_store(request)
    if ble_store is None:
        return {"mac": mac, "history": [], "count": 0}
    history = ble_store.get_device_history(mac, limit=limit)
    return {"mac": mac, "history": history, "count": len(history)}


@router.get("/ble/summary")
async def ble_summary(request: Request):
    """Summary stats from the BLE sighting database."""
    ble_store = _get_ble_store(request)
    if ble_store is None:
        return {"error": "BleStore not available"}
    return ble_store.get_device_summary()


class SetNodePositionRequest(BaseModel):
    x: float
    y: float
    lat: Optional[float] = None
    lon: Optional[float] = None
    label: str = ""


@router.get("/nodes/positions")
async def node_positions(request: Request):
    """All sensor node positions for trilateration."""
    ble_store = _get_ble_store(request)
    if ble_store is None:
        return {"positions": {}, "count": 0}
    positions = ble_store.get_node_positions()
    return {"positions": positions, "count": len(positions)}


@router.put("/nodes/{node_id}/position")
async def set_node_position(
    node_id: str, body: SetNodePositionRequest, request: Request
):
    """Set a sensor node's position."""
    ble_store = _get_ble_store(request)
    if ble_store is None:
        return {"error": "BleStore not available"}
    ble_store.set_node_position(
        node_id, body.x, body.y,
        lat=body.lat, lon=body.lon, label=body.label,
    )
    return {"node_id": node_id, "set": True}


@router.delete("/nodes/{node_id}/position")
async def remove_node_position(node_id: str, request: Request):
    """Remove a sensor node's position."""
    ble_store = _get_ble_store(request)
    if ble_store is None:
        return {"error": "BleStore not available"}
    removed = ble_store.remove_node_position(node_id)
    return {"node_id": node_id, "removed": removed}


@router.get("/ble/{mac}")
async def ble_device_detail(mac: str, request: Request):
    """Get all sightings of a specific BLE MAC across nodes.

    Uses the JSON fleet store for the live snapshot.
    """
    store = _get_store(request)
    devices = store.list_devices()

    sightings = []
    name = ""
    known = False

    for device in devices:
        ble_list = device.get("ble_devices", [])
        for ble_dev in ble_list:
            if ble_dev.get("mac", "").upper() == mac.upper():
                if ble_dev.get("name"):
                    name = ble_dev["name"]
                if ble_dev.get("known"):
                    known = True
                sightings.append({
                    "node_id": device["device_id"],
                    "node_ip": device.get("ip", ""),
                    "node_wifi_rssi": device.get("rssi"),
                    "ble_rssi": ble_dev.get("rssi"),
                    "seen_count": ble_dev.get("seen", 0),
                    "last_seen": device.get("last_seen"),
                })

    return {
        "mac": mac.upper(),
        "name": name,
        "known": known,
        "sighting_count": len(sightings),
        "sightings": sightings,
    }
