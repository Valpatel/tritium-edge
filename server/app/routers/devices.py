# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Device management endpoints."""

import uuid
from datetime import datetime, timezone

from fastapi import APIRouter, HTTPException, Request

from ..models import CommandRequest, DeviceUpdate
from ..services.device_service import enrich_devices, process_heartbeat
from .ws import broadcast

router = APIRouter(prefix="/api", tags=["devices"])


def _get_store(request: Request):
    return request.app.state.store


# --- Admin device endpoints ---

@router.get("/devices")
async def list_devices(request: Request):
    return enrich_devices(_get_store(request).list_devices())


@router.get("/devices/{device_id}")
async def get_device(device_id: str, request: Request):
    d = _get_store(request).get_device(device_id)
    if not d:
        raise HTTPException(404, "Device not found")
    enrich_devices([d])
    return d


@router.patch("/devices/{device_id}")
async def update_device(device_id: str, body: DeviceUpdate, request: Request):
    store = _get_store(request)
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")
    updates = body.model_dump(exclude_none=True)
    for k, v in updates.items():
        device[k] = v
    store.save_device(device)
    store.add_event("device_updated", device_id, f"Updated: {', '.join(updates.keys())}")
    return device


@router.delete("/devices/{device_id}")
async def delete_device(device_id: str, request: Request):
    store = _get_store(request)
    store.add_event("device_deleted", device_id)
    store.delete_device(device_id)
    return {"status": "deleted"}


@router.post("/devices/{device_id}/reboot")
async def reboot_device(device_id: str, request: Request):
    store = _get_store(request)
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")
    device["pending_command"] = {
        "cmd": "reboot",
        "ts": datetime.now(timezone.utc).isoformat(),
    }
    store.save_device(device)
    store.add_event("reboot_scheduled", device_id)
    return {"status": "reboot_scheduled"}


@router.post("/devices/{device_id}/command")
async def send_command(device_id: str, body: CommandRequest, request: Request):
    store = _get_store(request)
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")
    cmd = {
        "id": f"cmd-{uuid.uuid4().hex[:8]}",
        "type": body.type,
        "payload": body.payload,
        "status": "pending",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "expires_at": datetime.fromtimestamp(
            datetime.now(timezone.utc).timestamp() + body.ttl_s, tz=timezone.utc
        ).isoformat(),
    }
    store.add_command(device_id, cmd)
    store.add_event("command_sent", device_id, f"{body.type}")
    return cmd


@router.get("/devices/{device_id}/config")
async def get_device_config(device_id: str, request: Request):
    """Get desired and reported config for a device."""
    store = _get_store(request)
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")
    return {
        "device_id": device_id,
        "desired_config": device.get("desired_config", {}),
        "reported_config": device.get("reported_config", {}),
    }


@router.put("/devices/{device_id}/config")
async def set_device_config(device_id: str, request: Request):
    """Set desired config for a device. Merged on next heartbeat."""
    store = _get_store(request)
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")
    body = await request.json()
    desired = device.get("desired_config", {})
    desired.update(body)
    device["desired_config"] = desired
    device["_config_drift_logged"] = False
    store.save_device(device)
    store.add_event("config_updated", device_id, f"Keys: {', '.join(body.keys())}")
    return {"status": "ok", "desired_config": desired}


# --- Device-facing heartbeat endpoint ---

@router.post("/devices/{device_id}/status")
async def device_heartbeat(device_id: str, request: Request):
    """Device heartbeat — accepts both v1 and v2 payloads."""
    try:
        from ..store.fleet_store import FleetStore
        FleetStore.safe_id(device_id)
    except ValueError:
        raise HTTPException(400, "Invalid device_id")
    body = await request.json()
    store = _get_store(request)
    is_new = store.get_device(device_id) is None
    result = process_heartbeat(store, device_id, body)
    if "error" in result:
        raise HTTPException(429, result["error"])

    # Broadcast to WebSocket clients (non-fatal)
    try:
        device = store.get_device(device_id)
        if device:
            enrich_devices([device])
            event_type = "device_registered" if is_new else "device_heartbeat"
            await broadcast(event_type, device)
    except Exception:
        pass  # WebSocket broadcast should never break heartbeat

    return result


# --- Device provisioning ---

@router.post("/device/provision")
async def device_provision(request: Request):
    """Device self-registration (v2 protocol)."""
    body = await request.json()
    store = _get_store(request)
    mac = body.get("mac", "")
    board = body.get("board", "unknown")
    device_id = mac.replace(":", "-") if mac else f"esp32-{uuid.uuid4().hex[:12]}"

    device = store.get_device(device_id)
    if not device:
        device = {
            "device_id": device_id,
            "registered_at": datetime.now(timezone.utc).isoformat(),
            "board": board,
            "family": body.get("family", "esp32"),
            "version": body.get("firmware_version", "unknown"),
            "capabilities": body.get("capabilities", []),
            "tags": ["auto-provisioned"],
            "notes": "",
            "provisioned": True,
        }
        store.save_device(device)
        store.add_event("device_provisioned", device_id, f"Board: {board}")

    return {
        "device_id": device_id,
        "heartbeat_interval_s": 60,
    }
