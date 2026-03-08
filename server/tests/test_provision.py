# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for the provisioning workflow endpoints."""


def test_discovered_empty(client):
    """Discovered nodes returns empty when no devices exist."""
    r = client.get("/api/provision/discovered")
    assert r.status_code == 200
    data = r.json()
    assert data["discovered_count"] == 0
    assert data["commissioned_count"] == 0


def test_discovered_shows_pending_node(client, sample_device):
    """Auto-registered node shows as discovered (pending)."""
    r = client.get("/api/provision/discovered")
    data = r.json()
    assert data["discovered_count"] == 1
    assert data["discovered"][0]["device_id"] == "test-node-001"
    assert data["discovered"][0]["provisioned"] is False


def test_commission_node(client, sample_device):
    """Commissioning a node sets name, role, and marks provisioned."""
    r = client.post("/api/provision/commission", json={
        "device_id": "test-node-001",
        "device_name": "Living Room Sensor",
        "location": "Building A, Room 101",
        "role": "sensor",
    })
    assert r.status_code == 200
    data = r.json()
    assert data["status"] == "commissioned"
    assert data["provisioned"] is True

    # Verify it moved from discovered to commissioned
    r = client.get("/api/provision/discovered")
    data = r.json()
    assert data["discovered_count"] == 0
    assert data["commissioned_count"] == 1
    comm = data["commissioned"][0]
    assert comm["device_name"] == "Living Room Sensor"
    assert comm["role"] == "sensor"


def test_commission_not_found(client):
    """Commissioning nonexistent device returns 404."""
    r = client.post("/api/provision/commission", json={
        "device_id": "nonexistent",
        "device_name": "Test",
    })
    assert r.status_code == 404


def test_commission_with_wifi(client, sample_device, store):
    """Commissioning with WiFi credentials saves factory_wifi.json."""
    r = client.post("/api/provision/commission", json={
        "device_id": "test-node-001",
        "wifi_ssid": "TestNet",
        "wifi_pass": "secret123",
    })
    assert r.status_code == 200

    wifi_path = store.certs_dir / "test-node-001" / "factory_wifi.json"
    assert wifi_path.exists()
    import json
    wifi = json.loads(wifi_path.read_text())
    assert wifi["ssid"] == "TestNet"
    assert wifi["password"] == "secret123"


def test_decommission_node(client, sample_device):
    """Decommissioning a node reverts it to unprovisioned."""
    # Commission first
    client.post("/api/provision/commission", json={
        "device_id": "test-node-001",
        "device_name": "Sensor",
    })

    # Decommission
    r = client.post("/api/provision/decommission/test-node-001")
    assert r.status_code == 200
    assert r.json()["status"] == "decommissioned"

    # Verify it's back in discovered
    r = client.get("/api/provision/discovered")
    data = r.json()
    # It has "decommissioned" tag, not "pending" or "auto-registered",
    # and provisioned=False, so it shows as discovered
    assert data["discovered_count"] == 1


def test_decommission_not_found(client):
    """Decommissioning nonexistent device returns 404."""
    r = client.post("/api/provision/decommission/nonexistent")
    assert r.status_code == 404


def test_bulk_commission(client, store):
    """Bulk commissioning processes multiple nodes."""
    # Create two devices
    for i in range(2):
        store.save_device({
            "device_id": f"bulk-node-{i}",
            "board": "test-board",
            "tags": ["auto-registered"],
            "provisioned": False,
        })

    r = client.post("/api/provision/commission/bulk", json={
        "nodes": [
            {"device_id": "bulk-node-0", "device_name": "Node A", "role": "sensor"},
            {"device_id": "bulk-node-1", "device_name": "Node B", "role": "gateway"},
            {"device_id": "nonexistent"},
        ],
    })
    assert r.status_code == 200
    data = r.json()
    assert data["total"] == 3
    statuses = {r["device_id"]: r["status"] for r in data["results"]}
    assert statuses["bulk-node-0"] == "commissioned"
    assert statuses["bulk-node-1"] == "commissioned"
    assert statuses["nonexistent"] == "not_found"
