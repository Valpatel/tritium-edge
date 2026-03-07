#!/usr/bin/env python3
"""
ESP32 Fleet Management Server — Admin Panel + Device API.

Features:
  - Full admin web UI with SPA navigation
  - Device commissioning workflow (detect USB, flash, provision)
  - Remote OTA management (upload, push, rollback, fleet-wide deploy)
  - Real-time device monitoring (heartbeat, status, metrics)
  - Event/activity logging
  - Device groups and tagging

Usage:
    cd server && .venv/bin/python fleet_server.py
    .venv/bin/python fleet_server.py --port 9000 --host 0.0.0.0
"""

import argparse
import binascii
import glob as globmod
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, File, Form, HTTPException, Query, Request, UploadFile
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse
from fastapi.templating import Jinja2Templates

import uvicorn

# ---------------------------------------------------------------------------
# OTA header parsing (matches ota_header.h)
# ---------------------------------------------------------------------------
OTA_MAGIC = 0x4154304F
OTA_HEADER_SIZE = 64
OTA_SIG_SIZE = 64


def parse_ota_header(data: bytes):
    if len(data) < OTA_HEADER_SIZE:
        return None
    magic, hdr_ver, flags, fw_size, fw_crc = struct.unpack('<IHHII', data[:16])
    if magic != OTA_MAGIC:
        return None
    version = data[16:40].split(b'\x00')[0].decode(errors='replace')
    board = data[40:56].split(b'\x00')[0].decode(errors='replace')
    is_signed = (hdr_ver == 2) and (flags & 0x01)
    is_encrypted = bool(flags & 0x02)
    return {
        'firmware_size': fw_size, 'firmware_crc32': fw_crc,
        'version': version, 'board': board, 'signed': is_signed,
        'encrypted': is_encrypted,
        'total_header_size': OTA_HEADER_SIZE + (OTA_SIG_SIZE if is_signed else 0),
    }


def compute_crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Data Store
# ---------------------------------------------------------------------------
class FleetStore:
    def __init__(self, data_dir: str):
        self.data_dir = Path(data_dir)
        self.devices_dir = self.data_dir / "devices"
        self.firmware_dir = self.data_dir / "firmware"
        self.certs_dir = self.data_dir / "certs"
        self.events_dir = self.data_dir / "events"
        for d in [self.devices_dir, self.firmware_dir, self.certs_dir, self.events_dir]:
            d.mkdir(parents=True, exist_ok=True)

    # --- Devices ---
    def list_devices(self) -> list:
        devices = []
        for f in sorted(self.devices_dir.glob("*.json")):
            try:
                devices.append(json.loads(f.read_text()))
            except Exception:
                pass
        return devices

    @staticmethod
    def _safe_id(id_str: str) -> str:
        """Validate ID contains only safe characters (no path traversal)."""
        if not id_str or not re.match(r'^[a-zA-Z0-9_\-]+$', id_str):
            raise ValueError(f"Invalid ID: {id_str!r}")
        return id_str

    def get_device(self, device_id: str) -> Optional[dict]:
        self._safe_id(device_id)
        p = self.devices_dir / f"{device_id}.json"
        return json.loads(p.read_text()) if p.exists() else None

    def save_device(self, device: dict):
        self._safe_id(device['device_id'])
        p = self.devices_dir / f"{device['device_id']}.json"
        p.write_text(json.dumps(device, indent=2, default=str) + "\n")

    def delete_device(self, device_id: str):
        self._safe_id(device_id)
        p = self.devices_dir / f"{device_id}.json"
        if p.exists():
            p.unlink()

    # --- Firmware ---
    def list_firmware(self) -> list:
        fws = []
        for f in sorted(self.firmware_dir.glob("*.json")):
            try:
                fws.append(json.loads(f.read_text()))
            except Exception:
                pass
        fws.sort(key=lambda x: x.get('uploaded_at', ''), reverse=True)
        return fws

    def get_firmware(self, fw_id: str) -> Optional[dict]:
        self._safe_id(fw_id)
        p = self.firmware_dir / f"{fw_id}.json"
        return json.loads(p.read_text()) if p.exists() else None

    def save_firmware_meta(self, meta: dict):
        self._safe_id(meta['id'])
        p = self.firmware_dir / f"{meta['id']}.json"
        p.write_text(json.dumps(meta, indent=2, default=str) + "\n")

    def get_firmware_path(self, fw_id: str) -> Optional[Path]:
        self._safe_id(fw_id)
        for ext in ['.ota', '.bin']:
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
            fb = (fw.get('board') or 'any').lower()
            if fb == 'any' or fb in board.lower() or board.lower() in fb:
                return fw
        return all_fw[0]

    # --- Events ---
    def add_event(self, event_type: str, device_id: str = None, detail: str = "",
                  extra: dict = None):
        evt = {
            "id": uuid.uuid4().hex[:12],
            "ts": datetime.now(timezone.utc).isoformat(),
            "type": event_type,
            "device_id": device_id or "",
            "detail": detail,
        }
        if extra:
            evt.update(extra)
        # Append to daily log file
        day = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        p = self.events_dir / f"{day}.jsonl"
        with open(p, "a") as f:
            f.write(json.dumps(evt, default=str) + "\n")
        return evt

    def list_events(self, limit: int = 50, device_id: str = None) -> list:
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


# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
PROJECT_DIR = Path(__file__).parent.parent

app = FastAPI(title="ESP32 Fleet Manager", version="2.2.0")
templates = Jinja2Templates(directory=str(Path(__file__).parent / "templates"))
store: FleetStore = None
api_key: str = None  # Set by CLI --api-key or auto-generated

# Paths that devices can access without admin API key
DEVICE_PATHS = {"/api/devices/", "/api/firmware/"}
DEVICE_PATH_SUFFIXES = {"/status", "/download"}


from starlette.middleware.base import BaseHTTPMiddleware
from collections import defaultdict

# Rate limit: track failed auth attempts per IP
_auth_failures: dict[str, list[float]] = defaultdict(list)
AUTH_FAIL_WINDOW = 300   # 5 minutes
AUTH_FAIL_MAX = 10       # max failures before lockout


class APIKeyMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        path = request.url.path
        # Always allow: admin panel, static assets
        if path == "/" or not path.startswith("/api/"):
            return await call_next(request)

        # Allow device heartbeat/download without API key
        # POST /api/devices/{id}/status, GET /api/firmware/{id}/download
        if any(path.endswith(s) for s in DEVICE_PATH_SUFFIXES):
            return await call_next(request)

        # All other /api/ routes require API key
        if api_key:
            client_ip = request.client.host if request.client else "unknown"

            # Check rate limit
            now = time.time()
            fails = _auth_failures[client_ip]
            fails[:] = [t for t in fails if now - t < AUTH_FAIL_WINDOW]
            if len(fails) >= AUTH_FAIL_MAX:
                return JSONResponse({"error": "rate limited"}, status_code=429)

            key = (request.headers.get("X-API-Key") or
                   request.query_params.get("api_key"))
            if key != api_key:
                fails.append(now)
                return JSONResponse({"error": "unauthorized"}, status_code=401)

        return await call_next(request)


app.add_middleware(APIKeyMiddleware)


def enrich_devices(devices: list) -> list:
    """Add computed fields to device list."""
    now = datetime.now(timezone.utc)
    for d in devices:
        ls = d.get('last_seen')
        if ls:
            try:
                age = (now - datetime.fromisoformat(ls)).total_seconds()
                d['_online'] = age < 120
                if age < 60:
                    d['_age'] = f"{int(age)}s ago"
                elif age < 3600:
                    d['_age'] = f"{int(age/60)}m ago"
                elif age < 86400:
                    d['_age'] = f"{int(age/3600)}h ago"
                else:
                    d['_age'] = f"{int(age/86400)}d ago"
            except Exception:
                d['_online'] = False
                d['_age'] = 'unknown'
        else:
            d['_online'] = False
            d['_age'] = 'never'
    return devices


