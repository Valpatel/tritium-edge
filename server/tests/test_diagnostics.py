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


def test_diagnostics_history(client):
    """GET /api/devices/{id}/diag/history returns persisted reports."""
    # Submit multiple reports
    for i in range(3):
        client.post("/api/devices/hist-node/diag", json={
            "health": {"free_heap": 100000 + i * 10000},
            "anomalies": [],
        })

    resp = client.get("/api/devices/hist-node/diag/history")
    assert resp.status_code == 200
    data = resp.json()
    assert len(data) == 3
    # Chronological order
    assert data[0]["health"]["free_heap"] == 100000
    assert data[2]["health"]["free_heap"] == 120000


def test_diagnostics_history_empty(client):
    """GET history for unknown device returns empty list."""
    resp = client.get("/api/devices/no-such-node/diag/history")
    assert resp.status_code == 200
    assert resp.json() == []


def test_diagnostics_history_with_anomalies(client):
    """History entries include anomaly counts."""
    client.post("/api/devices/anom-hist/diag", json={
        "health": {"free_heap": 50000},
        "anomalies": [
            {"subsystem": "memory", "severity": 0.8},
            {"subsystem": "wifi", "severity": 0.5},
        ],
    })

    resp = client.get("/api/devices/anom-hist/diag/history")
    data = resp.json()
    assert len(data) == 1
    assert data[0]["anomaly_count"] == 2
    assert len(data[0]["anomalies"]) == 2


# --- Fleet health report (tritium-lib integration) ---


def test_fleet_health_report_empty(client):
    """Empty fleet returns perfect score."""
    resp = client.get("/api/fleet/health-report")
    assert resp.status_code == 200
    data = resp.json()
    assert data["health_score"] == 1.0
    assert data["total_nodes"] == 0
    assert data["infrastructure_anomalies"] == []


def test_fleet_health_report_healthy_node(client):
    """Single healthy node classified correctly."""
    client.post("/api/devices/healthy-1/diag", json={
        "health": {
            "free_heap": 120000,
            "min_free_heap": 100000,
            "free_psram": 4000000,
            "largest_block": 80000,
            "wifi_rssi": -45,
            "wifi_connected": True,
            "uptime_s": 3600,
        },
        "anomalies": [],
    })
    resp = client.get("/api/fleet/health-report")
    data = resp.json()
    assert data["total_nodes"] == 1
    assert data["healthy_nodes"] == 1
    assert data["health_score"] == 1.0
    assert data["nodes"][0]["status"] == "healthy"


def test_fleet_health_report_critical_node(client):
    """Node with critical anomaly classified as critical."""
    client.post("/api/devices/crit-1/diag", json={
        "health": {
            "free_heap": 10000,  # Below 20KB critical threshold
            "min_free_heap": 5000,
            "free_psram": 0,
            "largest_block": 5000,
            "wifi_connected": True,
        },
        "anomalies": [
            {"subsystem": "memory", "description": "Heap critical", "severity": 0.9},
        ],
    })
    resp = client.get("/api/fleet/health-report")
    data = resp.json()
    assert data["critical_nodes"] == 1
    assert data["healthy_nodes"] == 0
    assert data["health_score"] == 0.0
    assert data["nodes"][0]["status"] == "critical"


def test_fleet_health_report_infra_anomalies(client):
    """Cross-node anomaly detection flags fleet-wide WiFi issues."""
    # Submit 3 nodes with bad WiFi
    for i in range(3):
        client.post(f"/api/devices/wifi-bad-{i}/diag", json={
            "health": {
                "free_heap": 120000,
                "min_free_heap": 100000,
                "free_psram": 4000000,
                "largest_block": 80000,
                "wifi_rssi": -85,
                "wifi_connected": True,
                "uptime_s": 3600,
            },
            "anomalies": [],
        })
    resp = client.get("/api/fleet/health-report")
    data = resp.json()
    assert data["total_nodes"] == 3
    # All 3 have RSSI < -80, which exceeds threshold (>50%)
    assert len(data["infrastructure_anomalies"]) >= 1
    infra_types = [a["type"] for a in data["infrastructure_anomalies"]]
    assert "wifi_degradation" in infra_types


def test_fleet_health_report_i2c_slaves(client):
    """Health report includes per-slave I2C data."""
    client.post("/api/devices/i2c-node/diag", json={
        "health": {
            "free_heap": 120000,
            "min_free_heap": 100000,
            "free_psram": 4000000,
            "largest_block": 80000,
            "wifi_connected": True,
            "i2c_slaves": [
                {"addr": "0x34", "ok": 100, "nack": 2, "timeout": 1, "lat_us": 50},
                {"addr": "0x6B", "ok": 95, "nack": 0, "timeout": 5, "lat_us": 120},
            ],
        },
        "anomalies": [],
    })
    resp = client.get("/api/fleet/health-report")
    data = resp.json()
    assert data["total_nodes"] == 1
    node = data["nodes"][0]
    assert "i2c_slaves" in node
    assert len(node["i2c_slaves"]) == 2
    assert node["i2c_slaves"][0]["addr"] == "0x34"
    assert node["i2c_slaves"][0]["rate"] > 0.9


