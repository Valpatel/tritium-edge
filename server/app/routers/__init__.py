# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""API routers — one per domain, following tritium-sc pattern."""

from .devices import router as devices_router
from .firmware import router as firmware_router
from .ota import router as ota_router
from .commission import router as commission_router
from .events import router as events_router
from .stats import router as stats_router
from .profiles import router as profiles_router
from .ws import router as ws_router
from .presence import router as presence_router
from .map import router as map_router
from .fleet_ota import router as fleet_ota_router
from .provision import router as provision_router
from .diagnostics import router as diagnostics_router

__all__ = [
    "devices_router",
    "firmware_router",
    "ota_router",
    "commission_router",
    "events_router",
    "stats_router",
    "profiles_router",
    "ws_router",
    "presence_router",
    "map_router",
    "fleet_ota_router",
    "provision_router",
    "diagnostics_router",
]
