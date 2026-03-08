# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tests for alert/webhook notification system."""

import json
from unittest.mock import patch, MagicMock

import pytest

from app.services.alert_service import get_alert_service, AlertService


@pytest.fixture(autouse=True)
def reset_alert_service(app):
    """Ensure a fresh alert service for each test."""
    svc = get_alert_service()
    if svc:
        svc._webhooks.clear()
        svc._save_webhooks()
        svc._history.clear()
    yield
    if svc:
        svc._webhooks.clear()
        svc._history.clear()


# --- Webhook CRUD ---

def test_register_webhook(client):
    """POST /api/alerts/webhooks registers a new webhook."""
    resp = client.post("/api/alerts/webhooks", json={
        "url": "https://example.com/hook",
        "name": "My Hook",
        "severity_min": 0.5,
        "device_ids": ["node-001"],
        "event_types": ["node_anomaly"],
    })
    assert resp.status_code == 200
    data = resp.json()
    assert data["url"] == "https://example.com/hook"
    assert data["name"] == "My Hook"
    assert data["severity_min"] == 0.5
    assert data["device_ids"] == ["node-001"]
    assert data["event_types"] == ["node_anomaly"]
    assert "id" in data
    assert data["fire_count"] == 0


def test_register_webhook_defaults(client):
    """POST with minimal body uses defaults."""
    resp = client.post("/api/alerts/webhooks", json={
        "url": "https://example.com/hook2",
    })
    assert resp.status_code == 200
    data = resp.json()
    assert data["severity_min"] == 0.0
    assert data["device_ids"] == []
    assert data["event_types"] == []


def test_list_webhooks_empty(client):
    """GET /api/alerts/webhooks returns empty list initially."""
    resp = client.get("/api/alerts/webhooks")
    assert resp.status_code == 200
    assert resp.json() == []


def test_list_webhooks(client):
    """GET /api/alerts/webhooks lists registered hooks."""
    client.post("/api/alerts/webhooks", json={"url": "https://a.com/1"})
    client.post("/api/alerts/webhooks", json={"url": "https://b.com/2"})

    resp = client.get("/api/alerts/webhooks")
    assert resp.status_code == 200
    hooks = resp.json()
    assert len(hooks) == 2
    urls = {h["url"] for h in hooks}
    assert "https://a.com/1" in urls
    assert "https://b.com/2" in urls


def test_delete_webhook(client):
    """DELETE /api/alerts/webhooks/{id} removes a webhook."""
    resp = client.post("/api/alerts/webhooks", json={"url": "https://del.com/x"})
    wh_id = resp.json()["id"]

    resp = client.delete(f"/api/alerts/webhooks/{wh_id}")
    assert resp.status_code == 200
    assert resp.json()["status"] == "deleted"

    # Verify gone
    resp = client.get("/api/alerts/webhooks")
    assert len(resp.json()) == 0


def test_delete_webhook_404(client):
    """DELETE nonexistent webhook returns 404."""
    resp = client.delete("/api/alerts/webhooks/nonexistent")
    assert resp.status_code == 404


# --- Alert history ---

def test_alert_history_empty(client):
    """GET /api/alerts/history returns empty list initially."""
    resp = client.get("/api/alerts/history")
    assert resp.status_code == 200
    assert resp.json() == []


def test_alert_history_after_fire(client):
    """Alert history shows fired alerts."""
    svc = get_alert_service()
    svc.fire_alert("test_event", "dev-001", "something happened", 0.8)

    resp = client.get("/api/alerts/history")
    assert resp.status_code == 200
    history = resp.json()
    assert len(history) == 1
    assert history[0]["event_type"] == "test_event"
    assert history[0]["device_id"] == "dev-001"
    assert history[0]["severity"] == 0.8


def test_alert_history_limit(client):
    """History respects limit parameter."""
    svc = get_alert_service()
    for i in range(10):
        svc.fire_alert("evt", f"dev-{i}", "detail", 0.5)

    resp = client.get("/api/alerts/history?limit=3")
    assert len(resp.json()) == 3


def test_alert_history_newest_first(client):
    """History returns newest alerts first."""
    svc = get_alert_service()
    svc.fire_alert("evt", "dev-old", "first", 0.5)
    svc.fire_alert("evt", "dev-new", "second", 0.5)

    resp = client.get("/api/alerts/history")
    history = resp.json()
    assert history[0]["device_id"] == "dev-new"
    assert history[1]["device_id"] == "dev-old"


# --- Test alert endpoint ---

def test_send_test_alert(client):
    """POST /api/alerts/test fires a test alert."""
    resp = client.post("/api/alerts/test")
    assert resp.status_code == 200
    data = resp.json()
    assert data["event_type"] == "test_alert"
    assert data["severity"] == 1.0
    assert data["device_id"] == "test-device"


