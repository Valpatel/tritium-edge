# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for the device management endpoints."""


def test_list_devices_empty(client):
    """List devices returns empty list when no devices exist."""
    r = client.get("/api/devices")
    assert r.status_code == 200
    assert r.json() == []


def test_list_devices_with_device(client, sample_device):
    """List devices returns registered devices."""
    r = client.get("/api/devices")
    assert r.status_code == 200
    data = r.json()
    assert len(data) == 1
    assert data[0]["device_id"] == "test-node-001"


def test_get_device(client, sample_device):
    """Get a single device by ID."""
    r = client.get("/api/devices/test-node-001")
    assert r.status_code == 200
    data = r.json()
    assert data["device_id"] == "test-node-001"
    assert data["board"] == "touch-lcd-35bc"


def test_get_device_not_found(client):
    """Get nonexistent device returns 404."""
    r = client.get("/api/devices/nonexistent")
    assert r.status_code == 404


def test_update_device(client, sample_device):
    """Patch device updates allowed fields."""
    r = client.patch("/api/devices/test-node-001", json={
        "device_name": "Updated Sensor",
        "tags": ["production"],
        "notes": "moved to room 2",
    })
    assert r.status_code == 200
    data = r.json()
    assert data["device_name"] == "Updated Sensor"
    assert data["tags"] == ["production"]
    assert data["notes"] == "moved to room 2"


def test_update_device_not_found(client):
    """Patch nonexistent device returns 404."""
    r = client.patch("/api/devices/nonexistent", json={"device_name": "X"})
    assert r.status_code == 404


def test_delete_device(client, sample_device):
    """Delete a device removes it."""
    r = client.delete("/api/devices/test-node-001")
    assert r.status_code == 200
    assert r.json()["status"] == "deleted"

    r = client.get("/api/devices/test-node-001")
    assert r.status_code == 404


def test_reboot_device(client, sample_device):
    """Reboot sets pending_command on device."""
    r = client.post("/api/devices/test-node-001/reboot")
    assert r.status_code == 200
    assert r.json()["status"] == "reboot_scheduled"

    r = client.get("/api/devices/test-node-001")
    assert r.json()["pending_command"]["cmd"] == "reboot"


def test_reboot_device_not_found(client):
    """Reboot nonexistent device returns 404."""
    r = client.post("/api/devices/nonexistent/reboot")
    assert r.status_code == 404


def test_send_command(client, sample_device):
    """Send a command to a device."""
    r = client.post("/api/devices/test-node-001/command", json={
        "type": "gpio_set",
        "payload": {"pin": 5, "value": 1},
        "ttl_s": 120,
    })
    assert r.status_code == 200
    data = r.json()
    assert data["type"] == "gpio_set"
    assert data["status"] == "pending"
    assert data["payload"]["pin"] == 5


def test_send_command_not_found(client):
    """Command to nonexistent device returns 404."""
    r = client.post("/api/devices/nonexistent/command", json={
        "type": "reboot",
    })
    assert r.status_code == 404


def test_get_telemetry_empty(client, sample_device):
    """Telemetry returns empty list for device with no data."""
    r = client.get("/api/devices/test-node-001/telemetry")
    assert r.status_code == 200
    assert r.json() == []


def test_get_sensors(client, sample_device):
    """Sensors endpoint returns sensor data from device."""
    r = client.get("/api/devices/test-node-001/sensors")
    assert r.status_code == 200
    data = r.json()
    assert data["device_id"] == "test-node-001"
    assert data["sensors"] == {}


def test_get_sensors_not_found(client):
    """Sensors for nonexistent device returns 404."""
    r = client.get("/api/devices/nonexistent/sensors")
    assert r.status_code == 404


def test_get_config(client, sample_device):
    """Config endpoint returns desired and reported config."""
    r = client.get("/api/devices/test-node-001/config")
    assert r.status_code == 200
    data = r.json()
    assert data["device_id"] == "test-node-001"
    assert data["desired_config"] == {}
    assert data["reported_config"] == {}


def test_get_config_not_found(client):
    """Config for nonexistent device returns 404."""
    r = client.get("/api/devices/nonexistent/config")
    assert r.status_code == 404


