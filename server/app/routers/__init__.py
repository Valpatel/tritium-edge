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

__all__ = [
    "devices_router",
    "firmware_router",
    "ota_router",
    "commission_router",
    "events_router",
    "stats_router",
]
