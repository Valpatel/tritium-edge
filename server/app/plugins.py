# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""Plugin loader — discovers and registers plugins from plugins/ directory.

Follows the tritium-sc plugin pattern. Each plugin is a directory with:
  - plugin.json  (manifest with name, version, entry, hooks)
  - entry .py    (Python module with hook functions)

Plugin hooks:
  - register_routes(app)     — add FastAPI routes
  - register_commands(store) — add device command types
  - on_heartbeat(device, payload) -> dict  — process heartbeat
  - get_ui_pages() -> list[dict]  — sidebar nav entries
"""

import importlib.util
import json
import sys
from pathlib import Path
from typing import Any

from fastapi import FastAPI


class PluginRegistry:
    """Manages loaded plugins and their hooks."""

    def __init__(self):
        self.plugins: dict[str, dict] = {}
        self._heartbeat_hooks: list[Any] = []
        self._ui_pages: list[dict] = []

    def load_plugins(self, app: FastAPI, plugins_dir: str | Path) -> int:
        """Discover and load plugins. Returns count of loaded plugins."""
        plugins_path = Path(plugins_dir)
        if not plugins_path.is_dir():
            return 0

        loaded = 0
        for plugin_dir in sorted(plugins_path.iterdir()):
            if not plugin_dir.is_dir():
                continue
            manifest_path = plugin_dir / "plugin.json"
            if not manifest_path.exists():
                continue

            try:
                manifest = json.loads(manifest_path.read_text())
                name = manifest.get("name", plugin_dir.name)
                entry = manifest.get("entry", "main.py")
                entry_path = plugin_dir / entry

                if not entry_path.exists():
                    print(f"  Plugin {name}: entry {entry} not found, skipping")
                    continue

                # Load the module
                spec = importlib.util.spec_from_file_location(
                    f"plugins.{name}", str(entry_path)
                )
                module = importlib.util.module_from_spec(spec)
                sys.modules[f"plugins.{name}"] = module
                spec.loader.exec_module(module)

                hooks = manifest.get("hooks", {})

                # Register routes
                if hooks.get("routes") and hasattr(module, "register_routes"):
                    module.register_routes(app)

                # Register heartbeat hook
                if hooks.get("heartbeat") and hasattr(module, "on_heartbeat"):
                    self._heartbeat_hooks.append(module.on_heartbeat)

                # Register UI pages
                if hooks.get("ui_pages") and hasattr(module, "get_ui_pages"):
                    pages = module.get_ui_pages()
                    self._ui_pages.extend(pages)

                self.plugins[name] = {
                    "manifest": manifest,
                    "module": module,
                    "path": str(plugin_dir),
                }
                loaded += 1
                print(f"  Plugin loaded: {name} v{manifest.get('version', '?')}")

            except Exception as e:
                print(f"  Plugin {plugin_dir.name}: failed to load — {e}")

        return loaded

    def process_heartbeat(self, device: dict, payload: dict) -> dict:
        """Run all heartbeat hooks, merge extra response fields."""
        extra = {}
        for hook in self._heartbeat_hooks:
            try:
                result = hook(device, payload)
                if isinstance(result, dict):
                    extra.update(result)
            except Exception:
                pass
        return extra

    def get_ui_pages(self) -> list[dict]:
        """Get all plugin-contributed UI pages."""
        return list(self._ui_pages)


# Singleton
plugin_registry = PluginRegistry()
