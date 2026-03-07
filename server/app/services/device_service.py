# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Device management business logic."""

from datetime import datetime, timezone

from ..store.fleet_store import FleetStore


def enrich_devices(devices: list[dict]) -> list[dict]:
    """Add computed fields (_online, _age) to device list."""
    now = datetime.now(timezone.utc)
    for d in devices:
        ls = d.get("last_seen")
        if ls:
            try:
                age = (now - datetime.fromisoformat(ls)).total_seconds()
                d["_online"] = age < 120
                if age < 60:
                    d["_age"] = f"{int(age)}s ago"
                elif age < 3600:
                    d["_age"] = f"{int(age / 60)}m ago"
                elif age < 86400:
                    d["_age"] = f"{int(age / 3600)}h ago"
                else:
                    d["_age"] = f"{int(age / 86400)}d ago"
            except Exception:
                d["_online"] = False
                d["_age"] = "unknown"
        else:
            d["_online"] = False
            d["_age"] = "never"
    return devices


def process_heartbeat(store: FleetStore, device_id: str, body: dict) -> dict:
    """Process device heartbeat, return response dict.

    Handles: device registration, firmware attestation, OTA result tracking,
    pending OTA directives, and pending commands.
    """
    device = store.get_device(device_id)
    if not device:
        existing = store.list_devices()
        if len(existing) >= 100:
            return {"error": "device_limit_reached"}
        device = {
            "device_id": device_id,
            "registered_at": datetime.now(timezone.utc).isoformat(),
            "tags": ["auto-registered"],
            "notes": "",
        }
        store.add_event("device_registered", device_id, "Auto-registered via heartbeat")

    # Update device fields from heartbeat
    device.update({
        "last_seen": datetime.now(timezone.utc).isoformat(),
        "version": body.get("firmware_version", body.get("version", device.get("version", "unknown"))),
        "board": body.get("board", device.get("board", "unknown")),
        "family": body.get("family", device.get("family", "esp32")),
        "ip": body.get("ip_address", body.get("ip", device.get("ip"))),
        "mac": body.get("mac", device.get("mac")),
        "uptime_s": body.get("uptime_s"),
        "free_heap": body.get("free_heap"),
        "rssi": body.get("wifi_rssi", body.get("rssi")),
        "capabilities": body.get("capabilities", device.get("capabilities", [])),
    })

    # Firmware attestation
    fw_hash = body.get("firmware_hash", body.get("fw_hash"))
    attestation = "unknown"
    if fw_hash and len(fw_hash) == 64:
        device["fw_hash"] = fw_hash
        known_fw = store.list_firmware()
        matched = False
        for fw in known_fw:
            if fw.get("sha256") == fw_hash:
                device["fw_attested"] = True
                device["fw_attested_id"] = fw["id"]
                matched = True
                attestation = "trusted"
                break
        if not matched:
            device["fw_attested"] = False
            device["fw_attested_id"] = None
            attestation = "unknown"
            if device.get("_prev_fw_hash") != fw_hash:
                store.add_event("attestation_unknown", device_id,
                                f"Unknown firmware hash: {fw_hash[:16]}...")
            device["_prev_fw_hash"] = fw_hash

    # OTA result reporting
    ota_result = body.get("ota_result")
    if ota_result:
        status = ota_result.get("status", "unknown")
        version = ota_result.get("version", "unknown")
        error = ota_result.get("error", "")
        detail = f"version={version}"
        if error:
            detail += f", error={error}"
        store.add_event(f"ota_{status}", device_id, detail)
        if status == "success" and "pending_ota" in device:
            del device["pending_ota"]

    # Command acknowledgments
    command_acks = body.get("command_acks", [])
    if command_acks:
        commands = store.get_pending_commands(device_id)
        acked_ids = {ack["id"] for ack in command_acks if "id" in ack}
        for cmd in commands:
            if cmd.get("id") in acked_ids:
                ack = next(a for a in command_acks if a.get("id") == cmd.get("id"))
                cmd["status"] = "acked" if ack.get("status") == "ok" else "failed"
                cmd["acked_at"] = datetime.now(timezone.utc).isoformat()
        store.save_commands(device_id, commands)

    # Reported config (v2 heartbeat)
    reported_config = body.get("reported_config")
    if reported_config:
        device["reported_config"] = reported_config

    store.save_device(device)

    # Build response
    response = {
        "status": "ok",
        "server_time": int(datetime.now(timezone.utc).timestamp()),
        "heartbeat_interval_s": 60,
        "attestation": attestation,
    }

    # Pending OTA
    pending = device.get("pending_ota")
    if pending:
        response["ota"] = pending

    # Pending commands
    pending_cmds = store.get_pending_commands(device_id)
    if pending_cmds:
        response["commands"] = pending_cmds
        # Mark as delivered
        for cmd in pending_cmds:
            cmd["status"] = "delivered"
            cmd["delivered_at"] = datetime.now(timezone.utc).isoformat()
        store.save_commands(device_id, pending_cmds)

    # Config drift detection — compare reported_config vs desired_config
    desired = device.get("desired_config")
    if desired:
        reported = device.get("reported_config", {})
        drift = {}
        for key, val in desired.items():
            if reported.get(key) != val:
                drift[key] = val
        if drift:
            response["desired_config"] = drift
            if not device.get("_config_drift_logged"):
                store.add_event("config_drift", device_id,
                                f"Drift in: {', '.join(drift.keys())}")
                device["_config_drift_logged"] = True
                store.save_device(device)
        else:
            if device.get("_config_drift_logged"):
                device["_config_drift_logged"] = False
                store.save_device(device)

    return response
