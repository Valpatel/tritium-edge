# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""File-based JSON store for fleet data.

Extracted from fleet_server.py monolith. All filesystem paths are validated
against path traversal. IDs must match [a-zA-Z0-9_-]+.
"""

import json
import re
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional


class FleetStore:
    """JSON file persistence for devices, firmware, events, and commands."""

    def __init__(self, data_dir: str | Path):
        self.data_dir = Path(data_dir)
        self.devices_dir = self.data_dir / "devices"
        self.firmware_dir = self.data_dir / "firmware"
        self.certs_dir = self.data_dir / "certs"
        self.events_dir = self.data_dir / "events"
        self.commands_dir = self.data_dir / "commands"
        self.profiles_dir = self.data_dir / "profiles"
        for d in [self.devices_dir, self.firmware_dir, self.certs_dir,
                  self.events_dir, self.commands_dir, self.profiles_dir]:
            d.mkdir(parents=True, exist_ok=True)

    # --- ID validation ---
    @staticmethod
    def safe_id(id_str: str) -> str:
        """Validate ID contains only safe characters (no path traversal)."""
        if not id_str or not re.match(r'^[a-zA-Z0-9_\-:]+$', id_str):
            raise ValueError(f"Invalid ID: {id_str!r}")
        return id_str

    # --- Devices ---
    def list_devices(self) -> list[dict]:
        devices = []
        for f in sorted(self.devices_dir.glob("*.json")):
            try:
                devices.append(json.loads(f.read_text()))
            except Exception:
                pass
        return devices

    def get_device(self, device_id: str) -> Optional[dict]:
        self.safe_id(device_id)
        p = self.devices_dir / f"{device_id}.json"
        return json.loads(p.read_text()) if p.exists() else None

    def save_device(self, device: dict) -> None:
        self.safe_id(device["device_id"])
        p = self.devices_dir / f"{device['device_id']}.json"
        p.write_text(json.dumps(device, indent=2, default=str) + "\n")

    def delete_device(self, device_id: str) -> None:
        self.safe_id(device_id)
        p = self.devices_dir / f"{device_id}.json"
        if p.exists():
            p.unlink()

    # --- Firmware ---
    def list_firmware(self) -> list[dict]:
        fws = []
        for f in sorted(self.firmware_dir.glob("*.json")):
            try:
                fws.append(json.loads(f.read_text()))
            except Exception:
                pass
        fws.sort(key=lambda x: x.get("uploaded_at", ""), reverse=True)
        return fws

    def get_firmware(self, fw_id: str) -> Optional[dict]:
        self.safe_id(fw_id)
        p = self.firmware_dir / f"{fw_id}.json"
        return json.loads(p.read_text()) if p.exists() else None

    def save_firmware_meta(self, meta: dict) -> None:
        self.safe_id(meta["id"])
        p = self.firmware_dir / f"{meta['id']}.json"
        p.write_text(json.dumps(meta, indent=2, default=str) + "\n")

    def get_firmware_path(self, fw_id: str) -> Optional[Path]:
        self.safe_id(fw_id)
        for ext in [".ota", ".bin"]:
            p = self.firmware_dir / f"{fw_id}{ext}"
            if p.exists():
                return p
        return None

    def get_latest_firmware(self, board: str = None) -> Optional[dict]:
        all_fw = self.list_firmware()
        if not all_fw:
            return None
        if not board:
            return all_fw[0]
        for fw in all_fw:
            fb = (fw.get("board") or "any").lower()
            if fb == "any" or fb in board.lower() or board.lower() in fb:
                return fw
        return all_fw[0]

    def delete_firmware(self, fw_id: str) -> None:
        self.safe_id(fw_id)
        for ext in [".json", ".ota", ".bin"]:
            p = self.firmware_dir / f"{fw_id}{ext}"
            if p.exists():
                p.unlink()

    # --- Commands ---
    def get_pending_commands(self, device_id: str) -> list[dict]:
        self.safe_id(device_id)
        p = self.commands_dir / f"{device_id}.json"
        if not p.exists():
            return []
        try:
            commands = json.loads(p.read_text())
            now = datetime.now(timezone.utc)
            # Filter expired
            active = []
            for cmd in commands:
                expires = cmd.get("expires_at")
                if expires:
                    exp_dt = datetime.fromisoformat(expires)
                    if exp_dt < now:
                        cmd["status"] = "expired"
                        continue
                if cmd.get("status") == "pending":
                    active.append(cmd)
            return active
        except Exception:
            return []

    def save_commands(self, device_id: str, commands: list[dict]) -> None:
        self.safe_id(device_id)
        p = self.commands_dir / f"{device_id}.json"
        p.write_text(json.dumps(commands, indent=2, default=str) + "\n")

    def add_command(self, device_id: str, cmd: dict) -> dict:
        self.safe_id(device_id)
        p = self.commands_dir / f"{device_id}.json"
        commands = []
        if p.exists():
            try:
                commands = json.loads(p.read_text())
            except Exception:
                pass
        commands.append(cmd)
        # Keep last 50 commands
        if len(commands) > 50:
            commands = commands[-50:]
        p.write_text(json.dumps(commands, indent=2, default=str) + "\n")
        return cmd

    # --- Profiles ---
    def list_profiles(self) -> list[dict]:
        profiles = []
        for f in sorted(self.profiles_dir.glob("*.json")):
            try:
                profiles.append(json.loads(f.read_text()))
            except Exception:
                pass
        return profiles

    def get_profile(self, profile_id: str) -> Optional[dict]:
        self.safe_id(profile_id)
        p = self.profiles_dir / f"{profile_id}.json"
        return json.loads(p.read_text()) if p.exists() else None

    def save_profile(self, profile: dict) -> None:
        self.safe_id(profile["id"])
        p = self.profiles_dir / f"{profile['id']}.json"
        p.write_text(json.dumps(profile, indent=2, default=str) + "\n")

    def delete_profile(self, profile_id: str) -> None:
        self.safe_id(profile_id)
        p = self.profiles_dir / f"{profile_id}.json"
        if p.exists():
            p.unlink()

    # --- Events ---
    def add_event(self, event_type: str, device_id: str = None,
                  detail: str = "", extra: dict = None) -> dict:
        evt = {
            "id": uuid.uuid4().hex[:12],
            "ts": datetime.now(timezone.utc).isoformat(),
            "type": event_type,
            "device_id": device_id or "",
            "detail": detail,
        }
        if extra:
            evt.update(extra)
        day = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        p = self.events_dir / f"{day}.jsonl"
        with open(p, "a") as f:
            f.write(json.dumps(evt, default=str) + "\n")
        return evt

    def list_events(self, limit: int = 50, device_id: str = None) -> list[dict]:
        events = []
        files = sorted(self.events_dir.glob("*.jsonl"), reverse=True)
        for fp in files:
            for line in reversed(fp.read_text().splitlines()):
                if not line.strip():
                    continue
                try:
                    evt = json.loads(line)
                    if device_id and evt.get("device_id") != device_id:
                        continue
                    events.append(evt)
                    if len(events) >= limit:
                        return events
                except Exception:
                    pass
        return events
