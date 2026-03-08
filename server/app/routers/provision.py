# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Node provisioning workflow endpoints — discover, configure, commission."""

import uuid
from datetime import datetime, timezone
from typing import Optional

from fastapi import APIRouter, HTTPException, Request
from pydantic import BaseModel, Field

router = APIRouter(prefix="/api/provision", tags=["provisioning"])


class ProvisionConfig(BaseModel):
    """Provisioning configuration for a discovered node."""
    device_id: str
    device_name: Optional[str] = None
    location: Optional[str] = None
    role: Optional[str] = None
    wifi_ssid: Optional[str] = None
    wifi_pass: Optional[str] = None
    server_url: Optional[str] = None
    tags: list[str] = Field(default_factory=list)


class BulkProvisionRequest(BaseModel):
    """Provision multiple nodes at once."""
    nodes: list[ProvisionConfig]


def _get_store(request: Request):
    return request.app.state.store


@router.get("/discovered")
async def list_discovered(request: Request):
    """List discovered but uncommissioned nodes.

    Combines:
      - Network-discovered nodes (from /api/commission/discover cache)
      - Auto-registered nodes with 'auto-registered' or 'pending' tags
      - Nodes marked as provisioned=False
    """
    store = _get_store(request)
    devices = store.list_devices()
    from ..services.device_service import enrich_devices
    enrich_devices(devices)

    discovered = []
    commissioned = []

    for d in devices:
        tags = d.get("tags", [])
        is_pending = (
            not d.get("provisioned")
            or "pending" in tags
            or "auto-registered" in tags
        )
        entry = {
            "device_id": d["device_id"],
            "device_name": d.get("device_name", ""),
            "board": d.get("board", "unknown"),
            "ip": d.get("ip"),
            "mac": d.get("mac"),
            "version": d.get("version", "unknown"),
            "capabilities": d.get("capabilities", []),
            "online": d.get("_online", False),
            "age": d.get("_age", "never"),
            "tags": tags,
            "provisioned": d.get("provisioned", False),
            "role": d.get("role", ""),
            "location_label": (d.get("location", {}) or {}).get("label", ""),
            "registered_at": d.get("registered_at", ""),
        }
        if is_pending:
            discovered.append(entry)
        else:
            commissioned.append(entry)

    return {
        "discovered": discovered,
        "commissioned": commissioned,
        "discovered_count": len(discovered),
        "commissioned_count": len(commissioned),
    }


@router.post("/commission")
async def commission_node(body: ProvisionConfig, request: Request):
    """Commission a single node — assign name, location, role, push config."""
    store = _get_store(request)
    device = store.get_device(body.device_id)
    if not device:
        raise HTTPException(404, "Device not found")

    # Update device metadata
    if body.device_name:
        device["device_name"] = body.device_name
    if body.location:
        device["location_label"] = body.location
    if body.role:
        device["role"] = body.role
    if body.tags:
        device["tags"] = body.tags
    else:
        # Remove pending tags, add commissioned
        tags = [t for t in device.get("tags", []) if t not in ("auto-registered", "pending")]
        if "commissioned" not in tags:
            tags.append("commissioned")
        device["tags"] = tags

    device["provisioned"] = True
    device["commissioned_at"] = datetime.now(timezone.utc).isoformat()

    # Build desired config to push on next heartbeat
    desired = device.get("desired_config", {})
    if body.server_url:
        desired["server_url"] = body.server_url
    if body.device_name:
        desired["device_name"] = body.device_name
    device["desired_config"] = desired

    store.save_device(device)

    # Save WiFi credentials to provisioning directory if provided
    if body.wifi_ssid:
        import json
        prov_dir = store.certs_dir / body.device_id
        prov_dir.mkdir(exist_ok=True)
        wifi = {"ssid": body.wifi_ssid, "password": body.wifi_pass or ""}
        (prov_dir / "factory_wifi.json").write_text(
            json.dumps(wifi, indent=2) + "\n"
        )

    store.add_event("node_commissioned", body.device_id,
                    f"Name: {body.device_name or device.get('device_name', '')}, "
                    f"Role: {body.role or 'unset'}")

    return {
        "status": "commissioned",
        "device_id": body.device_id,
        "device_name": device.get("device_name", ""),
        "provisioned": True,
    }


@router.post("/commission/bulk")
async def commission_bulk(body: BulkProvisionRequest, request: Request):
    """Commission multiple nodes at once."""
    store = _get_store(request)
    results = []

    for node_config in body.nodes:
        device = store.get_device(node_config.device_id)
        if not device:
            results.append({
                "device_id": node_config.device_id,
                "status": "not_found",
            })
            continue

        if node_config.device_name:
            device["device_name"] = node_config.device_name
        if node_config.location:
            device["location_label"] = node_config.location
        if node_config.role:
            device["role"] = node_config.role

        tags = [t for t in device.get("tags", []) if t not in ("auto-registered", "pending")]
        if "commissioned" not in tags:
            tags.append("commissioned")
        if node_config.tags:
            for t in node_config.tags:
                if t not in tags:
                    tags.append(t)
        device["tags"] = tags
        device["provisioned"] = True
        device["commissioned_at"] = datetime.now(timezone.utc).isoformat()

        store.save_device(device)
        results.append({
            "device_id": node_config.device_id,
            "status": "commissioned",
        })

    store.add_event("bulk_commission", detail=f"{len(results)} nodes processed")
    return {"results": results, "total": len(results)}


@router.post("/decommission/{device_id}")
async def decommission_node(device_id: str, request: Request):
    """Decommission a node — mark as unprovisioned."""
    store = _get_store(request)
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")

    device["provisioned"] = False
    tags = device.get("tags", [])
    tags = [t for t in tags if t != "commissioned"]
    if "decommissioned" not in tags:
        tags.append("decommissioned")
    device["tags"] = tags
    device.pop("role", None)
    store.save_device(device)

    store.add_event("node_decommissioned", device_id)
    return {"status": "decommissioned", "device_id": device_id}