def test_fleet_health_report_camera_metrics(client):
    """Health report includes camera metrics when available."""
    client.post("/api/devices/cam-node/diag", json={
        "health": {
            "free_heap": 120000,
            "min_free_heap": 100000,
            "free_psram": 4000000,
            "largest_block": 80000,
            "wifi_connected": True,
            "camera": {
                "frames": 1500,
                "fails": 3,
                "last_us": 45000,
                "max_us": 120000,
                "avg_fps": 3.8,
            },
        },
        "anomalies": [],
    })
    resp = client.get("/api/fleet/health-report")
    data = resp.json()
    node = data["nodes"][0]
    assert "camera" in node
    assert node["camera"]["frames"] == 1500
    assert node["camera"]["avg_fps"] == 3.8


def test_fleet_health_report_node_details(client):
    """Health report nodes include extended fields."""
    client.post("/api/devices/detail-node/diag", json={
        "health": {
            "free_heap": 150000,
            "min_free_heap": 120000,
            "free_psram": 4000000,
            "largest_block": 100000,
            "battery_pct": 72.5,
            "wifi_rssi": -55,
            "wifi_connected": True,
            "i2c_errors": 3,
            "uptime_s": 7200,
            "reboot_count": 1,
        },
        "anomalies": [],
        "board": "touch-lcd-35bc",
        "version": "1.2.0",
    })
    resp = client.get("/api/fleet/health-report")
    data = resp.json()
    node = data["nodes"][0]
    assert node["min_free_heap"] == 120000
    assert node["battery_pct"] == 72.5
    assert node["i2c_errors"] == 3
    assert node["reboot_count"] == 1
    assert node["board"] == "touch-lcd-35bc"
    assert node["version"] == "1.2.0"


def test_fleet_heap_trends_empty(client):
    """Empty fleet returns no trends."""
    resp = client.get("/api/fleet/heap-trends")
    assert resp.status_code == 200
    data = resp.json()
    assert data["total_devices"] == 0
    assert data["leak_suspects"] == 0


def test_fleet_heap_trends_with_devices(client):
    """Heap trends computed from diag cache."""
    client.post("/api/devices/t1/diag", json={
        "health": {"free_heap": 100000, "uptime_s": 3600},
        "anomalies": [],
    })
    client.post("/api/devices/t2/diag", json={
        "health": {"free_heap": 50000, "uptime_s": 7200},
        "anomalies": [],
    })
    resp = client.get("/api/fleet/heap-trends")
    assert resp.status_code == 200
    data = resp.json()
    # Single sample per device, so no trends computed
    assert data["total_devices"] == 0


# --- Fleet correlations ---


def test_fleet_correlations_empty(client):
    """Empty fleet returns no correlations."""
    resp = client.get("/api/fleet/correlations")
    assert resp.status_code == 200
    data = resp.json()
    assert data["total_correlations"] == 0
    assert data["correlations"] == []


def test_fleet_correlations_with_events(client):
    """Correlations endpoint processes anomaly events."""
    # Submit nodes with anomalies — even without actual correlations,
    # the endpoint should return successfully
    for i in range(3):
        client.post(f"/api/devices/corr-{i}/diag", json={
            "health": {
                "free_heap": 120000,
                "uptime_s": 3600,
                "reboot_count": 0,
            },
            "anomalies": [
                {
                    "subsystem": "memory",
                    "description": f"Test anomaly {i}",
                    "severity": 0.5,
                    "detected_at": f"2026-03-07T12:0{i}:00Z",
                },
            ],
        })

    resp = client.get("/api/fleet/correlations")
    assert resp.status_code == 200
    data = resp.json()
    assert "total_correlations" in data
    assert "correlations" in data
    assert isinstance(data["correlations"], list)


# --- Fleet topology ---


def test_fleet_topology_empty(client):
    """Empty fleet returns empty topology."""
    resp = client.get("/api/fleet/topology")
    assert resp.status_code == 200
    data = resp.json()
    assert data["total_nodes"] == 0
    assert data["nodes"] == []
    assert data["links"] == []
    assert data["total_links"] == 0


def test_fleet_topology_with_devices(client, sample_device):
    """Topology includes registered devices as node objects with MAC."""
    resp = client.get("/api/fleet/topology")
    assert resp.status_code == 200
    data = resp.json()
    assert data["total_nodes"] >= 1
    node_ids = [n["device_id"] for n in data["nodes"]]
    assert "test-node-001" in node_ids
    # Node should include MAC address
    node = next(n for n in data["nodes"] if n["device_id"] == "test-node-001")
    assert node["mac"] == "20:6E:F1:9A:12:00"


