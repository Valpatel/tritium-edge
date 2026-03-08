# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Fleet diagnostics endpoints — per-node health reports and fleet-wide anomaly aggregation.

Devices POST their diagnostic reports via heartbeat or dedicated /api/diag endpoint.
The fleet server stores recent reports and exposes aggregated views for the dashboard.
"""

import json
import time
from datetime import datetime, timezone

from fastapi import APIRouter, HTTPException, Query, Request

router = APIRouter(prefix="/api", tags=["diagnostics"])

# In-memory diagnostic cache — keyed by device_id
# Each entry: {"report": {...}, "received_at": float, "anomalies": [...]}
_diag_cache: dict[str, dict] = {}
_MAX_CACHE = 200  # Max devices to track


def _store_diag(device_id: str, report: dict) -> dict:
    """Cache a diagnostic report for a device."""
    anomalies = report.get("anomalies", [])
    entry = {
        "report": report,
        "received_at": time.time(),
        "anomalies": anomalies,
        "anomaly_count": len(anomalies),
    }
    _diag_cache[device_id] = entry

    # Evict oldest if over limit
    if len(_diag_cache) > _MAX_CACHE:
        oldest = min(_diag_cache, key=lambda k: _diag_cache[k]["received_at"])
        del _diag_cache[oldest]

    return entry


@router.post("/devices/{device_id}/diag")
async def submit_diagnostics(device_id: str, request: Request):
    """Device submits its diagnostic report."""
    from .ws import broadcast

    body = await request.json()
    entry = _store_diag(device_id, body)

    # Persist to disk for historical analysis
    store = request.app.state.store
    store.save_diagnostic(device_id, body)

    # Log anomalies as fleet events
    anomalies = body.get("anomalies", [])
    if anomalies:
        detail = "; ".join(
            f"{a.get('subsystem', '?')}: {a.get('description', '?')}"
            for a in anomalies[:5]
        )
        store.add_event("node_anomaly", device_id, detail)

        # Fire alerts for critical anomalies (severity >= 0.7)
        from ..services.alert_service import get_alert_service
        alert_svc = get_alert_service()
        if alert_svc:
            for a in anomalies:
                sev = a.get("severity", a.get("score", 0.5))
                if sev >= 0.7:
                    alert_svc.fire_alert(
                        event_type="node_anomaly",
                        device_id=device_id,
                        detail=f"{a.get('subsystem', '?')}: {a.get('description', '?')}",
                        severity=sev,
                    )

    # Broadcast to WebSocket clients for real-time dashboard updates
    await broadcast("node_diag", {
        "device_id": device_id,
        "anomaly_count": entry["anomaly_count"],
        "health": body.get("health", {}),
    })
    if anomalies:
        await broadcast("node_anomaly", {
            "device_id": device_id,
            "anomalies": anomalies[:10],
            "count": len(anomalies),
        })

    return {
        "status": "ok",
        "anomalies_recorded": entry["anomaly_count"],
    }


@router.get("/devices/{device_id}/diag")
async def get_device_diagnostics(device_id: str):
    """Get latest diagnostic report for a specific device."""
    entry = _diag_cache.get(device_id)
    if not entry:
        raise HTTPException(404, "No diagnostic data for this device")
    return {
        "device_id": device_id,
        "received_at": datetime.fromtimestamp(
            entry["received_at"], tz=timezone.utc
        ).isoformat(),
        "report": entry["report"],
        "anomaly_count": entry["anomaly_count"],
    }


@router.get("/devices/{device_id}/diag/history")
async def get_device_diagnostics_history(
    device_id: str,
    request: Request,
    limit: int = Query(50),
):
    """Get diagnostic history for a device (persisted on disk)."""
    store = request.app.state.store
    return store.get_diagnostics(device_id, limit=limit)


@router.get("/fleet/diagnostics")
async def fleet_diagnostics():
    """Aggregated diagnostic overview for all nodes."""
    now = time.time()
    nodes = []
    total_anomalies = 0
    critical_nodes = []
    stale_nodes = []

    for device_id, entry in _diag_cache.items():
        age_s = now - entry["received_at"]
        report = entry["report"]
        health = report.get("health", {})

        node_summary = {
            "device_id": device_id,
            "age_s": round(age_s, 1),
            "anomaly_count": entry["anomaly_count"],
            "free_heap": health.get("free_heap"),
            "min_heap": health.get("min_heap"),
            "cpu_temp_c": health.get("cpu_temp_c"),
            "uptime_s": health.get("uptime_s"),
            "reboot_count": health.get("reboot_count"),
            "wifi_rssi": health.get("wifi_rssi"),
        }
        nodes.append(node_summary)
        total_anomalies += entry["anomaly_count"]

        # Flag critical: >3 anomalies or very low heap
        if entry["anomaly_count"] > 3:
            critical_nodes.append(device_id)
        elif health.get("min_heap") and health["min_heap"] < 20000:
            critical_nodes.append(device_id)

        # Flag stale: no report in >120s
        if age_s > 120:
            stale_nodes.append(device_id)

    return {
        "total_nodes": len(nodes),
        "total_anomalies": total_anomalies,
        "critical_nodes": critical_nodes,
        "stale_nodes": stale_nodes,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "nodes": nodes,
    }


@router.get("/fleet/anomalies")
async def fleet_anomalies(
    severity_min: float = Query(0.0, description="Minimum severity score"),
    limit: int = Query(50),
):
    """All anomalies across the fleet, sorted by severity."""
    all_anomalies = []
    for device_id, entry in _diag_cache.items():
        for anomaly in entry["anomalies"]:
            score = anomaly.get("severity", anomaly.get("score", 0.5))
            if score >= severity_min:
                all_anomalies.append({
                    "device_id": device_id,
                    "subsystem": anomaly.get("subsystem", "unknown"),
                    "description": anomaly.get("description", ""),
                    "severity": score,
                    "detected_at": anomaly.get("detected_at", ""),
                })

    all_anomalies.sort(key=lambda a: a["severity"], reverse=True)
    return {
        "total": len(all_anomalies),
        "anomalies": all_anomalies[:limit],
    }
