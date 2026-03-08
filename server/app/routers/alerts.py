# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Alert/webhook endpoints — register webhooks, view alert history, send test alerts."""

from fastapi import APIRouter, HTTPException, Query, Request
from pydantic import BaseModel

from ..services.alert_service import get_alert_service

router = APIRouter(prefix="/api/alerts", tags=["alerts"])


class WebhookCreate(BaseModel):
    url: str
    name: str = ""
    severity_min: float = 0.0
    device_ids: list[str] = []
    event_types: list[str] = []


# --- Webhook CRUD ---

@router.post("/webhooks")
async def register_webhook(body: WebhookCreate):
    """Register a webhook URL with optional filters."""
    svc = get_alert_service()
    if svc is None:
        raise HTTPException(503, "Alert service not initialized")
    webhook = svc.register_webhook(
        url=body.url,
        name=body.name,
        severity_min=body.severity_min,
        device_ids=body.device_ids,
        event_types=body.event_types,
    )
    return webhook


@router.get("/webhooks")
async def list_webhooks():
    """List all registered webhooks."""
    svc = get_alert_service()
    if svc is None:
        raise HTTPException(503, "Alert service not initialized")
    return svc.list_webhooks()


@router.delete("/webhooks/{webhook_id}")
async def delete_webhook(webhook_id: str):
    """Remove a webhook registration."""
    svc = get_alert_service()
    if svc is None:
        raise HTTPException(503, "Alert service not initialized")
    if not svc.delete_webhook(webhook_id):
        raise HTTPException(404, "Webhook not found")
    return {"status": "deleted", "webhook_id": webhook_id}


# --- Alert history ---

@router.get("/history")
async def alert_history(limit: int = Query(50)):
    """Recent alert history (newest first)."""
    svc = get_alert_service()
    if svc is None:
        raise HTTPException(503, "Alert service not initialized")
    return svc.get_history(limit=limit)


# --- Test alert ---

@router.post("/test")
async def send_test_alert():
    """Fire a test alert to all registered webhooks."""
    svc = get_alert_service()
    if svc is None:
        raise HTTPException(503, "Alert service not initialized")
    alert = svc.fire_alert(
        event_type="test_alert",
        device_id="test-device",
        detail="This is a test alert from Tritium-Edge",
        severity=1.0,
    )
    return alert
