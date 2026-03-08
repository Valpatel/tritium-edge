# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for the BLE presence aggregation endpoints."""


def test_ble_presence_empty(client):
    """BLE presence returns empty when no devices exist."""
    r = client.get("/api/presence/ble")
    assert r.status_code == 200
    data = r.json()
    assert data["total_ble_macs"] == 0
    assert data["reporting_nodes"] == 0
    assert data["devices"] == []


def test_ble_presence_no_ble_data(client, sample_device):
    """Devices without BLE data report zero."""
    r = client.get("/api/presence/ble")
    assert r.status_code == 200
    data = r.json()
    assert data["total_ble_macs"] == 0
    assert data["reporting_nodes"] == 0


def test_ble_presence_with_sightings(client, store):
    """BLE presence aggregates sightings across nodes."""
    store.save_device({
        "device_id": "node-a",
        "board": "test",
        "ip": "192.168.1.10",
        "rssi": -45,
        "ble_devices": [
            {"mac": "AA:BB:CC:DD:EE:01", "rssi": -60, "name": "Phone", "seen": 5, "known": True},
            {"mac": "AA:BB:CC:DD:EE:02", "rssi": -80, "name": "", "seen": 1},
        ],
    })
    store.save_device({
        "device_id": "node-b",
        "board": "test",
        "ip": "192.168.1.11",
        "rssi": -50,
        "ble_devices": [
            {"mac": "AA:BB:CC:DD:EE:01", "rssi": -55, "name": "Phone", "seen": 3},
        ],
    })

    r = client.get("/api/presence/ble")
    assert r.status_code == 200
    data = r.json()
    assert data["total_ble_macs"] == 2
    assert data["reporting_nodes"] == 2

    # Device seen by 2 nodes should be first (sorted by -node_count)
    first = data["devices"][0]
    assert first["mac"] == "AA:BB:CC:DD:EE:01"
    assert len(first["sightings"]) == 2


def test_ble_device_detail(client, store):
    """BLE device detail returns all sightings for a specific MAC."""
    store.save_device({
        "device_id": "node-a",
        "board": "test",
        "ip": "192.168.1.10",
        "ble_devices": [
            {"mac": "AA:BB:CC:DD:EE:01", "rssi": -60, "name": "Phone", "seen": 5},
        ],
    })

    r = client.get("/api/presence/ble/AA:BB:CC:DD:EE:01")
    assert r.status_code == 200
    data = r.json()
    assert data["mac"] == "AA:BB:CC:DD:EE:01"
    assert data["name"] == "Phone"
    assert data["sighting_count"] == 1
    assert data["sightings"][0]["node_id"] == "node-a"


def test_ble_device_detail_no_sightings(client, sample_device):
    """BLE device detail with no matches returns empty sightings."""
    r = client.get("/api/presence/ble/FF:FF:FF:FF:FF:FF")
    assert r.status_code == 200
    data = r.json()
    assert data["sighting_count"] == 0
    assert data["sightings"] == []
