# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tritium-Edge Management Server — FastAPI entry point.

Modular replacement for fleet_server.py monolith.
Follows tritium-sc patterns: lifespan, config, router registration.
"""

import os
import time
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates

from .config import settings
from .store import FleetStore
from .auth.middleware import AuthMiddleware

# Shared BLE store from tritium-lib
try:
    from tritium_lib.store.ble import BleStore
except ImportError:
    BleStore = None  # type: ignore[assignment,misc]
from .plugins import plugin_registry
from .services.alert_service import init_alert_service
from .routers import (
    devices_router,
    firmware_router,
    ota_router,
    commission_router,
    events_router,
    stats_router,
    profiles_router,
    ws_router,
    presence_router,
    map_router,
    fleet_ota_router,
    provision_router,
    diagnostics_router,
    alerts_router,
    sightings_router,
)


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Server startup and shutdown."""
    # Initialize store
    store = FleetStore(settings.data_dir)
    app.state.store = store
    app.state.start_time = time.time()

    # Initialize shared BLE sighting store (SQLite via tritium-lib)
    if BleStore is not None:
        ble_db_path = Path(settings.data_dir) / "ble_sightings.db"
        ble_store = BleStore(str(ble_db_path))
        app.state.ble_store = ble_store
        print(f"  BLE Store: {ble_db_path}")
    else:
        app.state.ble_store = None
        print(f"  BLE Store: disabled (tritium-lib not installed)")

    # Initialize alert service
    alert_svc = init_alert_service(settings.data_dir)
    app.state.alert_service = alert_svc

    # API key setup (backward compat)
    if settings.api_key:
        print(f"  Auth: API key configured")
    elif settings.secret_key:
        print(f"  Auth: JWT configured")
    else:
        print(f"  Auth: DISABLED (dev mode)")

    # Load plugins
    if settings.plugins_enabled:
        count = plugin_registry.load_plugins(app, settings.plugins_dir)
        if count:
            print(f"  Plugins: {count} loaded")

    devices = store.list_devices()
    firmware = store.list_firmware()
    use_ssl = settings.ssl_cert and settings.ssl_key
    proto = "https" if use_ssl else "http"

    print(f"Tritium-Edge Management Server v3.0")
    print(f"  URL: {proto}://{settings.host}:{settings.port}/")
    print(f"  Data: {os.path.abspath(settings.data_dir)}")
    print(f"  Devices: {len(devices)}  Firmware: {len(firmware)}")

    yield

    # Shutdown
    print("Tritium-Edge shutting down")


# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------
app = FastAPI(
    title="Tritium-Edge",
    version="3.0.0",
    description="Software Defined IoT — Edge device fleet management",
    lifespan=lifespan,
)

# Middleware
app.add_middleware(AuthMiddleware)

# Register routers
app.include_router(devices_router)
app.include_router(firmware_router)
app.include_router(ota_router)
app.include_router(commission_router)
app.include_router(events_router)
app.include_router(stats_router)
app.include_router(profiles_router)
app.include_router(ws_router)
app.include_router(presence_router)
app.include_router(map_router)
app.include_router(fleet_ota_router)
app.include_router(provision_router)
app.include_router(diagnostics_router)
app.include_router(alerts_router)
app.include_router(sightings_router)

# Templates (reuse existing admin.html)
_server_dir = Path(__file__).parent.parent
templates = Jinja2Templates(directory=str(_server_dir / "templates"))


@app.get("/", response_class=HTMLResponse)
async def admin_panel(request: Request):
    """Serve the admin SPA."""
    return templates.TemplateResponse("admin.html", {"request": request})


@app.get("/map", response_class=HTMLResponse)
async def fleet_map(request: Request):
    """Serve the standalone fleet map page."""
    return templates.TemplateResponse("map.html", {"request": request})


@app.get("/api/plugins/pages")
async def plugin_pages():
    """List plugin-contributed UI pages for admin sidebar."""
    return plugin_registry.get_ui_pages()