def test_fleet_topology_with_mesh_stats(client, sample_device):
    """Topology nodes include mesh stats from diag cache."""
    client.post("/api/devices/test-node-001/diag", json={
        "health": {
            "free_heap": 120000,
            "mesh": {
                "peers": 2,
                "routes": 3,
                "tx": 150,
                "rx": 140,
                "tx_fail": 5,
                "relayed": 20,
            },
        },
        "anomalies": [],
    })

    resp = client.get("/api/fleet/topology")
    assert resp.status_code == 200
    data = resp.json()
    node = next(n for n in data["nodes"] if n["device_id"] == "test-node-001")
    assert "mesh" in node
    assert node["mesh"]["peers"] == 2
    assert node["mesh"]["tx"] == 150
    assert node["mesh"]["relayed"] == 20


def test_fleet_topology_mesh_peers_adjacency(client, sample_device):
    """Topology builds adjacency links from mesh_peers with RSSI and hops."""
    client.post("/api/devices/test-node-001/diag", json={
        "health": {
            "free_heap": 120000,
            "mesh_peers": [
                {"mac": "AA:BB:CC:DD:EE:01", "rssi": -45, "hops": 0},
                {"mac": "AA:BB:CC:DD:EE:02", "rssi": -68, "hops": 1},
            ],
        },
        "anomalies": [],
    })

    resp = client.get("/api/fleet/topology")
    assert resp.status_code == 200
    data = resp.json()
    assert data["total_links"] == 2
    links = data["links"]
    assert len(links) == 2

    # Verify link details
    link_macs = {l["target_mac"] for l in links}
    assert "AA:BB:CC:DD:EE:01" in link_macs
    assert "AA:BB:CC:DD:EE:02" in link_macs

    link1 = next(l for l in links if l["target_mac"] == "AA:BB:CC:DD:EE:01")
    assert link1["source"] == "test-node-001"
    assert link1["rssi"] == -45
    assert link1["hops"] == 0

    link2 = next(l for l in links if l["target_mac"] == "AA:BB:CC:DD:EE:02")
    assert link2["rssi"] == -68
    assert link2["hops"] == 1


def test_fleet_topology_resolves_peer_device_id(client, store):
    """Links resolve peer MAC to device_id when the peer is a registered device."""
    # Register two devices
    store.save_device({
        "device_id": "node-alpha",
        "mac": "AA:BB:CC:DD:EE:01",
        "registered_at": "2026-01-01T00:00:00+00:00",
    })
    store.save_device({
        "device_id": "node-beta",
        "mac": "AA:BB:CC:DD:EE:02",
        "registered_at": "2026-01-01T00:00:00+00:00",
    })

    # node-alpha sees node-beta as a mesh peer
    client.post("/api/devices/node-alpha/diag", json={
        "health": {
            "free_heap": 120000,
            "mesh_peers": [
                {"mac": "AA:BB:CC:DD:EE:02", "rssi": -50, "hops": 0},
            ],
        },
        "anomalies": [],
    })

    resp = client.get("/api/fleet/topology")
    data = resp.json()
    assert data["total_links"] == 1
    link = data["links"][0]
    assert link["source"] == "node-alpha"
    assert link["target_mac"] == "AA:BB:CC:DD:EE:02"
    assert link["target_device_id"] == "node-beta"
    assert link["rssi"] == -50
    assert link["hops"] == 0


def test_fleet_topology_deduplicates_bidirectional_links(client, store):
    """Bidirectional mesh peer reports are deduplicated into a single link."""
    store.save_device({
        "device_id": "node-a",
        "mac": "11:22:33:44:55:AA",
        "registered_at": "2026-01-01T00:00:00+00:00",
    })
    store.save_device({
        "device_id": "node-b",
        "mac": "11:22:33:44:55:BB",
        "registered_at": "2026-01-01T00:00:00+00:00",
    })

    # Both nodes report each other as mesh peers
    client.post("/api/devices/node-a/diag", json={
        "health": {
            "free_heap": 120000,
            "mesh_peers": [
                {"mac": "11:22:33:44:55:BB", "rssi": -42, "hops": 0},
            ],
        },
        "anomalies": [],
    })
    client.post("/api/devices/node-b/diag", json={
        "health": {
            "free_heap": 120000,
            "mesh_peers": [
                {"mac": "11:22:33:44:55:AA", "rssi": -44, "hops": 0},
            ],
        },
        "anomalies": [],
    })

    resp = client.get("/api/fleet/topology")
    data = resp.json()
    # Should be deduplicated to 1 link, not 2
    assert data["total_links"] == 1
    assert len(data["links"]) == 1
