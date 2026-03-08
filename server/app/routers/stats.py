# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Fleet statistics endpoints."""

import time
from datetime import datetime, timezone

from fastapi import APIRouter, Request

from ..services.device_service import enrich_devices

router = APIRouter(prefix="/api", tags=["stats"])


@router.get("/stats")
async def fleet_stats(request: Request):
    store = request.app.state.store
    devices = enrich_devices(store.list_devices())
    firmware = store.list_firmware()
    attested = sum(1 for d in devices if d.get("fw_attested") is True)
    unattested = sum(1 for d in devices if d.get("fw_attested") is False)
    no_hash = sum(1 for d in devices if "fw_attested" not in d)
    return {
        "total_devices": len(devices),
        "online_devices": sum(1 for d in devices if d.get("_online")),
        "offline_devices": sum(1 for d in devices if not d.get("_online")),
        "pending_ota": sum(1 for d in devices if d.get("pending_ota")),
        "total_firmware": len(firmware),
        "signed_firmware": sum(1 for f in firmware if f.get("signed")),
        "boards": list(set(d.get("board", "unknown") for d in devices)),
        "families": list(set(d.get("family", "esp32") for d in devices)),
        "versions": list(set(d.get("version", "unknown") for d in devices)),
        "attested_devices": attested,
        "unattested_devices": unattested,
        "no_attestation": no_hash,
        "total_profiles": len(store.list_profiles()),
        "profiled_devices": sum(1 for d in devices if d.get("profile_id")),
        "config_drift": sum(1 for d in devices if d.get("_config_drift_logged")),
    }


@router.get("/fleet/status")
async def fleet_status(request: Request):
    """Aggregated fleet health for the admin dashboard."""
    store = request.app.state.store
    devices = enrich_devices(store.list_devices())
    total_ble = 0
    for d in devices:
        total_ble += len(d.get("ble_devices", []))
    start_time = getattr(request.app.state, "start_time", time.time())
    return {
        "total_nodes": len(devices),
        "online": sum(1 for d in devices if d.get("_online")),
        "offline": sum(1 for d in devices if not d.get("_online")),
        "total_ble_devices": total_ble,
        "server_uptime_s": int(time.time() - start_time),
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
