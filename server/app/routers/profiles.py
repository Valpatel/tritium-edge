# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Device profile endpoints — reusable config templates."""

import uuid
from datetime import datetime, timezone

from fastapi import APIRouter, HTTPException, Request

router = APIRouter(prefix="/api", tags=["profiles"])


def _get_store(request: Request):
    return request.app.state.store


@router.get("/profiles")
async def list_profiles(request: Request):
    return _get_store(request).list_profiles()


@router.post("/profiles")
async def create_profile(request: Request):
    body = await request.json()
    store = _get_store(request)
    name = body.get("name", "").strip()
    if not name:
        raise HTTPException(400, "Profile name required")
    profile = {
        "id": f"prof-{uuid.uuid4().hex[:8]}",
        "name": name,
        "description": body.get("description", ""),
        "config": body.get("config", {}),
        "created_at": datetime.now(timezone.utc).isoformat(),
        "updated_at": datetime.now(timezone.utc).isoformat(),
    }
    store.save_profile(profile)
    store.add_event("profile_created", detail=f"{name} ({profile['id']})")
    return profile


@router.get("/profiles/{profile_id}")
async def get_profile(profile_id: str, request: Request):
    p = _get_store(request).get_profile(profile_id)
    if not p:
        raise HTTPException(404, "Profile not found")
    return p


@router.put("/profiles/{profile_id}")
async def update_profile(profile_id: str, request: Request):
    store = _get_store(request)
    profile = store.get_profile(profile_id)
    if not profile:
        raise HTTPException(404, "Profile not found")
    body = await request.json()
    if "name" in body:
        profile["name"] = body["name"]
    if "description" in body:
        profile["description"] = body["description"]
    if "config" in body:
        profile["config"] = body["config"]
    profile["updated_at"] = datetime.now(timezone.utc).isoformat()
    store.save_profile(profile)
    store.add_event("profile_updated", detail=profile["name"])
    return profile


@router.delete("/profiles/{profile_id}")
async def delete_profile(profile_id: str, request: Request):
    store = _get_store(request)
    store.delete_profile(profile_id)
    store.add_event("profile_deleted", detail=profile_id)
    return {"status": "deleted"}


@router.post("/profiles/{profile_id}/apply")
async def apply_profile(profile_id: str, request: Request):
    """Apply a profile's config to all devices with this profile_id."""
    store = _get_store(request)
    profile = store.get_profile(profile_id)
    if not profile:
        raise HTTPException(404, "Profile not found")
    devices = store.list_devices()
    updated = 0
    for device in devices:
        if device.get("profile_id") == profile_id:
            device["desired_config"] = {
                **device.get("desired_config", {}),
                **profile["config"],
            }
            device["_config_drift_logged"] = False
            store.save_device(device)
            updated += 1
    store.add_event("profile_applied", detail=f"{profile['name']} -> {updated} devices")
    return {"status": "applied", "devices_updated": updated}