# ---------------------------------------------------------------------------
# Admin Panel (SPA)
# ---------------------------------------------------------------------------
@app.get("/", response_class=HTMLResponse)
async def admin_panel(request: Request):
    return templates.TemplateResponse("admin.html", {"request": request})


# ---------------------------------------------------------------------------
# Device API
# ---------------------------------------------------------------------------
@app.get("/api/devices")
async def api_list_devices():
    return enrich_devices(store.list_devices())


@app.get("/api/devices/{device_id}")
async def api_get_device(device_id: str):
    d = store.get_device(device_id)
    if not d:
        raise HTTPException(404, "Device not found")
    enrich_devices([d])
    return d


@app.post("/api/devices/{device_id}/status")
async def api_device_status(device_id: str, request: Request):
    try:
        FleetStore._safe_id(device_id)
    except ValueError:
        raise HTTPException(400, "Invalid device_id")
    body = await request.json()
    device = store.get_device(device_id)
    if not device:
        # Rate limit auto-registration: max 20 devices via heartbeat
        existing = store.list_devices()
        if len(existing) >= 100:
            raise HTTPException(429, "Device registration limit reached")
        device = {
            "device_id": device_id,
            "registered_at": datetime.now(timezone.utc).isoformat(),
            "tags": ["auto-registered"],
            "notes": "",
        }
        store.add_event("device_registered", device_id, "Auto-registered via heartbeat")

    device.update({
        "last_seen": datetime.now(timezone.utc).isoformat(),
        "version": body.get("version", device.get("version", "unknown")),
        "board": body.get("board", device.get("board", "unknown")),
        "partition": body.get("partition", device.get("partition")),
        "ip": body.get("ip", device.get("ip")),
        "mac": body.get("mac", device.get("mac")),
        "uptime_s": body.get("uptime_s"),
        "free_heap": body.get("free_heap"),
        "rssi": body.get("rssi"),
    })

    # Firmware attestation: verify fw_hash against known firmware
    fw_hash = body.get("fw_hash")
    if fw_hash and len(fw_hash) == 64:
        device["fw_hash"] = fw_hash
        # Check if this hash matches any known firmware
        known_fw = store.list_firmware()
        matched = False
        for fw in known_fw:
            if fw.get("sha256") == fw_hash:
                device["fw_attested"] = True
                device["fw_attested_id"] = fw["id"]
                matched = True
                break
        if not matched:
            # Hash doesn't match any known firmware — flag it
            device["fw_attested"] = False
            device["fw_attested_id"] = None
            if device.get("_prev_fw_hash") != fw_hash:
                store.add_event("attestation_unknown", device_id,
                                f"Unknown firmware hash: {fw_hash[:16]}...")
        device["_prev_fw_hash"] = fw_hash

    store.save_device(device)

    # Check for OTA result report in heartbeat
    ota_result = body.get("ota_result")
    if ota_result:
        status = ota_result.get("status", "unknown")
        version = ota_result.get("version", "unknown")
        error = ota_result.get("error", "")
        detail = f"version={version}"
        if error:
            detail += f", error={error}"
        store.add_event(f"ota_{status}", device_id, detail)
        # Clear pending OTA on success
        if status == "success" and "pending_ota" in device:
            del device["pending_ota"]
            store.save_device(device)

    response = {"status": "ok"}
    pending = device.get("pending_ota")
    if pending:
        response["ota"] = pending
    return response


@app.patch("/api/devices/{device_id}")
async def api_update_device(device_id: str, request: Request):
    """Update device metadata (name, tags, notes, group)."""
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")
    body = await request.json()
    allowed = {"device_name", "tags", "notes", "group"}
    for k, v in body.items():
        if k in allowed:
            device[k] = v
    store.save_device(device)
    store.add_event("device_updated", device_id, f"Updated: {', '.join(body.keys())}")
    return device


