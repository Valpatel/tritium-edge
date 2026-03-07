# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Persistence layer — file-based JSON store.

The store abstraction isolates all disk I/O so we can migrate to SQLite
(tritium-sc pattern) without touching routes or services.
"""

from .fleet_store import FleetStore

__all__ = ["FleetStore"]
