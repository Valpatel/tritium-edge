# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""OTA push and fleet rollout endpoints."""

from datetime import datetime, timezone

from fastapi import APIRouter, HTTPException, Request

from ..models import OTAPushRequest
from .ws import broadcast

router = APIRouter(prefix="/api", tags=["ota"])


def _get_store(request: Request):
    return request.app.state.store


@router.post("/ota/push/{device_id}")
async def ota_push(device_id: str, body: OTAPushRequest, request: Request):
    store = _get_store(request)
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")

    fw_id = body.firmware_id
    if body.latest:
        fw = store.get_latest_firmware(device.get("board"))
        if not fw:
            raise HTTPException(404, "No firmware available")
        fw_id = fw["id"]
    if not fw_id:
        raise HTTPException(400, "firmware_id or latest required")

    fw = store.get_firmware(fw_id)
    if not fw:
        raise HTTPException(404, f"Firmware {fw_id} not found")

    device["pending_ota"] = {
        "firmware_id": fw_id,
        "version": fw["version"],
        "size": fw["total_size"],
        "url": f"/api/firmware/{fw_id}/download",
        "scheduled_at": datetime.now(timezone.utc).isoformat(),
    }
    store.save_device(device)
    fw["deploy_count"] = fw.get("deploy_count", 0) + 1
    store.save_firmware_meta(fw)
    store.add_event("ota_scheduled", device_id, f"Firmware {fw['version']} ({fw_id})")
    await broadcast("ota_scheduled", {"device_id": device_id, "firmware_id": fw_id, "version": fw["version"]})
    return {"status": "scheduled", "device_id": device_id, "firmware_id": fw_id}


@router.post("/ota/push-all")
async def ota_push_all(body: OTAPushRequest, request: Request):
    store = _get_store(request)
    fw_id = body.firmware_id
    if body.latest:
        fw = store.get_latest_firmware()
        if not fw:
            raise HTTPException(404, "No firmware available")
        fw_id = fw["id"]
    if not fw_id:
        raise HTTPException(400, "firmware_id or latest required")
    fw = store.get_firmware(fw_id)
    if not fw:
        raise HTTPException(404)

    devices = store.list_devices()
    updated = 0
    for device in devices:
        device["pending_ota"] = {
            "firmware_id": fw_id,
            "version": fw["version"],
            "size": fw["total_size"],
            "url": f"/api/firmware/{fw_id}/download",
            "scheduled_at": datetime.now(timezone.utc).isoformat(),
        }
        store.save_device(device)
        updated += 1

    fw["deploy_count"] = fw.get("deploy_count", 0) + updated
    store.save_firmware_meta(fw)
    store.add_event("ota_fleet_push", detail=f"{fw['version']} -> {updated} devices")
    return {"status": "scheduled", "devices_updated": updated, "firmware_id": fw_id}


@router.post("/ota/clear/{device_id}")
async def ota_clear(device_id: str, request: Request):
    store = _get_store(request)
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404)
    device.pop("pending_ota", None)
    store.save_device(device)
    return {"status": "cleared"}
