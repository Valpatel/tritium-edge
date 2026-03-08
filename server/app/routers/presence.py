# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""BLE presence aggregation — cross-node device tracking for triangulation."""

from datetime import datetime, timezone

from fastapi import APIRouter, Request

from tritium_lib.models.ble import (
    BleDevice,
    BleSighting,
    BlePresence,
    BlePresenceMap,
)

router = APIRouter(prefix="/api/presence", tags=["presence"])


def _get_store(request: Request):
    return request.app.state.store


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


@router.get("/ble")
async def ble_presence(request: Request):
    """Aggregate BLE device sightings across all sensor nodes.

    Uses tritium-lib BlePresenceMap for structured aggregation.
    Returns a list of unique BLE MACs with per-node RSSI readings,
    suitable for triangulation.
    """
    store = _get_store(request)
    devices = store.list_devices()
    presence_map = _build_presence_map(devices)

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


@router.get("/ble/{mac}")
async def ble_device_detail(mac: str, request: Request):
    """Get all sightings of a specific BLE MAC across nodes."""
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
