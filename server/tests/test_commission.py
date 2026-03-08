# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for the device commissioning endpoints."""

import json


def test_flash_missing_params(client):
    """Flash without port or board returns 400."""
    r = client.post("/api/commission/flash", json={"port": "/dev/ttyACM0"})
    assert r.status_code == 400

    r = client.post("/api/commission/flash", json={"board": "touch-lcd-35bc"})
    assert r.status_code == 400


def test_flash_invalid_port(client):
    """Flash with invalid port path returns 400."""
    r = client.post("/api/commission/flash", json={
        "port": "/tmp/evil",
        "board": "touch-lcd-35bc",
    })
    assert r.status_code == 400


def test_flash_invalid_board(client):
    """Flash with invalid board name returns 400."""
    r = client.post("/api/commission/flash", json={
        "port": "/dev/ttyACM0",
        "board": "../../etc/passwd",
    })
    assert r.status_code == 400


def test_provision_missing_port(client):
    """Provision without port returns 400."""
    r = client.post("/api/commission/provision", json={})
    assert r.status_code == 400


def test_provision_invalid_port(client):
    """Provision with invalid port returns 400."""
    r = client.post("/api/commission/provision", json={"port": "/tmp/evil"})
    assert r.status_code == 400


def test_generate_provision(client, store):
    """Generate provisioning identity creates device and writes files."""
    r = client.post("/api/commission/generate", json={
        "device_name": "Test Node",
        "server_url": "https://fleet.local",
        "mqtt_broker": "mqtt.local",
        "wifi_ssid": "TestNet",
        "wifi_pass": "secret123",
    })
    assert r.status_code == 200
    data = r.json()
    device_id = data["device_id"]
    assert device_id.startswith("esp32-")
    assert data["identity"]["device_name"] == "Test Node"
    assert data["identity"]["server_url"] == "https://fleet.local"

    # Verify device was saved
    device = store.get_device(device_id)
    assert device is not None
    assert device["provisioned"] is False  # Pre-registered, not yet provisioned

    # Verify identity file written
    identity_path = store.certs_dir / device_id / "device.json"
    assert identity_path.exists()
    identity = json.loads(identity_path.read_text())
    assert identity["device_id"] == device_id

    # Verify WiFi file written
    wifi_path = store.certs_dir / device_id / "factory_wifi.json"
    assert wifi_path.exists()
    wifi = json.loads(wifi_path.read_text())
    assert wifi["ssid"] == "TestNet"
    assert wifi["password"] == "secret123"


def test_generate_provision_no_wifi(client, store):
    """Generate provisioning without WiFi skips factory_wifi.json."""
    r = client.post("/api/commission/generate", json={
        "device_name": "No WiFi Node",
    })
    assert r.status_code == 200
    device_id = r.json()["device_id"]

    wifi_path = store.certs_dir / device_id / "factory_wifi.json"
    assert not wifi_path.exists()