@app.delete("/api/devices/{device_id}")
async def api_delete_device(device_id: str):
    store.add_event("device_deleted", device_id)
    store.delete_device(device_id)
    return {"status": "deleted"}


@app.post("/api/devices/{device_id}/reboot")
async def api_reboot_device(device_id: str):
    """Schedule a reboot command for the device (picked up on next heartbeat)."""
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")
    device["pending_command"] = {"cmd": "reboot", "ts": datetime.now(timezone.utc).isoformat()}
    store.save_device(device)
    store.add_event("reboot_scheduled", device_id)
    return {"status": "reboot_scheduled"}


# ---------------------------------------------------------------------------
# Firmware API
# ---------------------------------------------------------------------------
@app.get("/api/firmware")
async def api_list_firmware():
    return store.list_firmware()


@app.post("/api/firmware/upload")
async def api_upload_firmware(
    file: UploadFile = File(...),
    version: str = Form(None),
    board: str = Form("any"),
    notes: str = Form(""),
):
    data = await file.read()
    MAX_FW_SIZE = 16 * 1024 * 1024  # 16MB max (matches ESP32-S3 flash)
    if len(data) < 256:
        raise HTTPException(400, "File too small")
    if len(data) > MAX_FW_SIZE:
        raise HTTPException(400, f"File too large ({len(data)} > {MAX_FW_SIZE})")

    fw_id = f"fw-{uuid.uuid4().hex[:8]}"
    ota_info = parse_ota_header(data)

    if ota_info:
        fw_crc = ota_info['firmware_crc32']
        fw_size = ota_info['firmware_size']
        if not version:
            version = ota_info['version']
        if board == "any" and ota_info['board']:
            board = ota_info['board']
        signed = ota_info['signed']
        encrypted = ota_info.get('encrypted', False)
        ext = '.ota'
    else:
        fw_crc = compute_crc32(data)
        fw_size = len(data)
        signed = False
        encrypted = False
        ext = '.bin'

    if not version:
        version = "unknown"

    (store.firmware_dir / f"{fw_id}{ext}").write_bytes(data)

    # Sanitize filename (prevent path injection in metadata)
    safe_filename = re.sub(r'[^\w.\-]', '_', file.filename or "firmware")
    meta = {
        "id": fw_id, "filename": safe_filename, "version": version,
        "board": board, "size": fw_size, "total_size": len(data),
        "crc32": f"0x{fw_crc:08X}", "signed": signed, "encrypted": encrypted,
        "sha256": hashlib.sha256(data).hexdigest(),
        "uploaded_at": datetime.now(timezone.utc).isoformat(),
        "notes": notes, "deploy_count": 0,
    }
    store.save_firmware_meta(meta)
    store.add_event("firmware_uploaded", detail=f"{version} ({fw_id}), {len(data)} bytes")
    return meta


@app.get("/api/firmware/{fw_id}/download")
async def api_download_firmware(fw_id: str):
    path = store.get_firmware_path(fw_id)
    if not path:
        raise HTTPException(404, "Firmware not found")
    return FileResponse(path, filename=path.name)


@app.delete("/api/firmware/{fw_id}")
async def api_delete_firmware(fw_id: str):
    FleetStore._safe_id(fw_id)
    for ext in ['.json', '.ota', '.bin']:
        p = store.firmware_dir / f"{fw_id}{ext}"
        if p.exists():
            p.unlink()
    store.add_event("firmware_deleted", detail=fw_id)
    return {"status": "deleted"}


