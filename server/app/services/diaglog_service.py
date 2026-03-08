# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Diagnostic event log store — persistent ring-buffer event collection from devices.

Stores events as JSON files per device per day under {data_dir}/diag/{device_id}/.
Supports querying by timestamp, severity, and device with fleet-wide aggregation.
"""

import json
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional


# Severity levels ordered from least to most severe
SEVERITY_LEVELS = ["DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"]
SEVERITY_RANK = {s: i for i, s in enumerate(SEVERITY_LEVELS)}


class DiagLogStore:
    """File-based storage for device diagnostic event logs."""

    def __init__(self, data_dir: str | Path):
        self.data_dir = Path(data_dir)
        self.diag_dir = self.data_dir / "diag"
        self.diag_dir.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def _safe_id(id_str: str) -> str:
        """Validate ID against path traversal."""
        import re
        if not id_str or not re.match(r'^[a-zA-Z0-9_\-:]+$', id_str):
            raise ValueError(f"Invalid ID: {id_str!r}")
        return id_str

    def _device_dir(self, device_id: str) -> Path:
        self._safe_id(device_id)
        d = self.diag_dir / device_id
        d.mkdir(parents=True, exist_ok=True)
        return d

    def _day_file(self, device_id: str, day: str) -> Path:
        return self._device_dir(device_id) / f"{day}.json"

    def _read_day_file(self, path: Path) -> list[dict]:
        if not path.exists():
            return []
        try:
            data = json.loads(path.read_text())
            return data if isinstance(data, list) else []
        except Exception:
            return []

    def _write_day_file(self, path: Path, events: list[dict]) -> None:
        path.write_text(json.dumps(events, indent=2, default=str) + "\n")

    def append_events(
        self, device_id: str, events: list[dict], boot_count: int = 0
    ) -> int:
        """Append diagnostic events for a device. Returns number stored."""
        self._safe_id(device_id)
        now = datetime.now(timezone.utc)
        day = now.strftime("%Y-%m-%d")
        path = self._day_file(device_id, day)

        existing = self._read_day_file(path)

        received_at = now.isoformat()
        for evt in events:
            entry = {
                "timestamp": evt.get("timestamp", 0),
                "severity": evt.get("severity", "INFO"),
                "subsystem": evt.get("subsystem", "unknown"),
                "code": evt.get("code", 0),
                "message": evt.get("message", ""),
                "value": evt.get("value", 0.0),
                "device_id": device_id,
                "boot_count": boot_count,
                "received_at": received_at,
            }
            existing.append(entry)

        self._write_day_file(path, existing)
        return len(events)

    def query_events(
        self,
        device_id: Optional[str] = None,
        since: Optional[float] = None,
        severity: Optional[str] = None,
        limit: int = 100,
    ) -> list[dict]:
        """Query diagnostic events with optional filters.

        Args:
            device_id: Filter to a specific device (None = all devices).
            since: Only events with timestamp >= this epoch value.
            severity: Minimum severity level (e.g. "WARN" includes WARN, ERROR, CRITICAL).
            limit: Maximum number of events to return.

        Returns:
            Events sorted by timestamp descending.
        """
        min_rank = SEVERITY_RANK.get((severity or "").upper(), 0)

        all_events = []

        if device_id:
            device_ids = [device_id]
        else:
            # Scan all device directories
            device_ids = []
            if self.diag_dir.exists():
                for d in self.diag_dir.iterdir():
                    if d.is_dir():
                        device_ids.append(d.name)

        for did in device_ids:
            dev_dir = self.diag_dir / self._safe_id(did)
            if not dev_dir.exists():
                continue
            for fp in sorted(dev_dir.glob("*.json"), reverse=True):
                events = self._read_day_file(fp)
                for evt in events:
                    # Apply timestamp filter
                    if since is not None and evt.get("timestamp", 0) < since:
                        continue
                    # Apply severity filter
                    evt_rank = SEVERITY_RANK.get(
                        evt.get("severity", "INFO").upper(), 0
                    )
                    if evt_rank < min_rank:
                        continue
                    all_events.append(evt)

        # Sort by timestamp descending
        all_events.sort(key=lambda e: e.get("timestamp", 0), reverse=True)
        return all_events[:limit]

    def get_summary(self) -> dict:
        """Aggregate stats across the fleet.

        Returns:
            per_device: {device_id: count}
            per_subsystem: {subsystem: count}
            most_frequent_errors: [(message, count), ...]
            critical_devices: [device_id, ...]
            total_events: int
        """
        per_device: Counter = Counter()
        per_subsystem: Counter = Counter()
        error_messages: Counter = Counter()
        critical_devices: set = set()
        total = 0

        if not self.diag_dir.exists():
            return {
                "per_device": {},
                "per_subsystem": {},
                "most_frequent_errors": [],
                "critical_devices": [],
                "total_events": 0,
            }

        for dev_dir in self.diag_dir.iterdir():
            if not dev_dir.is_dir():
                continue
            did = dev_dir.name
            for fp in dev_dir.glob("*.json"):
                events = self._read_day_file(fp)
                for evt in events:
                    total += 1
                    per_device[did] += 1
                    per_subsystem[evt.get("subsystem", "unknown")] += 1

                    sev = evt.get("severity", "INFO").upper()
                    if sev in ("ERROR", "CRITICAL"):
                        msg = evt.get("message", "(no message)")
                        error_messages[msg] += 1
                    if sev == "CRITICAL":
                        critical_devices.add(did)

        return {
            "per_device": dict(per_device),
            "per_subsystem": dict(per_subsystem),
            "most_frequent_errors": [
                {"message": msg, "count": cnt}
                for msg, cnt in error_messages.most_common(20)
            ],
            "critical_devices": sorted(critical_devices),
            "total_events": total,
        }
