# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for the fleet map endpoints."""


def test_map_nodes_empty(client):
    """Map nodes returns empty list when no devices exist."""
    r = client.get("/api/map/nodes")
    assert r.status_code == 200
    data = r.json()
    assert data["count"] == 0
    assert data["nodes"] == []


def test_map_nodes_no_location(client, sample_device):
    """Devices without location return location=null."""
    r = client.get("/api/map/nodes")
    assert r.status_code == 200
    data = r.json()
    assert data["count"] == 1
    node = data["nodes"][0]
    assert node["device_id"] == "test-node-001"
    assert node["location"] is None


def test_set_node_location(client, sample_device):
    """Setting a node location persists and is returned in map nodes."""
    r = client.put(
        "/api/map/nodes/test-node-001/location",
        json={"lat": 37.7749, "lng": -122.4194, "label": "San Francisco"},
    )
    assert r.status_code == 200
    assert r.json()["status"] == "ok"

    # Verify it shows up in map nodes
    r = client.get("/api/map/nodes")
    data = r.json()
    node = data["nodes"][0]
    assert node["location"] is not None
    assert abs(node["location"]["lat"] - 37.7749) < 0.001
    assert abs(node["location"]["lng"] - (-122.4194)) < 0.001
    assert node["location"]["label"] == "San Francisco"


def test_set_location_not_found(client):
    """Setting location for nonexistent device returns 404."""
    r = client.put(
        "/api/map/nodes/nonexistent/location",
        json={"lat": 0, "lng": 0},
    )
    assert r.status_code == 404


def test_clear_node_location(client, sample_device):
    """Clearing a node location removes it."""
    # Set first
    client.put(
        "/api/map/nodes/test-node-001/location",
        json={"lat": 37.7749, "lng": -122.4194},
    )
    # Clear
    r = client.delete("/api/map/nodes/test-node-001/location")
    assert r.status_code == 200

    # Verify cleared
    r = client.get("/api/map/nodes")
    node = r.json()["nodes"][0]
    assert node["location"] is None


def test_map_node_includes_device_info(client, sample_device):
    """Map nodes include board, IP, capabilities, status."""
    r = client.get("/api/map/nodes")
    node = r.json()["nodes"][0]
    assert node["board"] == "touch-lcd-35bc"
    assert node["ip"] == "192.168.1.100"
    assert "display" in node["capabilities"]
    assert node["version"] == "1.0.0"


def test_update_existing_location(client, sample_device):
    """PUT on a node that already has a location updates it."""
    # Set initial location
    client.put(
        "/api/map/nodes/test-node-001/location",
        json={"lat": 10.0, "lng": 20.0, "label": "Old"},
    )
    # Update to new location
    r = client.put(
        "/api/map/nodes/test-node-001/location",
        json={"lat": 51.5074, "lng": -0.1278, "label": "London"},
    )
    assert r.status_code == 200
    assert r.json()["location"]["lat"] == 51.5074
    assert r.json()["location"]["label"] == "London"

    # Verify old location is replaced
    r = client.get("/api/map/nodes")
    node = r.json()["nodes"][0]
    assert abs(node["location"]["lat"] - 51.5074) < 0.001
    assert abs(node["location"]["lng"] - (-0.1278)) < 0.001
    assert node["location"]["label"] == "London"


def test_clear_location_not_found(client):
    """Clearing location for nonexistent device returns 404."""
    r = client.delete("/api/map/nodes/nonexistent/location")
    assert r.status_code == 404


def test_gps_heartbeat_overrides_manual_location(client, store):
    """Nodes with GPS data from heartbeat use that over manual location."""
    store.save_device({
        "device_id": "gps-node-001",
        "registered_at": "2026-01-01T00:00:00+00:00",
        "board": "touch-lcd-35bc",
        "gps_lat": 35.6762,
        "gps_lng": 139.6503,
        "location": {"lat": 0.0, "lng": 0.0, "label": "Manual Override"},
    })

    r = client.get("/api/map/nodes")
    assert r.status_code == 200
    node = next(n for n in r.json()["nodes"] if n["device_id"] == "gps-node-001")
    # GPS should take priority over manual location
    assert abs(node["location"]["lat"] - 35.6762) < 0.001
    assert abs(node["location"]["lng"] - 139.6503) < 0.001
