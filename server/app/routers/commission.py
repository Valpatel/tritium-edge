# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Device commissioning endpoints — serial flash, provision, detect."""

import json
import re
import subprocess
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

from fastapi import APIRouter, HTTPException, Request

router = APIRouter(prefix="/api/commission", tags=["commissioning"])

PROJECT_DIR = Path(__file__).parent.parent.parent.parent


def _get_store(request: Request):
    return request.app.state.store


@router.get("/ports")
async def list_serial_ports(request: Request):
    """Detect USB serial ports with ESP32 devices."""
    import glob as globmod

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


@router.post("/flash")
async def flash_device(request: Request):
    """Flash firmware to a device via PlatformIO."""
    body = await request.json()
    port = body.get("port")
    board = body.get("board")
    if not port or not board:
        raise HTTPException(400, "port and board required")
    if not re.match(r'^/dev/tty(ACM|USB)\d+$', port):
        raise HTTPException(400, "Invalid port")
    if not re.match(r'^[a-zA-Z0-9_\-]+$', board):
        raise HTTPException(400, "Invalid board name")

    store = _get_store(request)
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


@router.post("/provision")
async def provision_device(request: Request):
    """Provision a device over serial USB."""
    body = await request.json()
    port = body.get("port")
    if not port:
        raise HTTPException(400, "port required")
    if not re.match(r'^/dev/tty(ACM|USB)\d+$', port):
        raise HTTPException(400, "Invalid port")

    store = _get_store(request)
    device_id = f"esp32-{uuid.uuid4().hex[:12]}"

    prov = {
        "cmd": "provision",
        "device_id": device_id,
        "device_name": body.get("device_name", ""),
        "server_url": body.get("server_url", ""),
        "mqtt_broker": body.get("mqtt_broker", ""),
        "mqtt_port": body.get("mqtt_port", 8883),
    }

    steps = []
    try:
        import serial
        s = serial.Serial(port, 115200, timeout=5)
        time.sleep(1)
        s.reset_input_buffer()

        s.write(b"PROVISION_BEGIN\n")
        s.flush()
        time.sleep(1)
        resp = s.read(1024).decode(errors='replace')
        ready = 'PROVISION_READY' in resp or '"ready"' in resp
        steps.append({"step": "begin", "ok": ready, "response": resp.strip()})

        if ready:
            prov_json = json.dumps(prov, separators=(',', ':'))
            s.write((prov_json + "\n").encode())
            s.flush()
            time.sleep(3)
            resp = s.read(2048).decode(errors='replace')
            ok = '"ok"' in resp or 'Provisioned' in resp
            steps.append({"step": "provision", "ok": ok, "response": resp.strip()})

            if ok:
                device = {
                    "device_id": device_id,
                    "device_name": body.get("device_name", ""),
                    "registered_at": datetime.now(timezone.utc).isoformat(),
                    "version": "freshly-provisioned",
                    "board": "unknown",
                    "provisioned": True,
                    "tags": ["new"],
                    "notes": f"Provisioned via admin panel on {port}",
                }
                store.save_device(device)
                store.add_event("device_provisioned", device_id, f"Provisioned on {port}")

        s.close()
    except Exception as e:
        steps.append({"step": "error", "ok": False, "response": str(e)})

    success = all(s["ok"] for s in steps)
    return {
        "success": success,
        "device_id": device_id if success else None,
        "steps": steps,
    }


@router.get("/discover")
async def discover_nodes(request: Request):
    """Scan local network for Tritium nodes via mDNS and /api/node probing."""
    import asyncio
    import httpx
    from zeroconf import Zeroconf, ServiceBrowser, ServiceStateChange

    discovered = []

    # Method 1: mDNS discovery (_http._tcp.local.)
    zc = Zeroconf()
    mdns_hosts = []

    class Listener:
        def add_service(self, zc, type_, name):
            info = zc.get_service_info(type_, name)
            if info and b"tritium" in name.lower().encode():
                ip = ".".join(str(b) for b in info.addresses[0]) if info.addresses else None
                if ip:
                    mdns_hosts.append({"ip": ip, "port": info.port, "name": name})

        def remove_service(self, *args): pass
        def update_service(self, *args): pass

    listener = Listener()
    browser = ServiceBrowser(zc, "_http._tcp.local.", listener)
    await asyncio.sleep(3)  # Wait for mDNS responses
    zc.close()

    # Method 2: Probe known device IPs from store
    store = _get_store(request)
    known_ips = set()
    for dev in store.list_devices():
        ip = dev.get("ip")
        if ip and ip != "0.0.0.0":
            known_ips.add(ip)

    # Add mDNS-discovered IPs
    for h in mdns_hosts:
        known_ips.add(h["ip"])

    # Probe each IP for /api/node
    async with httpx.AsyncClient(timeout=3.0) as client:
        async def probe(ip):
            try:
                r = await client.get(f"http://{ip}/api/node")
                if r.status_code == 200:
                    data = r.json()
                    if data.get("tritium"):
                        data["discovered_ip"] = ip
                        return data
            except Exception:
                pass
            return None

        tasks = [probe(ip) for ip in known_ips]
        results = await asyncio.gather(*tasks)

    for r in results:
        if r:
            discovered.append(r)

    return {
        "discovered": discovered,
        "mdns_hosts": mdns_hosts,
        "probed_ips": list(known_ips),
        "count": len(discovered),
    }


@router.post("/generate")
async def generate_provision(request: Request):
    """Generate provisioning identity (pre-register device)."""
    body = await request.json()
    store = _get_store(request)
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
