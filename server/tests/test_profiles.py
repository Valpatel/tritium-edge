# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for the device profile endpoints."""


def test_list_profiles_empty(client):
    """Profiles list returns empty when none exist."""
    r = client.get("/api/profiles")
    assert r.status_code == 200
    assert r.json() == []


def test_create_profile(client):
    """Create a new profile."""
    r = client.post("/api/profiles", json={
        "name": "Sensor Default",
        "description": "Default config for sensor nodes",
        "config": {"scan_interval": 30, "brightness": 50},
    })
    assert r.status_code == 200
    data = r.json()
    assert data["name"] == "Sensor Default"
    assert data["config"]["scan_interval"] == 30
    assert data["id"].startswith("prof-")


def test_create_profile_no_name(client):
    """Create profile without name returns 400."""
    r = client.post("/api/profiles", json={"config": {}})
    assert r.status_code == 400


def test_get_profile(client, store):
    """Get a profile by ID."""
    profile = {
        "id": "prof-test0001",
        "name": "Test Profile",
        "description": "test",
        "config": {"key": "value"},
        "created_at": "2026-01-01T00:00:00+00:00",
        "updated_at": "2026-01-01T00:00:00+00:00",
    }
    store.save_profile(profile)

    r = client.get("/api/profiles/prof-test0001")
    assert r.status_code == 200
    assert r.json()["name"] == "Test Profile"


def test_get_profile_not_found(client):
    """Get nonexistent profile returns 404."""
    r = client.get("/api/profiles/prof-nonexistent")
    assert r.status_code == 404


def test_update_profile(client, store):
    """Update an existing profile."""
    profile = {
        "id": "prof-upd001",
        "name": "Original",
        "description": "",
        "config": {"a": 1},
        "created_at": "2026-01-01T00:00:00+00:00",
        "updated_at": "2026-01-01T00:00:00+00:00",
    }
    store.save_profile(profile)

    r = client.put("/api/profiles/prof-upd001", json={
        "name": "Updated",
        "config": {"a": 2, "b": 3},
    })
    assert r.status_code == 200
    data = r.json()
    assert data["name"] == "Updated"
    assert data["config"]["b"] == 3


def test_update_profile_not_found(client):
    """Update nonexistent profile returns 404."""
    r = client.put("/api/profiles/prof-nonexistent", json={"name": "X"})
    assert r.status_code == 404


def test_delete_profile(client, store):
    """Delete a profile removes it."""
    profile = {
        "id": "prof-del001",
        "name": "To Delete",
        "description": "",
        "config": {},
        "created_at": "2026-01-01T00:00:00+00:00",
        "updated_at": "2026-01-01T00:00:00+00:00",
    }
    store.save_profile(profile)

    r = client.delete("/api/profiles/prof-del001")
    assert r.status_code == 200
    assert r.json()["status"] == "deleted"

    r = client.get("/api/profiles/prof-del001")
    assert r.status_code == 404


def test_apply_profile(client, store, sample_device):
    """Apply a profile pushes config to assigned devices."""
    profile = {
        "id": "prof-apply001",
        "name": "Apply Test",
        "description": "",
        "config": {"brightness": 100, "scan_interval": 10},
        "created_at": "2026-01-01T00:00:00+00:00",
        "updated_at": "2026-01-01T00:00:00+00:00",
    }
    store.save_profile(profile)

    # Assign profile to device
    device = store.get_device("test-node-001")
    device["profile_id"] = "prof-apply001"
    store.save_device(device)

    r = client.post("/api/profiles/prof-apply001/apply")
    assert r.status_code == 200
    data = r.json()
    assert data["status"] == "applied"
    assert data["devices_updated"] == 1

    # Verify config was pushed
    device = store.get_device("test-node-001")
    assert device["desired_config"]["brightness"] == 100


def test_apply_profile_not_found(client):
    """Apply nonexistent profile returns 404."""
    r = client.post("/api/profiles/prof-nonexistent/apply")
    assert r.status_code == 404


def test_apply_profile_no_devices(client, store):
    """Apply profile with no assigned devices updates zero."""
    profile = {
        "id": "prof-empty001",
        "name": "Empty",
        "description": "",
        "config": {"key": "val"},
        "created_at": "2026-01-01T00:00:00+00:00",
        "updated_at": "2026-01-01T00:00:00+00:00",
    }
    store.save_profile(profile)

    r = client.post("/api/profiles/prof-empty001/apply")
    assert r.status_code == 200
    assert r.json()["devices_updated"] == 0
