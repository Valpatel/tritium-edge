# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for the fleet OTA management endpoints."""


def test_fleet_ota_targets_empty(client):
    """OTA targets returns empty when no devices exist."""
    r = client.get("/api/fleet-ota/targets")
    assert r.status_code == 200
    data = r.json()
    assert data["total"] == 0


def test_fleet_ota_targets_with_device(client, sample_device):
    """OTA targets lists devices and groups by board."""
    r = client.get("/api/fleet-ota/targets")
    data = r.json()
    assert data["total"] == 1
    assert "touch-lcd-35bc" in data["boards"]
    assert len(data["by_board"]["touch-lcd-35bc"]) == 1


def test_fleet_ota_targets_filter_board(client, sample_device):
    """Board filter narrows targets."""
    r = client.get("/api/fleet-ota/targets?board=touch-lcd-35bc")
    assert r.json()["total"] == 1

    r = client.get("/api/fleet-ota/targets?board=nonexistent")
    assert r.json()["total"] == 0


def test_fleet_ota_push_by_device_ids(client, sample_device, sample_firmware):
    """Push OTA to specific device IDs."""
    r = client.post("/api/fleet-ota/push", json={
        "firmware_id": "fw-test0001",
        "device_ids": ["test-node-001"],
    })
    assert r.status_code == 200
    data = r.json()
    assert data["scheduled"] == 1
    assert data["firmware_version"] == "2.0.0"
    assert "rollout_id" in data


def test_fleet_ota_push_by_board(client, sample_device, sample_firmware):
    """Push OTA filtered by board type."""
    r = client.post("/api/fleet-ota/push", json={
        "firmware_id": "fw-test0001",
        "board_filter": "touch-lcd-35bc",
    })
    assert r.status_code == 200
    assert r.json()["scheduled"] == 1


def test_fleet_ota_push_no_firmware(client, sample_device):
    """Push OTA with nonexistent firmware returns 404."""
    r = client.post("/api/fleet-ota/push", json={
        "firmware_id": "fw-nonexistent",
        "device_ids": ["test-node-001"],
    })
    assert r.status_code == 404


def test_fleet_ota_push_no_targets(client, sample_firmware):
    """Push OTA with no matching targets returns 400."""
    r = client.post("/api/fleet-ota/push", json={
        "firmware_id": "fw-test0001",
        "device_ids": ["nonexistent"],
    })
    assert r.status_code == 400


def test_fleet_ota_status(client, sample_device, sample_firmware):
    """OTA status reflects pending deployments."""
    # Push first
    client.post("/api/fleet-ota/push", json={
        "firmware_id": "fw-test0001",
        "device_ids": ["test-node-001"],
    })

    r = client.get("/api/fleet-ota/status")
    assert r.status_code == 200
    data = r.json()
    assert data["pending_count"] == 1
    assert data["pending"][0]["device_id"] == "test-node-001"
    assert data["pending"][0]["target_version"] == "2.0.0"


def test_fleet_ota_cancel(client, sample_device, sample_firmware):
    """Cancel all clears pending OTA."""
    client.post("/api/fleet-ota/push", json={
        "firmware_id": "fw-test0001",
        "device_ids": ["test-node-001"],
    })

    r = client.post("/api/fleet-ota/cancel")
    assert r.status_code == 200
    assert r.json()["cancelled"] == 1

    # Verify cleared
    r = client.get("/api/fleet-ota/status")
    assert r.json()["pending_count"] == 0