def test_send_test_alert_delivers_to_webhook(client):
    """Test alert gets delivered to registered webhooks."""
    # Register a webhook
    client.post("/api/alerts/webhooks", json={"url": "https://hook.test/recv"})

    with patch("app.services.alert_service.httpx.post") as mock_post:
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_post.return_value = mock_resp

        resp = client.post("/api/alerts/test")
        data = resp.json()
        assert len(data["deliveries"]) == 1
        assert data["deliveries"][0]["ok"] is True
        mock_post.assert_called_once()


# --- Filter matching ---

def test_webhook_severity_filter(client):
    """Webhook with severity_min only receives alerts at or above threshold."""
    client.post("/api/alerts/webhooks", json={
        "url": "https://sev.test/hook",
        "severity_min": 0.7,
    })

    svc = get_alert_service()

    with patch("app.services.alert_service.httpx.post") as mock_post:
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_post.return_value = mock_resp

        # Below threshold — no delivery
        svc.fire_alert("evt", "dev-1", "minor", 0.3)
        mock_post.assert_not_called()

        # At threshold — delivered
        svc.fire_alert("evt", "dev-1", "major", 0.7)
        mock_post.assert_called_once()


def test_webhook_device_filter(client):
    """Webhook with device_ids filter only fires for matching devices."""
    client.post("/api/alerts/webhooks", json={
        "url": "https://dev.test/hook",
        "device_ids": ["node-A"],
    })

    svc = get_alert_service()

    with patch("app.services.alert_service.httpx.post") as mock_post:
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_post.return_value = mock_resp

        # Wrong device — no delivery
        svc.fire_alert("evt", "node-B", "wrong device", 0.9)
        mock_post.assert_not_called()

        # Right device — delivered
        svc.fire_alert("evt", "node-A", "right device", 0.9)
        mock_post.assert_called_once()


def test_webhook_event_type_filter(client):
    """Webhook with event_types filter only fires for matching events."""
    client.post("/api/alerts/webhooks", json={
        "url": "https://type.test/hook",
        "event_types": ["node_anomaly"],
    })

    svc = get_alert_service()

    with patch("app.services.alert_service.httpx.post") as mock_post:
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_post.return_value = mock_resp

        # Wrong type — no delivery
        svc.fire_alert("device_offline", "dev-1", "went offline", 0.9)
        mock_post.assert_not_called()

        # Right type — delivered
        svc.fire_alert("node_anomaly", "dev-1", "heap low", 0.9)
        mock_post.assert_called_once()


# --- Diagnostics integration ---

def test_diagnostics_fires_alert_for_critical_anomaly(client):
    """Submitting diagnostics with severity >= 0.7 fires an alert."""
    # Register a webhook
    client.post("/api/alerts/webhooks", json={"url": "https://diag.test/hook"})

    with patch("app.services.alert_service.httpx.post") as mock_post:
        mock_resp = MagicMock()
        mock_resp.status_code = 200
        mock_post.return_value = mock_resp

        client.post("/api/devices/node-crit/diag", json={
            "health": {"free_heap": 10000},
            "anomalies": [
                {"subsystem": "memory", "description": "Heap critical", "severity": 0.9},
                {"subsystem": "wifi", "description": "Minor issue", "severity": 0.3},
            ],
        })

        # Only the 0.9 anomaly should have fired
        assert mock_post.call_count == 1
        payload = mock_post.call_args[1]["json"]
        assert payload["severity"] == 0.9
        assert "memory" in payload["detail"]


def test_diagnostics_no_alert_for_low_severity(client):
    """Submitting diagnostics with all severities < 0.7 fires no alerts."""
    client.post("/api/alerts/webhooks", json={"url": "https://diag.test/hook"})

    with patch("app.services.alert_service.httpx.post") as mock_post:
        client.post("/api/devices/node-ok/diag", json={
            "health": {"free_heap": 200000},
            "anomalies": [
                {"subsystem": "wifi", "description": "Minor", "severity": 0.3},
            ],
        })

        mock_post.assert_not_called()


# --- Webhook persistence ---

def test_webhook_persists_to_disk(client, tmp_data_dir):
    """Webhooks survive service re-creation from same data dir."""
    client.post("/api/alerts/webhooks", json={
        "url": "https://persist.test/hook",
        "name": "Persistent",
    })

    # Recreate service from same dir
    svc = get_alert_service()
    alerts_dir = svc.data_dir
    new_svc = AlertService(alerts_dir)
    hooks = new_svc.list_webhooks()
    assert len(hooks) == 1
    assert hooks[0]["url"] == "https://persist.test/hook"
    assert hooks[0]["name"] == "Persistent"


# --- Delivery error handling ---

def test_webhook_delivery_failure(client):
    """Failed webhook delivery is recorded but doesn't crash."""
    client.post("/api/alerts/webhooks", json={"url": "https://fail.test/hook"})

    with patch("app.services.alert_service.httpx.post", side_effect=Exception("Connection refused")):
        svc = get_alert_service()
        alert = svc.fire_alert("evt", "dev-1", "test", 0.8)
        assert len(alert["deliveries"]) == 1
        assert alert["deliveries"][0]["ok"] is False
        assert "Connection refused" in alert["deliveries"][0]["error"]
