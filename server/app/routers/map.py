# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Map endpoints — node location management for fleet map view."""

from fastapi import APIRouter, HTTPException, Request
from pydantic import BaseModel
from typing import Optional

router = APIRouter(prefix="/api/map", tags=["map"])


class NodeLocation(BaseModel):
    """Manual or GPS-derived location for a node."""
    lat: float
    lng: float
    label: Optional[str] = None


def _get_store(request: Request):
    return request.app.state.store


@router.get("/nodes")
async def map_nodes(request: Request):
    """Get all nodes with location data for the fleet map.

    Location sources (priority):
      1. Heartbeat GPS data (gps_lat, gps_lng)
      2. Manually set location (location.lat, location.lng)

    Nodes without any location data are returned with location=null
    so the UI can offer manual placement.
    """
    store = _get_store(request)
    devices = store.list_devices()
    from ..services.device_service import enrich_devices
    enrich_devices(devices)

    nodes = []
    for d in devices:
        # Priority: heartbeat GPS > manual location
        lat = d.get("gps_lat") or (d.get("location", {}) or {}).get("lat")
        lng = d.get("gps_lng") or (d.get("location", {}) or {}).get("lng")
        label = (d.get("location", {}) or {}).get("label", "")

        nodes.append({
            "device_id": d["device_id"],
            "device_name": d.get("device_name", ""),
            "board": d.get("board", "unknown"),
            "ip": d.get("ip"),
            "version": d.get("version", "unknown"),
            "capabilities": d.get("capabilities", []),
            "online": d.get("_online", False),
            "age": d.get("_age", "never"),
            "rssi": d.get("rssi"),
            "location": {"lat": lat, "lng": lng, "label": label} if lat is not None and lng is not None else None,
        })

    return {"nodes": nodes, "count": len(nodes)}


@router.put("/nodes/{device_id}/location")
async def set_node_location(device_id: str, body: NodeLocation, request: Request):
    """Manually set or update a node's map location."""
    store = _get_store(request)
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")

    device["location"] = {
        "lat": body.lat,
        "lng": body.lng,
        "label": body.label or "",
    }
    store.save_device(device)
    store.add_event("location_set", device_id,
                    f"Lat: {body.lat:.6f}, Lng: {body.lng:.6f}")
    return {"status": "ok", "device_id": device_id, "location": device["location"]}


@router.delete("/nodes/{device_id}/location")
async def clear_node_location(device_id: str, request: Request):
    """Remove a node's manually-set location."""
    store = _get_store(request)
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")
    device.pop("location", None)
    store.save_device(device)
    return {"status": "cleared"}
