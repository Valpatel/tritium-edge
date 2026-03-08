# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for diagnostic event log (diaglog) collection endpoints."""

import pytest
from app.routers.diagnostics import _diag_cache


@pytest.fixture(autouse=True)
def clear_state(app):
    """Clear diagnostic caches between tests."""
    _diag_cache.clear()
    # Reset diaglog store so each test gets a fresh one backed by tmp dir
    if hasattr(app.state, "diaglog_store"):
        del app.state.diaglog_store
    yield
    _diag_cache.clear()


# --- POST /api/devices/{device_id}/diag/log ---


def test_post_diag_log(client):
    """POST /api/devices/{id}/diag/log stores events."""
    body = {
        "events": [
            {
                "timestamp": 1709836800,
                "severity": "WARN",
                "subsystem": "i2c",
                "code": 3,
                "message": "Bus timeout on addr 0x34",
                "value": 0.0,
            },
        ],
        "boot_count": 5,
    }
    resp = client.post("/api/devices/node-001/diag/log", json=body)
    assert resp.status_code == 200
    data = resp.json()
    assert data["status"] == "ok"
    assert data["stored"] == 1


def test_post_diag_log_multiple_events(client):
    """POST with multiple events stores all of them."""
    events = [
        {"timestamp": 1709836800 + i, "severity": "INFO", "subsystem": "wifi",
         "code": i, "message": f"Event {i}", "value": float(i)}
        for i in range(5)
    ]
    resp = client.post("/api/devices/node-002/diag/log", json={
        "events": events, "boot_count": 1
    })
    assert resp.status_code == 200
    assert resp.json()["stored"] == 5


def test_post_diag_log_empty(client):
    """POST with empty events list returns stored=0."""
    resp = client.post("/api/devices/node-003/diag/log", json={
        "events": [], "boot_count": 0
    })
    assert resp.status_code == 200
    assert resp.json()["stored"] == 0


# --- GET /api/devices/{device_id}/diag/log ---


def test_get_device_diag_log(client):
    """GET returns stored events for a device."""
    body = {
        "events": [
            {"timestamp": 1709836800, "severity": "ERROR", "subsystem": "power",
             "code": 1, "message": "Undervoltage", "value": 3.1},
            {"timestamp": 1709836900, "severity": "WARN", "subsystem": "i2c",
             "code": 3, "message": "Bus timeout", "value": 0.0},
        ],
        "boot_count": 2,
    }
    client.post("/api/devices/node-010/diag/log", json=body)

    resp = client.get("/api/devices/node-010/diag/log")
    assert resp.status_code == 200
    data = resp.json()
    assert data["device_id"] == "node-010"
    assert data["count"] == 2
    # Sorted by timestamp desc
    assert data["events"][0]["timestamp"] == 1709836900
    assert data["events"][1]["timestamp"] == 1709836800


def test_get_device_diag_log_empty(client):
    """GET for device with no events returns empty list."""
    resp = client.get("/api/devices/no-events/diag/log")
    assert resp.status_code == 200
    data = resp.json()
    assert data["count"] == 0
    assert data["events"] == []


def test_get_device_diag_log_since_filter(client):
    """Filter events by timestamp."""
    events = [
        {"timestamp": 1000, "severity": "INFO", "subsystem": "test",
         "code": 0, "message": "old"},
        {"timestamp": 2000, "severity": "WARN", "subsystem": "test",
         "code": 1, "message": "new"},
        {"timestamp": 3000, "severity": "ERROR", "subsystem": "test",
         "code": 2, "message": "newest"},
    ]
    client.post("/api/devices/node-020/diag/log", json={
        "events": events, "boot_count": 1
    })

    resp = client.get("/api/devices/node-020/diag/log?since=2000")
    data = resp.json()
    assert data["count"] == 2
    assert all(e["timestamp"] >= 2000 for e in data["events"])


def test_get_device_diag_log_severity_filter(client):
    """Filter events by minimum severity."""
    events = [
        {"timestamp": 100, "severity": "DEBUG", "subsystem": "a",
         "code": 0, "message": "debug msg"},
        {"timestamp": 200, "severity": "INFO", "subsystem": "b",
         "code": 0, "message": "info msg"},
        {"timestamp": 300, "severity": "WARN", "subsystem": "c",
         "code": 0, "message": "warn msg"},
        {"timestamp": 400, "severity": "ERROR", "subsystem": "d",
         "code": 0, "message": "error msg"},
        {"timestamp": 500, "severity": "CRITICAL", "subsystem": "e",
         "code": 0, "message": "critical msg"},
    ]
    client.post("/api/devices/node-030/diag/log", json={
        "events": events, "boot_count": 1
    })

    resp = client.get("/api/devices/node-030/diag/log?severity=WARN")
    data = resp.json()
    assert data["count"] == 3
    severities = {e["severity"] for e in data["events"]}
    assert severities == {"WARN", "ERROR", "CRITICAL"}


