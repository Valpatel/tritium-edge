# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""BLE presence aggregation — cross-node device tracking for triangulation."""

from datetime import datetime, timezone

from fastapi import APIRouter, Request

router = APIRouter(prefix="/api/presence", tags=["presence"])


def _get_store(request: Request):
    return request.app.state.store


@router.get("/ble")
async def ble_presence(request: Request):
    """Aggregate BLE device sightings across all sensor nodes.

    Returns a list of unique BLE MACs with per-node RSSI readings,
    suitable for triangulation. Each entry includes:
      - mac: BLE device MAC address
      - name: advertised name (if any)
      - known: whether it matches a known device
      - sightings: list of {node_id, rssi, node_ip} from each reporting node
    """
    store = _get_store(request)
    devices = store.list_devices()

    # Collect BLE sightings per MAC across all nodes
    mac_map: dict[str, dict] = {}

    for device in devices:
        ble_list = device.get("ble_devices", [])
        if not ble_list:
            continue

        node_id = device["device_id"]
        node_ip = device.get("ip", "")
        node_rssi = device.get("rssi")  # WiFi RSSI of the node itself

        for ble_dev in ble_list:
            mac = ble_dev.get("mac", "")
            if not mac:
                continue

            if mac not in mac_map:
                mac_map[mac] = {
                    "mac": mac,
                    "name": ble_dev.get("name", ""),
                    "known": ble_dev.get("known", False),
                    "sightings": [],
                }

            # Update name if we have one and existing doesn't
            if ble_dev.get("name") and not mac_map[mac]["name"]:
                mac_map[mac]["name"] = ble_dev["name"]

            # Mark as known if any node reports it
            if ble_dev.get("known"):
                mac_map[mac]["known"] = True

            mac_map[mac]["sightings"].append({
                "node_id": node_id,
                "node_ip": node_ip,
                "node_wifi_rssi": node_rssi,
                "ble_rssi": ble_dev.get("rssi"),
                "seen_count": ble_dev.get("seen", 0),
            })

    # Sort by number of sightings (most-seen first), then by MAC
    results = sorted(mac_map.values(),
                     key=lambda x: (-len(x["sightings"]), x["mac"]))

    return {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "total_ble_macs": len(results),
        "reporting_nodes": sum(1 for d in devices if d.get("ble_devices")),
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