# ---------------------------------------------------------------------------
# OTA Push API
# ---------------------------------------------------------------------------
@app.post("/api/ota/push/{device_id}")
async def api_ota_push(device_id: str, request: Request):
    body = await request.json()
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404, "Device not found")

    fw_id = body.get("firmware_id")
    if body.get("latest"):
        fw = store.get_latest_firmware(device.get("board"))
        if not fw:
            raise HTTPException(404, "No firmware available")
        fw_id = fw['id']
    if not fw_id:
        raise HTTPException(400, "firmware_id or latest required")

    fw = store.get_firmware(fw_id)
    if not fw:
        raise HTTPException(404, f"Firmware {fw_id} not found")

    device['pending_ota'] = {
        "firmware_id": fw_id, "version": fw['version'],
        "size": fw['total_size'],
        "url": f"/api/firmware/{fw_id}/download",
        "scheduled_at": datetime.now(timezone.utc).isoformat(),
    }
    store.save_device(device)
    fw['deploy_count'] = fw.get('deploy_count', 0) + 1
    store.save_firmware_meta(fw)
    store.add_event("ota_scheduled", device_id, f"Firmware {fw['version']} ({fw_id})")
    return {"status": "scheduled", "device_id": device_id, "firmware_id": fw_id}


@app.post("/api/ota/push-all")
async def api_ota_push_all(request: Request):
    body = await request.json()
    fw_id = body.get("firmware_id")
    if body.get("latest"):
        fw = store.get_latest_firmware()
        if not fw:
            raise HTTPException(404, "No firmware available")
        fw_id = fw['id']
    if not fw_id:
        raise HTTPException(400, "firmware_id or latest required")
    fw = store.get_firmware(fw_id)
    if not fw:
        raise HTTPException(404)

    devices = store.list_devices()
    updated = 0
    for device in devices:
        device['pending_ota'] = {
            "firmware_id": fw_id, "version": fw['version'],
            "size": fw['total_size'],
            "url": f"/api/firmware/{fw_id}/download",
            "scheduled_at": datetime.now(timezone.utc).isoformat(),
        }
        store.save_device(device)
        updated += 1

    fw['deploy_count'] = fw.get('deploy_count', 0) + updated
    store.save_firmware_meta(fw)
    store.add_event("ota_fleet_push", detail=f"{fw['version']} -> {updated} devices")
    return {"status": "scheduled", "devices_updated": updated, "firmware_id": fw_id}


@app.post("/api/ota/clear/{device_id}")
async def api_ota_clear(device_id: str):
    device = store.get_device(device_id)
    if not device:
        raise HTTPException(404)
    device.pop('pending_ota', None)
    store.save_device(device)
    return {"status": "cleared"}


# ---------------------------------------------------------------------------
# Commissioning API
# ---------------------------------------------------------------------------
@app.get("/api/commission/ports")
async def api_list_serial_ports():
    """Detect USB serial ports with ESP32 devices."""
    ports = []
    for p in sorted(globmod.glob("/dev/ttyACM*")) + sorted(globmod.glob("/dev/ttyUSB*")):
        info = {"port": p, "device": None, "busy": False}
        try:
            import serial
            s = serial.Serial(p, 115200, timeout=2)
            time.sleep(0.5)
            s.reset_input_buffer()
            s.write(b"IDENTIFY\n")
            time.sleep(1)
            data = s.read(2048).decode(errors='replace')
            s.close()
            for line in data.split('\n'):
                line = line.strip()
                if line.startswith('{') and '"board"' in line:
                    try:
                        info["device"] = json.loads(line)
                    except Exception:
                        pass
                    break
        except Exception as e:
            info["busy"] = True
            info["error"] = str(e)
        ports.append(info)
    return ports


@app.post("/api/commission/flash")
async def api_commission_flash(request: Request):
    """Flash firmware to a device via serial."""
    body = await request.json()
    port = body.get("port")
    board = body.get("board")
    if not port or not board:
        raise HTTPException(400, "port and board required")

    # Validate port path
    if not re.match(r'^/dev/tty(ACM|USB)\d+$', port):
        raise HTTPException(400, "Invalid port")

    # Validate board name (alphanumeric, dash, underscore only)
    if not re.match(r'^[a-zA-Z0-9_\-]+$', board):
        raise HTTPException(400, "Invalid board name")

    env = f"{board}-ota"
    cmd = ["pio", "run", "-e", env, "-t", "upload", "--upload-port", port]
    try:
        result = subprocess.run(cmd, cwd=str(PROJECT_DIR),
                                capture_output=True, text=True, timeout=120)
        success = result.returncode == 0
        store.add_event("commission_flash", detail=f"{board} on {port}: {'OK' if success else 'FAIL'}")
        return {
            "success": success,
            "output": (result.stdout + result.stderr)[-2000:],
        }
    except subprocess.TimeoutExpired:
        return {"success": False, "output": "Flash timed out (120s)"}


