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
