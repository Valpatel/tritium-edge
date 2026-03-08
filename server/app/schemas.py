# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Pydantic request/response schemas wrapping tritium-lib shared models.

These schemas extend the canonical tritium-lib models with server-specific
fields (computed enrichment, response metadata, etc.).
"""

from datetime import datetime, timezone
from typing import Optional

from pydantic import BaseModel, Field

from tritium_lib.models import DeviceHeartbeat, DeviceCapabilities
from tritium_lib.models.fleet import FleetNode, FleetStatus, NodeStatus
from tritium_lib.models.ble import BlePresence, BlePresenceMap


# ---------------------------------------------------------------------------
# Heartbeat
# ---------------------------------------------------------------------------
class HeartbeatPayload(DeviceHeartbeat):
    """Incoming heartbeat from ESP32 — extends tritium-lib DeviceHeartbeat.

    Adds optional fields that the fleet server accepts but aren't part
    of the core protocol (e.g., mac, sensors, ble_devices).
    """
    mac: Optional[str] = None
    sensors: Optional[dict] = None
    ble_devices: Optional[list[dict]] = None


# ---------------------------------------------------------------------------
# Device response
# ---------------------------------------------------------------------------
class DeviceResponse(BaseModel):
    """API response with enriched device data."""
    device_id: str
    device_name: str = ""
    mac: str = ""
    board: str = "unknown"
    family: str = "esp32"
    firmware_version: str = "unknown"
    ip: Optional[str] = None
    capabilities: DeviceCapabilities = Field(default_factory=DeviceCapabilities)
    status: NodeStatus = NodeStatus.OFFLINE
    last_seen: Optional[str] = None
    age: str = "never"
    uptime_s: Optional[int] = None
    free_heap: Optional[int] = None
    rssi: Optional[int] = None
    tags: list[str] = Field(default_factory=list)
    notes: str = ""
    fw_attested: Optional[bool] = None

    @classmethod
    def from_device_dict(cls, d: dict) -> "DeviceResponse":
        """Build from a raw device dict (as stored by FleetStore)."""
        caps_list = d.get("capabilities", [])
        caps = (
            DeviceCapabilities.from_list(caps_list)
            if isinstance(caps_list, list)
            else DeviceCapabilities()
        )
        online = d.get("_online", False)
        return cls(
            device_id=d.get("device_id", ""),
            device_name=d.get("device_name", ""),
            mac=d.get("mac", ""),
            board=d.get("board", "unknown"),
            family=d.get("family", "esp32"),
            firmware_version=d.get("version", d.get("firmware_version", "unknown")),
            ip=d.get("ip"),
            capabilities=caps,
            status=NodeStatus.ONLINE if online else NodeStatus.OFFLINE,
            last_seen=d.get("last_seen"),
            age=d.get("_age", "never"),
            uptime_s=d.get("uptime_s"),
            free_heap=d.get("free_heap"),
            rssi=d.get("rssi"),
            tags=d.get("tags", []),
            notes=d.get("notes", ""),
            fw_attested=d.get("fw_attested"),
        )


# ---------------------------------------------------------------------------
# Fleet status response
# ---------------------------------------------------------------------------
class FleetStatusResponse(BaseModel):
    """Fleet health overview — wraps tritium-lib FleetStatus."""
    fleet: FleetStatus
    health_score: float = 0.0
    server_uptime_s: int = 0
    timestamp: str = Field(
        default_factory=lambda: datetime.now(timezone.utc).isoformat()
    )


# ---------------------------------------------------------------------------
# BLE presence response
# ---------------------------------------------------------------------------
class BlePresenceResponse(BaseModel):
    """BLE aggregation response — wraps tritium-lib BlePresenceMap."""
    presence_map: BlePresenceMap
    reporting_nodes: int = 0
    timestamp: str = Field(
        default_factory=lambda: datetime.now(timezone.utc).isoformat()
    )