@app.post("/api/commission/provision")
async def api_commission_provision(request: Request):
    """Provision a device over serial USB."""
    body = await request.json()
    port = body.get("port")
    device_name = body.get("device_name", "")
    server_url = body.get("server_url", "")
    wifi_ssid = body.get("wifi_ssid", "")
    wifi_pass = body.get("wifi_pass", "")

    if not port:
        raise HTTPException(400, "port required")
    if not re.match(r'^/dev/tty(ACM|USB)\d+$', port):
        raise HTTPException(400, "Invalid port")

    device_id = f"esp32-{uuid.uuid4().hex[:12]}"

    # Build provisioning JSON for the device's HAL
    prov = {
        "cmd": "provision",
        "device_id": device_id,
        "device_name": device_name,
        "server_url": server_url,
        "mqtt_broker": body.get("mqtt_broker", ""),
        "mqtt_port": body.get("mqtt_port", 8883),
    }

    steps = []
    try:
        import serial
        s = serial.Serial(port, 115200, timeout=5)
        time.sleep(1)
        s.reset_input_buffer()

        # Step 1: Begin provisioning
        s.write(b"PROVISION_BEGIN\n")
        s.flush()
        time.sleep(1)
        resp = s.read(1024).decode(errors='replace')
        ready = 'PROVISION_READY' in resp or '"ready"' in resp
        steps.append({"step": "begin", "ok": ready, "response": resp.strip()})

        if ready:
            # Step 2: Send provisioning JSON
            prov_json = json.dumps(prov, separators=(',', ':'))
            s.write((prov_json + "\n").encode())
            s.flush()
            time.sleep(3)
            resp = s.read(2048).decode(errors='replace')
            ok = '"ok"' in resp or 'Provisioned' in resp
            steps.append({"step": "provision", "ok": ok, "response": resp.strip()})

            if ok:
                # Register the device
                device = {
                    "device_id": device_id,
                    "device_name": device_name,
                    "registered_at": datetime.now(timezone.utc).isoformat(),
                    "version": "freshly-provisioned",
                    "board": "unknown",
                    "provisioned": True,
                    "tags": ["new"],
                    "notes": f"Provisioned via admin panel on {port}",
                }
                store.save_device(device)
                store.add_event("device_provisioned", device_id,
                                f"Provisioned on {port}")

        s.close()
    except Exception as e:
        steps.append({"step": "error", "ok": False, "response": str(e)})

    success = all(s["ok"] for s in steps)
    return {
        "success": success,
        "device_id": device_id if success else None,
        "steps": steps,
    }


@app.post("/api/provision/generate")
async def api_provision_generate(request: Request):
    body = await request.json()
    device_id = f"esp32-{uuid.uuid4().hex[:12]}"

    device = {
        "device_id": device_id,
        "device_name": body.get("device_name", device_id),
        "registered_at": datetime.now(timezone.utc).isoformat(),
        "version": "unprovisioned",
        "board": "unknown",
        "provisioned": False,
        "tags": ["pending"],
        "notes": "",
    }
    store.save_device(device)

    identity = {
        "device_id": device_id,
        "device_name": body.get("device_name", device_id),
        "server_url": body.get("server_url", ""),
        "mqtt_broker": body.get("mqtt_broker", ""),
        "mqtt_port": body.get("mqtt_port", 8883),
        "provisioned": True,
    }

    prov_dir = store.certs_dir / device_id
    prov_dir.mkdir(exist_ok=True)
    (prov_dir / "device.json").write_text(json.dumps(identity, indent=2) + "\n")

    if body.get("wifi_ssid"):
        wifi = {"ssid": body["wifi_ssid"], "password": body.get("wifi_pass", "")}
        (prov_dir / "factory_wifi.json").write_text(json.dumps(wifi, indent=2) + "\n")

    store.add_event("provision_generated", device_id,
                    f"Name: {body.get('device_name', device_id)}")
    return {"device_id": device_id, "identity": identity}


