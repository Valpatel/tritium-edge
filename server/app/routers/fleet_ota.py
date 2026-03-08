# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Fleet OTA management endpoints — batch OTA operations with filtering."""

import uuid
from datetime import datetime, timezone
from typing import Optional

from fastapi import APIRouter, HTTPException, Request
from pydantic import BaseModel, Field

from .ws import broadcast

router = APIRouter(prefix="/api/fleet-ota", tags=["fleet-ota"])


class FleetOTARequest(BaseModel):
    """Request to push OTA to multiple nodes."""
    firmware_id: str
    device_ids: list[str] = Field(default_factory=list)
    board_filter: Optional[str] = None


class FleetOTAStatus(BaseModel):
    """Status of a fleet OTA rollout."""
    rollout_id: str
    firmware_id: str
    firmware_version: str
    total: int
    scheduled: int
    results: list[dict]


def _get_store(request: Request):
    return request.app.state.store


@router.get("/targets")
async def list_ota_targets(request: Request, board: str = None):
    """List devices eligible for OTA, optionally filtered by board type.

    Returns devices grouped by board type with current firmware info.
    """
    store = _get_store(request)
    from ..services.device_service import enrich_devices
    devices = enrich_devices(store.list_devices())

    targets = []
    for d in devices:
        if board and d.get("board", "unknown").lower() != board.lower():
            continue
        targets.append({
            "device_id": d["device_id"],
            "device_name": d.get("device_name", ""),
            "board": d.get("board", "unknown"),
            "version": d.get("version", "unknown"),
            "online": d.get("_online", False),
            "age": d.get("_age", "never"),
            "pending_ota": d.get("pending_ota"),
            "fw_attested": d.get("fw_attested"),
        })

    # Group by board for the UI
    boards = {}
    for t in targets:
        b = t["board"]
        if b not in boards:
            boards[b] = []
        boards[b].append(t)

    return {
        "targets": targets,
        "by_board": boards,
        "boards": list(boards.keys()),
        "total": len(targets),
    }


@router.post("/push")
async def fleet_ota_push(body: FleetOTARequest, request: Request):
    """Push OTA firmware to selected nodes.

    Targets can be specified by:
      - device_ids: explicit list of device IDs
      - board_filter: push to all devices matching a board type
      - Both: push to devices matching both criteria
    """
    store = _get_store(request)

    fw = store.get_firmware(body.firmware_id)
    if not fw:
        raise HTTPException(404, f"Firmware {body.firmware_id} not found")

    devices = store.list_devices()
    targets = []

    for d in devices:
        # Apply filters
        if body.device_ids and d["device_id"] not in body.device_ids:
            continue
        if body.board_filter and d.get("board", "unknown").lower() != body.board_filter.lower():
            continue
        if not body.device_ids and not body.board_filter:
            # No filter = push to all
            pass
        targets.append(d)

    if not targets:
        raise HTTPException(400, "No matching target devices")

    rollout_id = f"rollout-{uuid.uuid4().hex[:8]}"
    results = []
    scheduled = 0

    for device in targets:
        device["pending_ota"] = {
            "firmware_id": body.firmware_id,
            "version": fw["version"],
            "size": fw["total_size"],
            "url": f"/api/firmware/{body.firmware_id}/download",
            "scheduled_at": datetime.now(timezone.utc).isoformat(),
            "rollout_id": rollout_id,
        }
        store.save_device(device)
        scheduled += 1
        results.append({
            "device_id": device["device_id"],
            "status": "scheduled",
            "board": device.get("board", "unknown"),
        })

    fw["deploy_count"] = fw.get("deploy_count", 0) + scheduled
    store.save_firmware_meta(fw)

    store.add_event("fleet_ota_push", detail=f"{fw['version']} -> {scheduled} devices (rollout {rollout_id})")
    await broadcast("fleet_ota_push", {
        "rollout_id": rollout_id,
        "firmware_id": body.firmware_id,
        "version": fw["version"],
        "scheduled": scheduled,
    })

    return {
        "rollout_id": rollout_id,
        "firmware_id": body.firmware_id,
        "firmware_version": fw["version"],
        "total": len(targets),
        "scheduled": scheduled,
        "results": results,
    }


@router.get("/status")
async def fleet_ota_status(request: Request):
    """Get OTA status across all devices — pending, completed, failed."""
    store = _get_store(request)
    from ..services.device_service import enrich_devices
    devices = enrich_devices(store.list_devices())

    pending = []
    completed = []
    for d in devices:
        ota = d.get("pending_ota")
        if ota:
            pending.append({
                "device_id": d["device_id"],
                "device_name": d.get("device_name", ""),
                "board": d.get("board", "unknown"),
                "current_version": d.get("version", "unknown"),
                "target_version": ota.get("version", "unknown"),
                "firmware_id": ota.get("firmware_id"),
                "scheduled_at": ota.get("scheduled_at"),
                "rollout_id": ota.get("rollout_id"),
                "online": d.get("_online", False),
            })

    return {
        "pending": pending,
        "pending_count": len(pending),
        "total_devices": len(devices),
    }


@router.post("/cancel")
async def fleet_ota_cancel(request: Request):
    """Cancel all pending OTA deployments."""
    store = _get_store(request)
    devices = store.list_devices()
    cancelled = 0
    for d in devices:
        if "pending_ota" in d:
            del d["pending_ota"]
            store.save_device(d)
            cancelled += 1

    store.add_event("fleet_ota_cancelled", detail=f"Cancelled {cancelled} pending deployments")
    return {"status": "cancelled", "cancelled": cancelled}
