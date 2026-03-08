# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Fleet statistics endpoints."""

import time
from datetime import datetime, timezone

from fastapi import APIRouter, Request

from tritium_lib.models.config import compute_fleet_config_status

from ..services.alert_service import get_alert_service
from ..services.device_service import enrich_devices, build_fleet_status, get_fleet_health

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


@router.get("/fleet/health")
async def fleet_health(request: Request):
    """Fleet health score (0.0-1.0) computed by tritium-lib.

    Factors: online ratio (50%), avg WiFi RSSI (25%), avg heap usage (25%).
    """
    store = request.app.state.store
    devices = enrich_devices(store.list_devices())
    fleet = build_fleet_status(devices)
    score = get_fleet_health(devices)
    start_time = getattr(request.app.state, "start_time", time.time())
    return {
        "health_score": score,
        "total_nodes": fleet.total_nodes,
        "online_count": fleet.online_count,
        "ble_total": fleet.ble_total,
        "server_uptime_s": int(time.time() - start_time),
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }


@router.get("/fleet/config")
async def fleet_config(request: Request):
    """Config sync status — desired vs reported drift across the fleet.

    Uses tritium-lib drift detection to classify mismatches as
    critical / moderate / minor.
    """
    store = request.app.state.store
    devices = enrich_devices(store.list_devices())

    # Build input for tritium-lib: each device needs desired + reported config
    config_entries = []
    for d in devices:
        profile_id = d.get("profile_id")
        desired = {}
        if profile_id:
            profile = store.get_profile(profile_id)
            if profile:
                desired = profile.get("config", {})
        reported = d.get("reported_config", d.get("config", {}))
        config_entries.append({
            "device_id": d.get("device_id", d.get("id", "?")),
            "desired_config": desired,
            "reported_config": reported,
        })

    status = compute_fleet_config_status(config_entries)

    # Build per-device drift details for drifted nodes
    drifted_details = []
    for dc in status.devices:
        if not dc.is_synced:
            drifted_details.append({
                "device_id": dc.device_id,
                "drift_count": dc.drift_count,
                "max_severity": dc.max_severity.value,
                "drifts": [
                    {
                        "key": drift.key,
                        "desired": drift.desired_value,
                        "reported": drift.reported_value,
                        "severity": drift.severity.value,
                        "missing": drift.is_missing,
                        "extra": drift.is_extra,
                    }
                    for drift in dc.drifts
                ],
            })

    return {
        "total_devices": status.total_devices,
        "synced_count": status.synced_count,
        "drifted_count": status.drifted_count,
        "critical_drift_count": status.critical_drift_count,
        "sync_ratio": round(status.sync_ratio, 3),
        "drifted_devices": drifted_details,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }


@router.get("/fleet/dashboard")
async def fleet_dashboard(request: Request):
    """Combined dashboard summary — health + config + alerts in one call.

    Reduces roundtrips for the admin UI by combining the most
    commonly-needed fleet metrics into a single response.
    """
    store = request.app.state.store
    devices = enrich_devices(store.list_devices())
    start_time = getattr(request.app.state, "start_time", time.time())

    # Fleet health
    fleet = build_fleet_status(devices)
    health_score = get_fleet_health(devices)

    # Config sync
    config_entries = []
    for d in devices:
        profile_id = d.get("profile_id")
        desired = {}
        if profile_id:
            profile = store.get_profile(profile_id)
            if profile:
                desired = profile.get("config", {})
        reported = d.get("reported_config", d.get("config", {}))
        config_entries.append({
            "device_id": d.get("device_id", d.get("id", "?")),
            "desired_config": desired,
            "reported_config": reported,
        })
    config_status = compute_fleet_config_status(config_entries)

    # Recent alerts
    alert_svc = get_alert_service()
    recent_alerts = []
    alert_counts = {"critical": 0, "warning": 0, "info": 0}
    if alert_svc:
        recent_alerts = alert_svc.get_history(limit=10)
        for a in recent_alerts:
            sev = a.get("severity", 0)
            if sev >= 0.7:
                alert_counts["critical"] += 1
            elif sev >= 0.4:
                alert_counts["warning"] += 1
            else:
                alert_counts["info"] += 1

    return {
        "health": {
            "score": round(health_score, 3),
            "total_nodes": fleet.total_nodes,
            "online_count": fleet.online_count,
            "ble_total": fleet.ble_total,
        },
        "config": {
            "synced_count": config_status.synced_count,
            "drifted_count": config_status.drifted_count,
            "critical_drift_count": config_status.critical_drift_count,
            "sync_ratio": round(config_status.sync_ratio, 3),
        },
        "alerts": {
            "recent_count": len(recent_alerts),
            "critical": alert_counts["critical"],
            "warning": alert_counts["warning"],
            "recent": recent_alerts[:5],
        },
        "server_uptime_s": int(time.time() - start_time),
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
