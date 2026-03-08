# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Alert/webhook notification service.

Stores webhook configurations on disk (JSON) and fires HTTP POST
notifications when events match registered filters. Keeps a ring buffer
of recent alerts for the history endpoint.
"""

import json
import time
import uuid
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

import httpx


# Ring buffer size for alert history
_MAX_HISTORY = 500


class AlertService:
    """Manages webhook registrations and alert dispatch."""

    def __init__(self, data_dir: str | Path):
        self.data_dir = Path(data_dir)
        self._webhooks_file = self.data_dir / "webhooks.json"
        self._webhooks: dict[str, dict] = {}
        self._history: deque[dict] = deque(maxlen=_MAX_HISTORY)
        self._load_webhooks()

    # --- Persistence ---

    def _load_webhooks(self) -> None:
        if self._webhooks_file.exists():
            try:
                data = json.loads(self._webhooks_file.read_text())
                self._webhooks = {w["id"]: w for w in data}
            except Exception:
                self._webhooks = {}

    def _save_webhooks(self) -> None:
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self._webhooks_file.write_text(
            json.dumps(list(self._webhooks.values()), indent=2, default=str) + "\n"
        )

    # --- Webhook CRUD ---

    def register_webhook(
        self,
        url: str,
        severity_min: float = 0.0,
        device_ids: list[str] | None = None,
        event_types: list[str] | None = None,
        name: str = "",
    ) -> dict:
        """Register a new webhook endpoint."""
        webhook = {
            "id": uuid.uuid4().hex[:12],
            "url": url,
            "name": name,
            "severity_min": severity_min,
            "device_ids": device_ids or [],
            "event_types": event_types or [],
            "created_at": datetime.now(timezone.utc).isoformat(),
            "fire_count": 0,
            "last_fired_at": None,
        }
        self._webhooks[webhook["id"]] = webhook
        self._save_webhooks()
        return webhook

    def list_webhooks(self) -> list[dict]:
        return list(self._webhooks.values())

    def get_webhook(self, webhook_id: str) -> Optional[dict]:
        return self._webhooks.get(webhook_id)

    def delete_webhook(self, webhook_id: str) -> bool:
        if webhook_id in self._webhooks:
            del self._webhooks[webhook_id]
            self._save_webhooks()
            return True
        return False

    # --- Alert dispatch ---

    def _matches_filter(self, webhook: dict, event_type: str,
                        device_id: str, severity: float) -> bool:
        """Check if an alert matches the webhook's filters."""
        if severity < webhook.get("severity_min", 0.0):
            return False
        allowed_devices = webhook.get("device_ids", [])
        if allowed_devices and device_id not in allowed_devices:
            return False
        allowed_types = webhook.get("event_types", [])
        if allowed_types and event_type not in allowed_types:
            return False
        return True

    def fire_alert(
        self,
        event_type: str,
        device_id: str,
        detail: str,
        severity: float,
    ) -> dict:
        """Create an alert and POST to all matching webhooks.

        Returns the alert record with delivery results.
        """
        alert = {
            "id": uuid.uuid4().hex[:12],
            "ts": datetime.now(timezone.utc).isoformat(),
            "event_type": event_type,
            "device_id": device_id,
            "detail": detail,
            "severity": severity,
            "deliveries": [],
        }

        for webhook in self._webhooks.values():
            if not self._matches_filter(webhook, event_type, device_id, severity):
                continue
            result = self._deliver(webhook, alert)
            alert["deliveries"].append(result)

        self._history.append(alert)
        return alert

    def _deliver(self, webhook: dict, alert: dict) -> dict:
        """POST alert payload to a single webhook URL."""
        payload = {
            "alert_id": alert["id"],
            "ts": alert["ts"],
            "event_type": alert["event_type"],
            "device_id": alert["device_id"],
            "detail": alert["detail"],
            "severity": alert["severity"],
        }
        result = {
            "webhook_id": webhook["id"],
            "url": webhook["url"],
        }
        try:
            resp = httpx.post(webhook["url"], json=payload, timeout=5.0)
            result["status_code"] = resp.status_code
            result["ok"] = 200 <= resp.status_code < 300
        except Exception as exc:
            result["status_code"] = None
            result["ok"] = False
            result["error"] = str(exc)

        # Update webhook stats
        webhook["fire_count"] = webhook.get("fire_count", 0) + 1
        webhook["last_fired_at"] = alert["ts"]
        self._save_webhooks()

        return result

    def get_history(self, limit: int = 50) -> list[dict]:
        """Return recent alerts, newest first."""
        items = list(self._history)
        items.reverse()
        return items[:limit]


# Module-level singleton, initialized lazily via init_alert_service()
_instance: Optional[AlertService] = None


def get_alert_service() -> Optional[AlertService]:
    return _instance


def init_alert_service(data_dir: str | Path) -> AlertService:
    global _instance
    alerts_dir = Path(data_dir) / "alerts"
    alerts_dir.mkdir(parents=True, exist_ok=True)
    _instance = AlertService(alerts_dir)
    return _instance
