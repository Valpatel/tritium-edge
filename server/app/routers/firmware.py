# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Firmware management endpoints."""

import hashlib
import re
import uuid
from datetime import datetime, timezone

from fastapi import APIRouter, File, Form, HTTPException, Request, UploadFile
from fastapi.responses import FileResponse

from ..services.ota import parse_ota_header, compute_crc32

router = APIRouter(prefix="/api", tags=["firmware"])

MAX_FW_SIZE = 16 * 1024 * 1024  # 16MB max (matches ESP32-S3 flash)


def _get_store(request: Request):
    return request.app.state.store


@router.get("/firmware")
async def list_firmware(request: Request):
    return _get_store(request).list_firmware()


@router.post("/firmware/upload")
async def upload_firmware(
    request: Request,
    file: UploadFile = File(...),
    version: str = Form(None),
    board: str = Form("any"),
    notes: str = Form(""),
):
    data = await file.read()
    if len(data) < 256:
        raise HTTPException(400, "File too small")
    if len(data) > MAX_FW_SIZE:
        raise HTTPException(400, f"File too large ({len(data)} > {MAX_FW_SIZE})")

    store = _get_store(request)
    fw_id = f"fw-{uuid.uuid4().hex[:8]}"
    ota_info = parse_ota_header(data)

    if ota_info:
        fw_crc = ota_info["firmware_crc32"]
        fw_size = ota_info["firmware_size"]
        if not version:
            version = ota_info["version"]
        if board == "any" and ota_info["board"]:
            board = ota_info["board"]
        signed = ota_info["signed"]
        encrypted = ota_info.get("encrypted", False)
        ext = ".ota"
    else:
        fw_crc = compute_crc32(data)
        fw_size = len(data)
        signed = False
        encrypted = False
        ext = ".bin"

    if not version:
        version = "unknown"

    (store.firmware_dir / f"{fw_id}{ext}").write_bytes(data)

    safe_filename = re.sub(r'[^\w.\-]', '_', file.filename or "firmware")
    meta = {
        "id": fw_id,
        "filename": safe_filename,
        "version": version,
        "board": board,
        "size": fw_size,
        "total_size": len(data),
        "crc32": f"0x{fw_crc:08X}",
        "signed": signed,
        "encrypted": encrypted,
        "sha256": hashlib.sha256(data).hexdigest(),
        "uploaded_at": datetime.now(timezone.utc).isoformat(),
        "notes": notes,
        "deploy_count": 0,
    }
    store.save_firmware_meta(meta)
    store.add_event("firmware_uploaded", detail=f"{version} ({fw_id}), {len(data)} bytes")
    return meta


@router.get("/firmware/{fw_id}/download")
async def download_firmware(fw_id: str, request: Request):
    path = _get_store(request).get_firmware_path(fw_id)
    if not path:
        raise HTTPException(404, "Firmware not found")
    return FileResponse(path, filename=path.name)


@router.delete("/firmware/{fw_id}")
async def delete_firmware(fw_id: str, request: Request):
    store = _get_store(request)
    store.delete_firmware(fw_id)
    store.add_event("firmware_deleted", detail=fw_id)
    return {"status": "deleted"}