def test_get_device_diag_log_limit(client):
    """Limit number of returned events."""
    events = [
        {"timestamp": i, "severity": "INFO", "subsystem": "test",
         "code": 0, "message": f"evt-{i}"}
        for i in range(50)
    ]
    client.post("/api/devices/node-040/diag/log", json={
        "events": events, "boot_count": 1
    })

    resp = client.get("/api/devices/node-040/diag/log?limit=10")
    data = resp.json()
    assert data["count"] == 10


# --- GET /api/fleet/diag/log ---


def test_fleet_diag_log(client):
    """Fleet-wide event log aggregates across devices."""
    for dev in ["dev-a", "dev-b"]:
        client.post(f"/api/devices/{dev}/diag/log", json={
            "events": [
                {"timestamp": 1000, "severity": "WARN", "subsystem": "i2c",
                 "code": 1, "message": f"from {dev}"},
            ],
            "boot_count": 1,
        })

    resp = client.get("/api/fleet/diag/log")
    assert resp.status_code == 200
    data = resp.json()
    assert data["count"] == 2
    device_ids = {e["device_id"] for e in data["events"]}
    assert device_ids == {"dev-a", "dev-b"}


def test_fleet_diag_log_device_filter(client):
    """Fleet log with device_id query param filters to one device."""
    for dev in ["dev-x", "dev-y"]:
        client.post(f"/api/devices/{dev}/diag/log", json={
            "events": [
                {"timestamp": 5000, "severity": "INFO", "subsystem": "test",
                 "code": 0, "message": f"from {dev}"},
            ],
            "boot_count": 1,
        })

    resp = client.get("/api/fleet/diag/log?device_id=dev-x")
    data = resp.json()
    assert data["count"] == 1
    assert data["events"][0]["device_id"] == "dev-x"


def test_fleet_diag_log_sorted_by_timestamp_desc(client):
    """Fleet events sorted by timestamp descending."""
    client.post("/api/devices/dev-1/diag/log", json={
        "events": [
            {"timestamp": 1000, "severity": "INFO", "subsystem": "a",
             "code": 0, "message": "early"},
        ],
        "boot_count": 1,
    })
    client.post("/api/devices/dev-2/diag/log", json={
        "events": [
            {"timestamp": 9000, "severity": "INFO", "subsystem": "b",
             "code": 0, "message": "late"},
        ],
        "boot_count": 1,
    })

    resp = client.get("/api/fleet/diag/log")
    data = resp.json()
    timestamps = [e["timestamp"] for e in data["events"]]
    assert timestamps == sorted(timestamps, reverse=True)


# --- GET /api/fleet/diag/summary ---


def test_fleet_diag_summary(client):
    """Summary returns per-device and per-subsystem counts."""
    client.post("/api/devices/sum-a/diag/log", json={
        "events": [
            {"timestamp": 1000, "severity": "WARN", "subsystem": "i2c",
             "code": 3, "message": "Bus timeout"},
            {"timestamp": 2000, "severity": "ERROR", "subsystem": "power",
             "code": 1, "message": "Undervoltage"},
        ],
        "boot_count": 1,
    })
    client.post("/api/devices/sum-b/diag/log", json={
        "events": [
            {"timestamp": 3000, "severity": "CRITICAL", "subsystem": "i2c",
             "code": 5, "message": "Bus hang"},
        ],
        "boot_count": 2,
    })

    resp = client.get("/api/fleet/diag/summary")
    assert resp.status_code == 200
    data = resp.json()

    assert data["total_events"] == 3
    assert data["per_device"]["sum-a"] == 2
    assert data["per_device"]["sum-b"] == 1
    assert data["per_subsystem"]["i2c"] == 2
    assert data["per_subsystem"]["power"] == 1

    # Most frequent errors: "Undervoltage" and "Bus hang" (both ERROR/CRITICAL)
    error_msgs = {e["message"] for e in data["most_frequent_errors"]}
    assert "Undervoltage" in error_msgs
    assert "Bus hang" in error_msgs

    # Critical devices
    assert "sum-b" in data["critical_devices"]


def test_fleet_diag_summary_empty(client):
    """Empty fleet returns sane defaults."""
    resp = client.get("/api/fleet/diag/summary")
    assert resp.status_code == 200
    data = resp.json()
    assert data["total_events"] == 0
    assert data["per_device"] == {}
    assert data["per_subsystem"] == {}
    assert data["most_frequent_errors"] == []
    assert data["critical_devices"] == []


def test_fleet_diag_summary_no_errors(client):
    """Fleet with only INFO/WARN events has no error messages."""
    client.post("/api/devices/info-node/diag/log", json={
        "events": [
            {"timestamp": 1000, "severity": "INFO", "subsystem": "wifi",
             "code": 0, "message": "Connected"},
            {"timestamp": 2000, "severity": "WARN", "subsystem": "i2c",
             "code": 1, "message": "Slow response"},
        ],
        "boot_count": 1,
    })

    resp = client.get("/api/fleet/diag/summary")
    data = resp.json()
    assert data["total_events"] == 2
    assert data["most_frequent_errors"] == []
    assert data["critical_devices"] == []
