# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Data models — Pydantic schemas for API request/response validation.

These models are designed for future extraction into tritium-lib as shared
types across tritium-edge and tritium-sc.
"""

from datetime import datetime
from typing import Optional

from pydantic import BaseModel, Field


# ---------------------------------------------------------------------------
# Auth
# ---------------------------------------------------------------------------
class LoginRequest(BaseModel):
    email: str
    password: str


class TokenResponse(BaseModel):
    access_token: str
    refresh_token: str
    token_type: str = "bearer"


# ---------------------------------------------------------------------------
# Devices
# ---------------------------------------------------------------------------
class DeviceHeartbeat(BaseModel):
    """Heartbeat payload from device (v2 protocol)."""
    device_id: str
    device_token: Optional[str] = None
    firmware_version: str = "unknown"
    firmware_hash: Optional[str] = None
    board: str = "unknown"
    family: str = "esp32"
    uptime_s: Optional[int] = None
    free_heap: Optional[int] = None
    wifi_rssi: Optional[int] = None
    ip_address: Optional[str] = None
    boot_count: Optional[int] = None
    reported_config: Optional[dict] = None
    capabilities: list[str] = Field(default_factory=list)
    ota_status: Optional[str] = None
    ota_result: Optional[dict] = None
    command_acks: list[dict] = Field(default_factory=list)
    mesh_peers: Optional[int] = None
    timestamp: Optional[int] = None


class HeartbeatResponse(BaseModel):
    """Server response to device heartbeat."""
    status: str = "ok"
    server_time: int = 0
    heartbeat_interval_s: int = 60
    desired_config: Optional[dict] = None
    commands: list[dict] = Field(default_factory=list)
    ota: Optional[dict] = None
    attestation: str = "unknown"


class DeviceUpdate(BaseModel):
    """Allowed fields for device metadata update."""
    device_name: Optional[str] = None
    tags: Optional[list[str]] = None
    notes: Optional[str] = None
    group: Optional[str] = None
    profile_id: Optional[str] = None


# ---------------------------------------------------------------------------
# Firmware
# ---------------------------------------------------------------------------
class FirmwareMeta(BaseModel):
    id: str
    filename: str
    version: str
    board: str = "any"
    size: int
    total_size: int
    crc32: str
    signed: bool = False
    encrypted: bool = False
    sha256: str
    uploaded_at: str
    notes: str = ""
    deploy_count: int = 0


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
class CommandRequest(BaseModel):
    """Request to send a command to a device."""
    type: str  # reboot, gpio_set, gpio_read, config_update, ota_url, etc.
    payload: dict = Field(default_factory=dict)
    ttl_s: int = 300  # Time-to-live in seconds


# ---------------------------------------------------------------------------
# OTA
# ---------------------------------------------------------------------------
class OTAPushRequest(BaseModel):
    firmware_id: Optional[str] = None
    latest: bool = False


# ---------------------------------------------------------------------------
# Provisioning
# ---------------------------------------------------------------------------
class ProvisionRequest(BaseModel):
    mac: str
    board: str
    firmware_version: str = "unknown"
    firmware_hash: Optional[str] = None
    capabilities: list[str] = Field(default_factory=list)
    family: str = "esp32"
