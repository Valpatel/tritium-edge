# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for the fleet statistics endpoints."""


def test_stats_empty(client):
    """Stats returns zeros when no devices exist."""
    r = client.get("/api/stats")
    assert r.status_code == 200
    data = r.json()
    assert data["total_devices"] == 0
    assert data["online_devices"] == 0
    assert data["total_firmware"] == 0


def test_stats_with_device(client, sample_device):
    """Stats reflects registered devices."""
    r = client.get("/api/stats")
    assert r.status_code == 200
    data = r.json()
    assert data["total_devices"] == 1
    assert data["offline_devices"] == 1  # No heartbeat, so offline
    assert "touch-lcd-35bc" in data["boards"]
    assert "1.0.0" in data["versions"]


def test_stats_with_firmware(client, sample_firmware):
    """Stats reflects uploaded firmware."""
    r = client.get("/api/stats")
    assert r.status_code == 200
    data = r.json()
    assert data["total_firmware"] == 1


def test_fleet_status_empty(client):
    """Fleet status returns zeros when no devices exist."""
    r = client.get("/api/fleet/status")
    assert r.status_code == 200
    data = r.json()
    assert data["total_nodes"] == 0
    assert data["online"] == 0
    assert data["offline"] == 0
    assert data["total_ble_devices"] == 0
    assert "server_uptime_s" in data
    assert "timestamp" in data


def test_fleet_status_with_device(client, sample_device):
    """Fleet status counts devices."""
    r = client.get("/api/fleet/status")
    assert r.status_code == 200
    data = r.json()
    assert data["total_nodes"] == 1
    assert data["offline"] == 1


def test_fleet_health_empty(client):
    """Fleet health returns score for empty fleet."""
    r = client.get("/api/fleet/health")
    assert r.status_code == 200
    data = r.json()
    assert "health_score" in data
    assert isinstance(data["health_score"], (int, float))
    assert data["total_nodes"] == 0
    assert "server_uptime_s" in data


def test_fleet_health_with_device(client, sample_device):
    """Fleet health includes device counts."""
    r = client.get("/api/fleet/health")
    assert r.status_code == 200
    data = r.json()
    assert data["total_nodes"] == 1
    assert 0.0 <= data["health_score"] <= 1.0
