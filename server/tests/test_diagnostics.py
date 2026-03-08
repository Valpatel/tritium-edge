# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for fleet diagnostics endpoints."""

import pytest
from app.routers.diagnostics import _diag_cache


@pytest.fixture(autouse=True)
def clear_diag_cache():
    """Clear diagnostic cache between tests."""
    _diag_cache.clear()
    yield
    _diag_cache.clear()


def test_submit_diagnostics(client):
    """POST /api/devices/{id}/diag stores a report."""
    report = {
        "health": {
            "free_heap": 120000,
            "min_heap": 80000,
            "cpu_temp_c": 42.5,
            "uptime_s": 3600,
            "wifi_rssi": -55,
            "reboot_count": 2,
        },
        "anomalies": [],
    }
    resp = client.post("/api/devices/node-001/diag", json=report)
    assert resp.status_code == 200
    data = resp.json()
    assert data["status"] == "ok"
    assert data["anomalies_recorded"] == 0


def test_submit_diagnostics_with_anomalies(client):
    """POST with anomalies records them and logs events."""
    report = {
        "health": {"free_heap": 30000, "min_heap": 15000},
        "anomalies": [
            {
                "subsystem": "memory",
                "description": "Heap below 20KB threshold",
                "severity": 0.8,
                "detected_at": "2026-03-07T12:00:00Z",
            },
            {
                "subsystem": "wifi",
                "description": "RSSI degrading over time",
                "severity": 0.5,
            },
        ],
    }
    resp = client.post("/api/devices/node-002/diag", json=report)
    assert resp.status_code == 200
    assert resp.json()["anomalies_recorded"] == 2


def test_get_device_diagnostics(client):
    """GET /api/devices/{id}/diag returns latest report."""
    report = {"health": {"free_heap": 100000}, "anomalies": []}
    client.post("/api/devices/node-003/diag", json=report)

    resp = client.get("/api/devices/node-003/diag")
    assert resp.status_code == 200
    data = resp.json()
    assert data["device_id"] == "node-003"
    assert data["report"]["health"]["free_heap"] == 100000
    assert "received_at" in data


def test_get_device_diagnostics_404(client):
    """GET for unknown device returns 404."""
    resp = client.get("/api/devices/nonexistent/diag")
    assert resp.status_code == 404


def test_fleet_diagnostics_empty(client):
    """GET /api/fleet/diagnostics with no data."""
    resp = client.get("/api/fleet/diagnostics")
    assert resp.status_code == 200
    data = resp.json()
    assert data["total_nodes"] == 0
    assert data["nodes"] == []


def test_fleet_diagnostics_with_nodes(client):
    """GET /api/fleet/diagnostics aggregates across nodes."""
    # Submit diag for 3 nodes
    for i in range(3):
        client.post(f"/api/devices/node-{i:03d}/diag", json={
            "health": {
                "free_heap": 100000 - i * 20000,
                "min_heap": 80000 - i * 20000,
                "cpu_temp_c": 40 + i * 5,
                "uptime_s": 3600 * (i + 1),
                "reboot_count": i,
            },
            "anomalies": [{"subsystem": "test", "severity": 0.5}] * i,
        })

    resp = client.get("/api/fleet/diagnostics")
    assert resp.status_code == 200
    data = resp.json()
    assert data["total_nodes"] == 3
    assert data["total_anomalies"] == 3  # 0+1+2
    assert len(data["nodes"]) == 3


def test_fleet_diagnostics_critical_nodes(client):
    """Nodes with >3 anomalies or low heap are flagged critical."""
    # Node with many anomalies
    client.post("/api/devices/bad-node/diag", json={
        "health": {"free_heap": 100000, "min_heap": 80000},
        "anomalies": [{"subsystem": f"s{i}", "severity": 0.7} for i in range(5)],
    })
    # Node with low heap
    client.post("/api/devices/low-heap/diag", json={
        "health": {"free_heap": 15000, "min_heap": 10000},
        "anomalies": [],
    })
    # Healthy node
    client.post("/api/devices/good-node/diag", json={
        "health": {"free_heap": 200000, "min_heap": 150000},
        "anomalies": [],
    })

    resp = client.get("/api/fleet/diagnostics")
    data = resp.json()
    assert "bad-node" in data["critical_nodes"]
    assert "low-heap" in data["critical_nodes"]
    assert "good-node" not in data["critical_nodes"]


def test_fleet_anomalies_empty(client):
    """GET /api/fleet/anomalies with no data."""
    resp = client.get("/api/fleet/anomalies")
    assert resp.status_code == 200
    assert resp.json()["total"] == 0


def test_fleet_anomalies_sorted_by_severity(client):
    """Anomalies sorted by severity descending."""
    client.post("/api/devices/node-a/diag", json={
        "health": {},
        "anomalies": [
            {"subsystem": "wifi", "description": "Low signal", "severity": 0.3},
            {"subsystem": "memory", "description": "Heap critical", "severity": 0.9},
        ],
    })
    client.post("/api/devices/node-b/diag", json={
        "health": {},
        "anomalies": [
            {"subsystem": "temp", "description": "Overheating", "severity": 0.7},
        ],
    })

    resp = client.get("/api/fleet/anomalies")
    data = resp.json()
    assert data["total"] == 3
    severities = [a["severity"] for a in data["anomalies"]]
    assert severities == sorted(severities, reverse=True)
    assert data["anomalies"][0]["severity"] == 0.9


def test_fleet_anomalies_severity_filter(client):
    """Filter anomalies by minimum severity."""
    client.post("/api/devices/node-x/diag", json={
        "health": {},
        "anomalies": [
            {"subsystem": "a", "severity": 0.2},
            {"subsystem": "b", "severity": 0.6},
            {"subsystem": "c", "severity": 0.9},
        ],
    })

    resp = client.get("/api/fleet/anomalies?severity_min=0.5")
    data = resp.json()
    assert data["total"] == 2
    assert all(a["severity"] >= 0.5 for a in data["anomalies"])


def test_fleet_anomalies_limit(client):
    """Limit number of returned anomalies."""
    client.post("/api/devices/node-y/diag", json={
        "health": {},
        "anomalies": [{"subsystem": f"s{i}", "severity": 0.5} for i in range(20)],
    })

    resp = client.get("/api/fleet/anomalies?limit=5")
    data = resp.json()
    assert data["total"] == 20
    assert len(data["anomalies"]) == 5


def test_overwrite_diagnostic_report(client):
    """Submitting again overwrites previous report."""
    client.post("/api/devices/node-z/diag", json={
        "health": {"free_heap": 50000},
        "anomalies": [{"subsystem": "old"}],
    })
    client.post("/api/devices/node-z/diag", json={
        "health": {"free_heap": 200000},
        "anomalies": [],
    })

    resp = client.get("/api/devices/node-z/diag")
    data = resp.json()
    assert data["report"]["health"]["free_heap"] == 200000
    assert data["anomaly_count"] == 0
