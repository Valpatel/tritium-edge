# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Integration tests: firmware heartbeat -> fleet server -> tritium-lib model pipeline.

Validates that data flowing from ESP32 devices through the fleet server uses
the correct tritium-lib model schemas at every stage.
"""

import pytest
from app.routers.diagnostics import _diag_cache

from tritium_lib.models.fleet import FleetNode, NodeStatus
from tritium_lib.models.correlation import CorrelationEvent, CorrelationType
from tritium_lib.models.diagnostics import HealthSnapshot


@pytest.fixture(autouse=True)
def clear_diag_cache():
    """Clear diagnostic cache between tests."""
    _diag_cache.clear()
    yield
    _diag_cache.clear()


# ---------------------------------------------------------------------------
# 1. Heartbeat creates health snapshot
# ---------------------------------------------------------------------------


def test_heartbeat_creates_health_snapshot(client, store):
    """POST a heartbeat payload matching firmware format, verify it's stored
    and can be retrieved with health snapshot fields matching tritium-lib
    HealthSnapshot schema."""

    # Register device via heartbeat (firmware format)
    hb_resp = client.post("/api/devices/pipeline-node-01/status", json={
        "firmware_version": "1.2.0",
        "board": "touch-lcd-35bc",
        "ip_address": "192.168.1.50",
        "mac": "20:6E:F1:9A:12:00",
        "uptime_s": 7200,
        "free_heap": 150000,
        "wifi_rssi": -52,
        "capabilities": ["display", "camera", "imu"],
    })
    assert hb_resp.status_code == 200
    assert hb_resp.json()["status"] == "ok"

    # Submit diagnostics with full health snapshot data
    diag_resp = client.post("/api/devices/pipeline-node-01/diag", json={
        "health": {
            "free_heap": 150000,
            "min_heap": 120000,
            "free_psram": 4000000,
            "largest_block": 100000,
            "battery_pct": 85.5,
            "cpu_temp_c": 42.0,
            "wifi_rssi": -52,
            "wifi_connected": True,
            "i2c_devices": 6,
            "i2c_errors": 0,
            "uptime_s": 7200,
            "reboot_count": 0,
        },
        "anomalies": [],
        "board": "touch-lcd-35bc",
        "version": "1.2.0",
    })
    assert diag_resp.status_code == 200
    assert diag_resp.json()["status"] == "ok"

    # Retrieve via health report and validate against tritium-lib HealthSnapshot schema
    report_resp = client.get("/api/fleet/health-report")
    assert report_resp.status_code == 200
    data = report_resp.json()
    assert data["total_nodes"] == 1

    node = data["nodes"][0]
    assert node["device_id"] == "pipeline-node-01"
    assert node["free_heap"] == 150000
    assert node["min_free_heap"] == 120000
    assert node["wifi_connected"] is True

    # Validate HealthSnapshot can be constructed from the same source data
    snapshot = HealthSnapshot(
        timestamp="2026-03-08T12:00:00Z",
        node_id="pipeline-node-01",
        free_heap=node["free_heap"],
        min_free_heap=node["min_free_heap"],
        free_psram=0,
        largest_free_block=0,
        wifi_rssi=node["wifi_rssi"],
        wifi_connected=node["wifi_connected"],
        uptime_s=node["uptime_s"],
        reboot_count=node["reboot_count"],
    )
    assert snapshot.free_heap == 150000
    assert snapshot.wifi_connected is True
    assert snapshot.uptime_s == 7200


# ---------------------------------------------------------------------------
# 2. Health report uses lib models
# ---------------------------------------------------------------------------


def test_health_report_uses_lib_models(client, store):
    """Register a device, POST heartbeat, GET /api/fleet/health-report and
    verify the response contains classification fields (healthy/warning/critical)."""

    # Register via heartbeat
    client.post("/api/devices/health-node-01/status", json={
        "firmware_version": "1.0.0",
        "board": "touch-amoled-241b",
        "ip_address": "192.168.1.60",
    })

    # Submit healthy diagnostics
    client.post("/api/devices/health-node-01/diag", json={
        "health": {
            "free_heap": 200000,
            "min_heap": 180000,
            "free_psram": 6000000,
            "largest_block": 150000,
            "wifi_rssi": -40,
            "wifi_connected": True,
            "uptime_s": 3600,
            "reboot_count": 0,
        },
        "anomalies": [],
    })

    # Submit warning-level diagnostics (WiFi disconnected)
    client.post("/api/devices/health-node-02/status", json={
        "firmware_version": "1.0.0",
        "board": "touch-lcd-349",
    })
    client.post("/api/devices/health-node-02/diag", json={
        "health": {
            "free_heap": 100000,
            "min_heap": 80000,
            "free_psram": 4000000,
            "largest_block": 60000,
            "wifi_rssi": -75,
            "wifi_connected": False,
            "uptime_s": 1800,
            "reboot_count": 0,
        },
        "anomalies": [],
    })

    # Submit critical diagnostics (low heap + high severity anomaly)
    client.post("/api/devices/health-node-03/status", json={
        "firmware_version": "1.0.0",
        "board": "touch-lcd-35bc",
    })
    client.post("/api/devices/health-node-03/diag", json={
        "health": {
            "free_heap": 10000,
            "min_heap": 5000,
            "free_psram": 0,
            "largest_block": 4000,
            "wifi_connected": True,
            "uptime_s": 600,
            "reboot_count": 5,
        },
        "anomalies": [
            {"subsystem": "memory", "description": "Heap critical", "severity": 0.95},
        ],
    })

    # Get health report
    resp = client.get("/api/fleet/health-report")
    assert resp.status_code == 200
    data = resp.json()

    # Validate classification fields exist and use correct schema
    assert "health_score" in data
    assert "total_nodes" in data
    assert "healthy_nodes" in data
    assert "warning_nodes" in data
    assert "critical_nodes" in data
    assert isinstance(data["health_score"], float)
    assert 0.0 <= data["health_score"] <= 1.0

    assert data["total_nodes"] == 3
    assert data["healthy_nodes"] >= 1
    assert data["warning_nodes"] >= 1
    assert data["critical_nodes"] >= 1

    # Verify per-node status classification
    statuses = {n["device_id"]: n["status"] for n in data["nodes"]}
    assert statuses["health-node-01"] == "healthy"
    assert statuses["health-node-02"] == "warning"
    assert statuses["health-node-03"] == "critical"


# ---------------------------------------------------------------------------
# 3. Correlations return typed events
# ---------------------------------------------------------------------------


def test_correlations_return_typed_events(client):
    """GET /api/fleet/correlations and verify the response includes type,
    confidence, devices_involved fields matching tritium-lib CorrelationEvent schema."""

    # Submit diagnostic snapshots with anomalies to multiple devices
    for i in range(4):
        client.post(f"/api/devices/corr-typed-{i}/diag", json={
            "health": {
                "free_heap": 120000,
                "uptime_s": 3600,
                "reboot_count": 0,
            },
            "anomalies": [
                {
                    "subsystem": "memory",
                    "description": f"Test anomaly for device {i}",
                    "severity": 0.6,
                    "detected_at": f"2026-03-08T12:0{i}:00Z",
                },
            ],
        })

    resp = client.get("/api/fleet/correlations")
    assert resp.status_code == 200
    data = resp.json()

    # Validate top-level schema fields
    assert "total_correlations" in data
    assert "high_confidence" in data
    assert "affected_devices" in data
    assert "correlations" in data
    assert "summary" in data
    assert isinstance(data["total_correlations"], int)
    assert isinstance(data["correlations"], list)

    # Validate summary has the expected tritium-lib summarize_correlations structure
    summary = data["summary"]
    assert "total" in summary
    assert "high_confidence" in summary
    assert "by_type" in summary
    assert "affected_devices" in summary

    # Validate each correlation event matches CorrelationEvent schema
    for corr in data["correlations"]:
        assert "type" in corr
        assert "description" in corr
        assert "devices_involved" in corr
        assert "confidence" in corr
        assert "timestamp" in corr
        assert "severity" in corr  # Added by the endpoint

        # Verify type is a valid CorrelationType value
        assert corr["type"] in [ct.value for ct in CorrelationType]
        assert 0.0 <= corr["confidence"] <= 1.0
        assert isinstance(corr["devices_involved"], list)

        # Validate that the raw dict can instantiate a CorrelationEvent
        evt = CorrelationEvent(
            type=CorrelationType(corr["type"]),
            description=corr["description"],
            devices_involved=corr["devices_involved"],
            confidence=corr["confidence"],
            timestamp=corr["timestamp"],
        )
        assert evt.type in CorrelationType
        assert len(evt.devices_involved) >= 0


# ---------------------------------------------------------------------------
# 4. Map nodes include heartbeat data
# ---------------------------------------------------------------------------


def test_map_nodes_include_heartbeat_data(client, store):
    """Register device with GPS heartbeat, verify /api/map/nodes returns
    the GPS location."""

    # Register device with GPS coordinates stored in device record
    store.save_device({
        "device_id": "gps-pipeline-01",
        "registered_at": "2026-01-01T00:00:00+00:00",
        "board": "touch-lcd-35bc",
        "version": "1.2.0",
        "ip": "192.168.1.70",
        "mac": "20:6E:F1:9A:12:01",
        "capabilities": ["display", "gps"],
        "gps_lat": 40.7128,
        "gps_lng": -74.0060,
    })

    # Send heartbeat to update last_seen
    client.post("/api/devices/gps-pipeline-01/status", json={
        "firmware_version": "1.2.0",
        "board": "touch-lcd-35bc",
        "ip_address": "192.168.1.70",
    })

    # Verify map nodes returns GPS location
    resp = client.get("/api/map/nodes")
    assert resp.status_code == 200
    data = resp.json()

    gps_node = next(
        (n for n in data["nodes"] if n["device_id"] == "gps-pipeline-01"),
        None,
    )
    assert gps_node is not None
    assert gps_node["location"] is not None
    assert abs(gps_node["location"]["lat"] - 40.7128) < 0.001
    assert abs(gps_node["location"]["lng"] - (-74.0060)) < 0.001
    assert gps_node["board"] == "touch-lcd-35bc"


# ---------------------------------------------------------------------------
# 5. Full device lifecycle
# ---------------------------------------------------------------------------


def test_full_device_lifecycle(client, store):
    """Register -> heartbeat -> diagnostics -> health report -> verify
    health classification updates through the full pipeline."""

    device_id = "lifecycle-node-01"

    # Step 1: Register via heartbeat
    hb1 = client.post(f"/api/devices/{device_id}/status", json={
        "firmware_version": "2.0.0",
        "board": "touch-lcd-35bc",
        "ip_address": "192.168.1.80",
        "mac": "20:6E:F1:9A:12:02",
        "uptime_s": 100,
        "free_heap": 250000,
        "wifi_rssi": -35,
        "capabilities": ["display", "camera", "imu", "audio"],
    })
    assert hb1.status_code == 200
    assert hb1.json()["status"] == "ok"

    # Verify device was auto-registered
    dev_resp = client.get(f"/api/devices/{device_id}")
    assert dev_resp.status_code == 200
    assert dev_resp.json()["board"] == "touch-lcd-35bc"
    assert dev_resp.json()["version"] == "2.0.0"

    # Step 2: Submit healthy diagnostics
    diag1 = client.post(f"/api/devices/{device_id}/diag", json={
        "health": {
            "free_heap": 250000,
            "min_heap": 220000,
            "free_psram": 6000000,
            "largest_block": 200000,
            "wifi_rssi": -35,
            "wifi_connected": True,
            "i2c_errors": 0,
            "uptime_s": 100,
            "reboot_count": 0,
        },
        "anomalies": [],
        "board": "touch-lcd-35bc",
        "version": "2.0.0",
    })
    assert diag1.status_code == 200

    # Step 3: Verify healthy classification
    report1 = client.get("/api/fleet/health-report")
    assert report1.status_code == 200
    data1 = report1.json()
    assert data1["total_nodes"] == 1
    assert data1["healthy_nodes"] == 1
    assert data1["critical_nodes"] == 0
    node1 = data1["nodes"][0]
    assert node1["status"] == "healthy"
    assert node1["device_id"] == device_id

    # Step 4: Device degrades — submit critical diagnostics
    # Simulate memory leak and high reboot count
    diag2 = client.post(f"/api/devices/{device_id}/diag", json={
        "health": {
            "free_heap": 15000,
            "min_heap": 8000,
            "free_psram": 1000000,
            "largest_block": 10000,
            "wifi_rssi": -80,
            "wifi_connected": True,
            "i2c_errors": 25,
            "uptime_s": 300,
            "reboot_count": 5,
        },
        "anomalies": [
            {
                "subsystem": "memory",
                "description": "Heap critically low, possible leak",
                "severity": 0.9,
            },
            {
                "subsystem": "i2c",
                "description": "Excessive I2C bus errors",
                "severity": 0.7,
            },
        ],
        "board": "touch-lcd-35bc",
        "version": "2.0.0",
    })
    assert diag2.status_code == 200
    assert diag2.json()["anomalies_recorded"] == 2

    # Step 5: Verify classification updated to critical
    report2 = client.get("/api/fleet/health-report")
    assert report2.status_code == 200
    data2 = report2.json()
    assert data2["total_nodes"] == 1
    assert data2["critical_nodes"] == 1
    assert data2["healthy_nodes"] == 0
    node2 = data2["nodes"][0]
    assert node2["status"] == "critical"
    assert node2["device_id"] == device_id

    # Verify the health score reflects degradation
    assert data2["health_score"] == 0.0

    # Validate the full pipeline data can round-trip through tritium-lib models
    snapshot = HealthSnapshot(
        timestamp="2026-03-08T12:00:00Z",
        node_id=device_id,
        free_heap=node2["free_heap"],
        min_free_heap=node2["min_free_heap"],
        free_psram=0,
        largest_free_block=0,
        wifi_rssi=node2["wifi_rssi"],
        wifi_connected=node2["wifi_connected"],
        i2c_errors=node2["i2c_errors"],
        uptime_s=node2["uptime_s"],
        reboot_count=node2["reboot_count"],
    )
    assert snapshot.free_heap == 15000
    assert snapshot.reboot_count == 5
    assert snapshot.i2c_errors == 25

    fleet_node = FleetNode(
        device_id=device_id,
        free_heap=snapshot.free_heap,
        wifi_rssi=snapshot.wifi_rssi or 0,
        status=NodeStatus.ONLINE,
    )
    assert fleet_node.device_id == device_id
    assert fleet_node.status == NodeStatus.ONLINE
