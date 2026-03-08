# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Shared test fixtures for the Tritium-Edge server."""

import sys
import tempfile
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

# Ensure the server package is importable
sys.path.insert(0, str(Path(__file__).parent.parent))


@pytest.fixture
def tmp_data_dir(tmp_path):
    """Temporary data directory for FleetStore."""
    return tmp_path / "fleet_data"


@pytest.fixture
def store(tmp_data_dir):
    """FleetStore backed by a temp directory."""
    from app.store.fleet_store import FleetStore
    return FleetStore(str(tmp_data_dir))


@pytest.fixture
def app(store):
    """FastAPI app with a temp store, no auth."""
    from app.main import app as _app
    _app.state.store = store
    import time
    _app.state.start_time = time.time()
    return _app


@pytest.fixture
def client(app):
    """TestClient for the FastAPI app."""
    return TestClient(app, raise_server_exceptions=False)


@pytest.fixture
def sample_device(store):
    """Pre-registered sample device."""
    device = {
        "device_id": "test-node-001",
        "device_name": "Lab Sensor",
        "registered_at": "2026-01-01T00:00:00+00:00",
        "board": "touch-lcd-35bc",
        "version": "1.0.0",
        "ip": "192.168.1.100",
        "mac": "20:6E:F1:9A:12:00",
        "provisioned": False,
        "tags": ["auto-registered"],
        "capabilities": ["display", "camera", "imu"],
    }
    store.save_device(device)
    return device


@pytest.fixture
def sample_firmware(store):
    """Pre-uploaded sample firmware metadata + binary."""
    meta = {
        "id": "fw-test0001",
        "filename": "test_firmware.bin",
        "version": "2.0.0",
        "board": "touch-lcd-35bc",
        "size": 1024,
        "total_size": 1024,
        "crc32": "0xDEADBEEF",
        "signed": False,
        "encrypted": False,
        "sha256": "a" * 64,
        "uploaded_at": "2026-01-01T00:00:00+00:00",
        "notes": "test firmware",
        "deploy_count": 0,
    }
    store.save_firmware_meta(meta)
    # Write a dummy binary
    (store.firmware_dir / "fw-test0001.bin").write_bytes(b"\x00" * 1024)
    return meta
