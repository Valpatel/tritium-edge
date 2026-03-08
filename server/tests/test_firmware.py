# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for the firmware management endpoints."""

import io


def test_list_firmware_empty(client):
    """Firmware list returns empty when none uploaded."""
    r = client.get("/api/firmware")
    assert r.status_code == 200
    assert r.json() == []


def test_list_firmware_with_entry(client, sample_firmware):
    """Firmware list returns uploaded firmware."""
    r = client.get("/api/firmware")
    assert r.status_code == 200
    data = r.json()
    assert len(data) == 1
    assert data[0]["id"] == "fw-test0001"
    assert data[0]["version"] == "2.0.0"


def test_upload_firmware(client):
    """Upload a firmware binary."""
    # Create a dummy binary (must be >= 256 bytes)
    firmware_data = b"\x00" * 512
    r = client.post(
        "/api/firmware/upload",
        files={"file": ("test_fw.bin", io.BytesIO(firmware_data), "application/octet-stream")},
        data={"version": "3.0.0", "board": "touch-lcd-35bc", "notes": "test upload"},
    )
    assert r.status_code == 200
    data = r.json()
    assert data["version"] == "3.0.0"
    assert data["board"] == "touch-lcd-35bc"
    assert data["notes"] == "test upload"
    assert data["total_size"] == 512
    assert data["id"].startswith("fw-")


def test_upload_firmware_too_small(client):
    """Upload rejects files smaller than 256 bytes."""
    firmware_data = b"\x00" * 100
    r = client.post(
        "/api/firmware/upload",
        files={"file": ("tiny.bin", io.BytesIO(firmware_data), "application/octet-stream")},
    )
    assert r.status_code == 400


def test_download_firmware(client, sample_firmware):
    """Download a firmware binary."""
    r = client.get("/api/firmware/fw-test0001/download")
    assert r.status_code == 200
    assert len(r.content) == 1024


def test_download_firmware_not_found(client):
    """Download nonexistent firmware returns 404."""
    r = client.get("/api/firmware/fw-nonexistent/download")
    assert r.status_code == 404


def test_delete_firmware(client, sample_firmware):
    """Delete firmware removes it."""
    r = client.delete("/api/firmware/fw-test0001")
    assert r.status_code == 200
    assert r.json()["status"] == "deleted"

    r = client.get("/api/firmware")
    assert r.json() == []
