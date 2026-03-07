# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Tritium-Edge Management Server — FastAPI entry point.

Modular replacement for fleet_server.py monolith.
Follows tritium-sc patterns: lifespan, config, router registration.
"""

import os
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates

from .config import settings
from .store import FleetStore
from .auth.middleware import AuthMiddleware
from .plugins import plugin_registry
from .routers import (
    devices_router,
    firmware_router,
    ota_router,
    commission_router,
    events_router,
    stats_router,
    profiles_router,
    ws_router,
)


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Server startup and shutdown."""
    # Initialize store
    store = FleetStore(settings.data_dir)
    app.state.store = store

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

# Templates (reuse existing admin.html)
_server_dir = Path(__file__).parent.parent
templates = Jinja2Templates(directory=str(_server_dir / "templates"))


@app.get("/", response_class=HTMLResponse)
async def admin_panel(request: Request):
    """Serve the admin SPA."""
    return templates.TemplateResponse("admin.html", {"request": request})


@app.get("/api/plugins/pages")
async def plugin_pages():
    """List plugin-contributed UI pages for admin sidebar."""
    return plugin_registry.get_ui_pages()