def test_set_config(client, sample_device):
    """Set desired config merges with existing."""
    r = client.put("/api/devices/test-node-001/config", json={
        "scan_interval": 30,
        "brightness": 80,
    })
    assert r.status_code == 200
    data = r.json()
    assert data["status"] == "ok"
    assert data["desired_config"]["scan_interval"] == 30
    assert data["desired_config"]["brightness"] == 80

    # Merge additional keys
    r = client.put("/api/devices/test-node-001/config", json={
        "volume": 50,
    })
    assert r.status_code == 200
    desired = r.json()["desired_config"]
    assert desired["scan_interval"] == 30
    assert desired["volume"] == 50


def test_set_config_not_found(client):
    """Set config for nonexistent device returns 404."""
    r = client.put("/api/devices/nonexistent/config", json={"key": "val"})
    assert r.status_code == 404


def test_push_config(client, sample_device):
    """POST config stores desired_config and returns updated device."""
    r = client.post("/api/devices/test-node-001/config", json={
        "scan_interval": 30,
        "brightness": 80,
    })
    assert r.status_code == 200
    data = r.json()
    assert data["device_id"] == "test-node-001"
    assert data["desired_config"]["scan_interval"] == 30
    assert data["desired_config"]["brightness"] == 80

    # Verify it persists
    r = client.get("/api/devices/test-node-001/config")
    assert r.status_code == 200
    assert r.json()["desired_config"]["scan_interval"] == 30


def test_push_config_not_found(client):
    """POST config to nonexistent device returns 404."""
    r = client.post("/api/devices/nonexistent/config", json={
        "key": "value",
    })
    assert r.status_code == 404


def test_get_config_no_data(client, sample_device):
    """GET config when none set returns empty configs and synced state."""
    r = client.get("/api/devices/test-node-001/config")
    assert r.status_code == 200
    data = r.json()
    assert data["device_id"] == "test-node-001"
    assert data["desired_config"] == {}
    assert data["reported_config"] == {}
    assert data["is_synced"] is True
    assert data["drift_count"] == 0
    assert data["drifts"] == []


def test_get_config_with_drift(client, sample_device, store):
    """GET config detects drift between desired and reported."""
    # Push desired config
    r = client.post("/api/devices/test-node-001/config", json={
        "scan_interval": 30,
        "brightness": 80,
        "server_url": "https://new.example.com",
    })
    assert r.status_code == 200

    # Simulate device reporting different config via store
    device = store.get_device("test-node-001")
    device["reported_config"] = {
        "scan_interval": 60,
        "brightness": 80,
        "volume": 50,
    }
    store.save_device(device)

    # GET should show drift
    r = client.get("/api/devices/test-node-001/config")
    assert r.status_code == 200
    data = r.json()
    assert data["is_synced"] is False
    assert data["drift_count"] == 3  # scan_interval differs, server_url missing, volume extra

    # Check drift details
    drift_keys = {d["key"] for d in data["drifts"]}
    assert "scan_interval" in drift_keys
    assert "server_url" in drift_keys
    assert "volume" in drift_keys

    # Verify severity classification
    drifts_by_key = {d["key"]: d for d in data["drifts"]}
    assert drifts_by_key["server_url"]["severity"] == "critical"
    assert drifts_by_key["server_url"]["missing"] is True
    assert drifts_by_key["volume"]["extra"] is True


def test_heartbeat_new_device(client):
    """Heartbeat auto-registers a new device."""
    r = client.post("/api/devices/new-node-99/status", json={
        "firmware_version": "1.0.0",
        "board": "touch-lcd-35bc",
        "ip_address": "192.168.1.50",
    })
    assert r.status_code == 200
    data = r.json()
    assert data["status"] == "ok"
    assert "server_time" in data

    # Verify device was created
    r = client.get("/api/devices/new-node-99")
    assert r.status_code == 200
    assert r.json()["board"] == "touch-lcd-35bc"


def test_heartbeat_invalid_id(client):
    """Heartbeat with invalid device_id returns 400."""
    r = client.post("/api/devices/bad..id/status", json={
        "firmware_version": "1.0.0",
    })
    assert r.status_code == 400


def test_device_provision(client):
    """Device self-provision creates a new device."""
    r = client.post("/api/device/provision", json={
        "mac": "AA:BB:CC:DD:EE:FF",
        "board": "touch-lcd-35bc",
        "firmware_version": "2.0.0",
        "capabilities": ["display", "camera"],
    })
    assert r.status_code == 200
    data = r.json()
    assert data["device_id"] == "AA-BB-CC-DD-EE-FF"
    assert data["heartbeat_interval_s"] == 60
