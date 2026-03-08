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

from tritium_lib.models.diagnostics import (
    AnomalyType,
    HealthSnapshot,
    NodeDiagReport,
    Anomaly as LibAnomaly,
    aggregate_fleet_health,
    classify_node_health,
    detect_fleet_anomalies,
)

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


def _cache_entry_to_report(device_id: str, entry: dict) -> NodeDiagReport:
    """Convert a cached diag entry to a tritium-lib NodeDiagReport."""
    report = entry["report"]
    health_data = report.get("health", {})

    health = HealthSnapshot(
        timestamp=datetime.fromtimestamp(entry["received_at"], tz=timezone.utc),
        node_id=device_id,
        free_heap=health_data.get("free_heap", 0),
        min_free_heap=health_data.get("min_heap", health_data.get("min_free_heap", 0)),
        free_psram=health_data.get("free_psram", 0),
        largest_free_block=health_data.get("largest_block", 0),
        battery_voltage=health_data.get("battery_v"),
        battery_percent=health_data.get("battery_pct"),
        cpu_temp_c=health_data.get("cpu_temp_c", health_data.get("cpu_c")),
        wifi_rssi=health_data.get("wifi_rssi"),
        wifi_connected=health_data.get("wifi_connected", False),
        wifi_disconnects=health_data.get("wifi_disconnects", 0),
        i2c_devices_found=health_data.get("i2c_devices", 0),
        i2c_errors=health_data.get("i2c_errors", 0),
        loop_time_us=health_data.get("loop_us", 0),
        max_loop_time_us=health_data.get("max_loop_us", 0),
        uptime_s=health_data.get("uptime_s", 0),
        reboot_count=health_data.get("reboot_count", 0),
        reset_reason=health_data.get("reset_reason"),
    )

    anomalies = []
    for a in entry.get("anomalies", []):
        anomalies.append(LibAnomaly(
            timestamp=health.timestamp,
            node_id=device_id,
            anomaly_type=_map_anomaly_type(a.get("subsystem", "")),
            subsystem=a.get("subsystem", "unknown"),
            description=a.get("description", ""),
            severity_score=min(1.0, max(0.0, a.get("severity", a.get("score", 0.5)))),
        ))

    return NodeDiagReport(
        node_id=device_id,
        board_type=report.get("board", "unknown"),
        firmware_version=report.get("version", "unknown"),
        current_health=health,
        active_anomalies=anomalies,
    )


def _map_anomaly_type(subsystem: str) -> AnomalyType:
    """Map a subsystem name to an AnomalyType enum."""
    mapping = {
        "memory": AnomalyType.MEMORY_LEAK,
        "power": AnomalyType.BATTERY_DRAIN,
        "wifi": AnomalyType.WIFI_DEGRADATION,
        "perf": AnomalyType.PERFORMANCE_DROP,
        "i2c": AnomalyType.I2C_FAILURE,
        "display": AnomalyType.DISPLAY_FAILURE,
        "thermal": AnomalyType.TEMPERATURE_HIGH,
    }
    return mapping.get(subsystem, AnomalyType.PERFORMANCE_DROP)


@router.get("/fleet/health-report")
async def fleet_health_report():
    """Fleet health report using tritium-lib classification.

    Returns healthy/warning/critical node counts, per-node status,
    and cross-node infrastructure anomaly detection.
    """
    reports = []
    for device_id, entry in _diag_cache.items():
        reports.append(_cache_entry_to_report(device_id, entry))

    summary = aggregate_fleet_health(reports)
    infra_anomalies = detect_fleet_anomalies(reports)

    node_statuses = []
    for report in reports:
        status = classify_node_health(report)
        node_statuses.append({
            "device_id": report.node_id,
            "status": status,
            "board": report.board_type,
            "version": report.firmware_version,
            "anomaly_count": len(report.active_anomalies),
            "free_heap": report.current_health.free_heap,
            "wifi_rssi": report.current_health.wifi_rssi,
            "uptime_s": report.current_health.uptime_s,
        })

    return {
        "health_score": round(summary.health_score, 3),
        "total_nodes": summary.total_nodes,
        "healthy_nodes": summary.healthy_nodes,
        "warning_nodes": summary.warning_nodes,
        "critical_nodes": summary.critical_nodes,
        "infrastructure_anomalies": [
            {
                "type": a.anomaly_type.value,
                "subsystem": a.subsystem,
                "description": a.description,
                "severity": round(a.severity_score, 2),
            }
            for a in infra_anomalies
        ],
        "nodes": node_statuses,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