# ---------------------------------------------------------------------------
# Events API
# ---------------------------------------------------------------------------
@app.get("/api/events")
async def api_list_events(limit: int = Query(50), device_id: str = Query(None)):
    return store.list_events(limit=limit, device_id=device_id)


# ---------------------------------------------------------------------------
# Stats API
# ---------------------------------------------------------------------------
@app.get("/api/stats")
async def api_stats():
    devices = enrich_devices(store.list_devices())
    firmware = store.list_firmware()
    attested = sum(1 for d in devices if d.get('fw_attested') is True)
    unattested = sum(1 for d in devices if d.get('fw_attested') is False)
    no_hash = sum(1 for d in devices if 'fw_attested' not in d)
    return {
        "total_devices": len(devices),
        "online_devices": sum(1 for d in devices if d.get('_online')),
        "offline_devices": sum(1 for d in devices if not d.get('_online')),
        "pending_ota": sum(1 for d in devices if d.get('pending_ota')),
        "total_firmware": len(firmware),
        "signed_firmware": sum(1 for f in firmware if f.get('signed')),
        "boards": list(set(d.get('board', 'unknown') for d in devices)),
        "versions": list(set(d.get('version', 'unknown') for d in devices)),
        "attested_devices": attested,
        "unattested_devices": unattested,
        "no_attestation": no_hash,
    }


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    global store, api_key
    parser = argparse.ArgumentParser(description="ESP32 Fleet Manager")
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--data-dir", default="./fleet_data")
    parser.add_argument("--api-key", default=None,
                        help="API key for admin endpoints (auto-generated if not set)")
    parser.add_argument("--no-auth", action="store_true",
                        help="Disable API key authentication (dev only)")
    parser.add_argument("--ssl-cert", default=None,
                        help="TLS certificate file (.pem) for HTTPS")
    parser.add_argument("--ssl-key", default=None,
                        help="TLS private key file (.pem) for HTTPS")
    args = parser.parse_args()

    store = FleetStore(args.data_dir)

    # API key setup
    if args.no_auth:
        api_key = None
        print("  Auth: DISABLED (--no-auth)")
    else:
        key_file = Path(args.data_dir) / ".api_key"
        if args.api_key:
            api_key = args.api_key
        elif key_file.exists():
            api_key = key_file.read_text().strip()
        else:
            api_key = uuid.uuid4().hex
            key_file.write_text(api_key + "\n")
            key_file.chmod(0o600)
        print(f"  Auth: API key required (X-API-Key header)")
        print(f"  Key:  {api_key}")

    use_ssl = args.ssl_cert and args.ssl_key
    proto = "https" if use_ssl else "http"

    print(f"ESP32 Fleet Manager v2.2")
    print(f"  URL: {proto}://{args.host}:{args.port}/")
    print(f"  Data: {os.path.abspath(args.data_dir)}")
    print(f"  TLS: {'enabled' if use_ssl else 'disabled (use --ssl-cert/--ssl-key for HTTPS)'}")
    print(f"  Devices: {len(store.list_devices())}  Firmware: {len(store.list_firmware())}")

    ssl_kwargs = {}
    if use_ssl:
        ssl_kwargs["ssl_certfile"] = args.ssl_cert
        ssl_kwargs["ssl_keyfile"] = args.ssl_key

    uvicorn.run(app, host=args.host, port=args.port, log_level="info", **ssl_kwargs)


if __name__ == "__main__":
    main()
