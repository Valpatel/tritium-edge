# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Event log endpoints."""

from fastapi import APIRouter, Query, Request

router = APIRouter(prefix="/api", tags=["events"])


@router.get("/events")
async def list_events(
    request: Request,
    limit: int = Query(50),
    device_id: str = Query(None),
):
    return request.app.state.store.list_events(limit=limit, device_id=device_id)
