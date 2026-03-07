# Tritium-Edge Plugin System

Copyright 2026 Valpatel Software LLC. Created by Matthew Valancy. Licensed under AGPL-3.0.

Tritium-Edge supports a plugin architecture that allows extending the server with
custom functionality without modifying core code. Plugins can add API routes,
device command types, heartbeat processors, and admin UI pages.

---

## Table of Contents

- [Plugin Architecture](#plugin-architecture)
- [Plugin Manifest](#plugin-manifest-pluginjson)
- [Plugin API](#plugin-api)
- [Plugin Lifecycle](#plugin-lifecycle)
- [Plugin Examples](#plugin-examples)
- [Plugin Isolation](#plugin-isolation)
- [Configuration](#configuration)
- [Developing Plugins](#developing-plugins)

---

## Plugin Architecture

Plugins are Python packages dropped into the `plugins/` directory at the server root.
Each plugin is a self-contained directory with a manifest file and a Python entry module.

```
server/
  plugins/
    alert-email/
      plugin.json
      main.py
      templates/
    mqtt-bridge/
      plugin.json
      main.py
    grafana-export/
      plugin.json
      main.py
```

On server startup, when `plugins_enabled: true` is set in the server configuration,
the plugin loader scans the `plugins/` directory and auto-discovers all valid plugins.
Each subdirectory containing a valid `plugin.json` manifest is loaded and initialized
in dependency order.

Plugins can extend Tritium-Edge in four ways:

| Extension Point | Hook | Description |
|----------------|------|-------------|
| API Routes | `routes` | Add custom HTTP endpoints under `/api/plugins/{name}/` |
| Device Commands | `commands` | Register new command types that can be dispatched to devices |
| Heartbeat Processing | `heartbeat` | Inspect and augment heartbeat data from devices |
| Admin UI Pages | `ui_pages` | Add navigation entries and pages to the admin dashboard |

---

## Plugin Manifest (plugin.json)

Every plugin must include a `plugin.json` file in its root directory. This manifest
declares the plugin's metadata and which hooks it implements.

```json
{
  "name": "my-plugin",
  "version": "1.0.0",
  "description": "A brief description of what this plugin does",
  "author": "Author Name",
  "entry": "main.py",
  "requires": ["tritium-edge>=1.0"],
  "hooks": {
    "routes": true,
    "commands": true,
    "heartbeat": true,
    "ui_pages": true
  }
}
```

### Manifest Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | Yes | Unique plugin identifier (lowercase, hyphens allowed) |
| `version` | string | Yes | Semver version string |
| `description` | string | Yes | Human-readable description |
| `author` | string | Yes | Plugin author |
| `entry` | string | Yes | Python module filename relative to plugin directory |
| `requires` | list | No | Version constraints for Tritium-Edge or other plugins |
| `hooks` | object | Yes | Declares which extension points the plugin implements |

The `hooks` object keys correspond to the four extension points. Set a hook to `true`
if the plugin implements that interface. The plugin loader only calls hook functions
that are declared in the manifest.

---

## Plugin API

The entry module specified in `plugin.json` must implement one or more of the following
functions, corresponding to the hooks declared in the manifest.

### register_routes(app: FastAPI)

Called once during startup. The plugin receives the FastAPI application instance and
can mount additional endpoints.

```python
from fastapi import APIRouter

router = APIRouter()

@router.get("/status")
async def plugin_status():
    return {"status": "active", "alerts_sent": 42}

@router.post("/configure")
async def configure(config: dict):
    # Update plugin configuration
    return {"ok": True}

def register_routes(app):
    app.include_router(router, prefix="/api/plugins/alert-email")
```

All plugin routes are automatically prefixed with `/api/plugins/{plugin_name}/` to
avoid collisions with core routes or other plugins.

### register_commands(registry: CommandRegistry)

Called once during startup. The plugin receives the command registry and can add
new command types that the server can dispatch to devices.

```python
def register_commands(registry):
    registry.register(
        name="capture_photo",
        schema={
            "resolution": {"type": "string", "enum": ["VGA", "QVGA", "SVGA"]},
            "quality": {"type": "integer", "min": 1, "max": 100}
        },
        handler=handle_capture_photo
    )

async def handle_capture_photo(device_id: str, params: dict):
    # Build the command payload for the device
    return {
        "cmd": "capture_photo",
        "resolution": params.get("resolution", "VGA"),
        "quality": params.get("quality", 80)
    }
```

### on_heartbeat(device: dict, payload: dict) -> dict

Called on every device heartbeat. Receives the device record and the raw heartbeat
payload. Returns a dictionary of extra fields to merge into the heartbeat response
sent back to the device.

```python
import smtplib

def on_heartbeat(device, payload):
    uptime = payload.get("uptime_s", 0)
    heap_free = payload.get("heap_free", 0)

    if heap_free < 50000:
        send_alert(device["device_id"], f"Low heap: {heap_free} bytes")

    # Extra fields are merged into the server's heartbeat response
    return {"alert_status": "ok"}
```

Heartbeat hooks run synchronously in the request path. Keep processing lightweight
to avoid adding latency to heartbeat responses.

### get_ui_pages() -> list[dict]

Called once during startup. Returns a list of page definitions that the admin
dashboard renders in its sidebar navigation.

```python
def get_ui_pages():
    return [
        {
            "title": "Email Alerts",
            "path": "/plugins/alert-email",
            "icon": "mail",
            "template": "templates/alerts.html"
        },
        {
            "title": "Alert History",
            "path": "/plugins/alert-email/history",
            "icon": "history",
            "template": "templates/history.html"
        }
    ]
```

---

## Plugin Lifecycle

1. **Discovery** -- The plugin loader scans `plugins/` for directories containing `plugin.json`.
2. **Validation** -- Each manifest is validated for required fields and version constraints.
3. **Dependency Resolution** -- Plugins are sorted by their `requires` field. Missing dependencies cause the plugin to be skipped with a warning.
4. **Loading** -- The entry module is imported. Hook functions are resolved.
5. **Registration** -- `register_routes()` and `register_commands()` are called once.
6. **Runtime** -- `on_heartbeat()` is called on each heartbeat. UI pages are served by the admin dashboard.
7. **Shutdown** -- If the entry module defines `shutdown()`, it is called during server shutdown for cleanup.

---

## Plugin Examples

### plugins/alert-email/

Sends email notifications when a device goes offline or fails attestation.

```
plugins/alert-email/
  plugin.json
  main.py
  templates/
    alerts.html
    history.html
```

Hooks: `routes`, `heartbeat`, `ui_pages`. Monitors heartbeat timestamps and fires
SMTP alerts when a device misses its expected check-in window. Provides an admin
page to configure recipients and view alert history.

### plugins/mqtt-bridge/

Forwards heartbeat data and telemetry to an MQTT broker for integration with
tritium-sc or other systems.

```
plugins/mqtt-bridge/
  plugin.json
  main.py
```

Hooks: `heartbeat`, `routes`. On each heartbeat, publishes device telemetry to
configurable MQTT topics. Provides a `/configure` endpoint for broker settings.

### plugins/grafana-export/

Exports device metrics to InfluxDB for Grafana dashboards.

```
plugins/grafana-export/
  plugin.json
  main.py
```

Hooks: `heartbeat`, `routes`, `ui_pages`. Writes time-series data (heap, uptime,
RSSI, temperature) to InfluxDB on each heartbeat. Admin page shows export status.

### plugins/custom-sensors/

Adds custom sensor types and configuration options for specialized hardware.

```
plugins/custom-sensors/
  plugin.json
  main.py
  sensor_defs/
    air_quality.json
    vibration.json
```

Hooks: `commands`, `routes`, `ui_pages`. Registers new command types for sensor
configuration and calibration. Admin pages for managing sensor definitions.

---

## Plugin Isolation

Plugins run in-process within the server's Python runtime. There is no sandboxing
in v1. This means:

- Plugins share the same event loop and memory space as the core server.
- A misbehaving plugin can block the event loop or crash the server.
- Only install plugins from trusted sources.

Each plugin receives its own data directory for persistent storage:

```
data/plugins/{plugin_name}/
```

Plugins should use this directory for configuration files, caches, and any local
state. The core server does not read or modify plugin data directories.

Route isolation is enforced by prefix. All plugin API routes live under
`/api/plugins/{plugin_name}/` and cannot shadow core routes. Plugin UI pages
are rendered within the admin single-page application and cannot replace core pages.

---

## Configuration

Enable the plugin system in the server configuration:

```yaml
plugins_dir: plugins
plugins_enabled: true
```

| Key | Default | Description |
|-----|---------|-------------|
| `plugins_dir` | `plugins` | Path to the plugins directory (relative to server root) |
| `plugins_enabled` | `false` | Set to `true` to enable plugin auto-discovery |

Individual plugins can be disabled without removing them by adding a `.disabled`
file to the plugin directory:

```bash
touch plugins/alert-email/.disabled
```

---

## Developing Plugins

### Minimal Plugin

The smallest valid plugin requires two files:

```json
// plugins/hello/plugin.json
{
  "name": "hello",
  "version": "0.1.0",
  "description": "Hello world plugin",
  "author": "Developer",
  "entry": "main.py",
  "requires": [],
  "hooks": {
    "routes": true
  }
}
```

```python
# plugins/hello/main.py
from fastapi import APIRouter

router = APIRouter()

@router.get("/hello")
async def hello():
    return {"message": "Hello from plugin"}

def register_routes(app):
    app.include_router(router, prefix="/api/plugins/hello")
```

### Testing Plugins

Run the server with plugins enabled and verify:

```bash
# Start the server
python -m tritium_edge --plugins-enabled

# Test plugin route
curl http://localhost:8000/api/plugins/hello/hello

# Check plugin status via admin API
curl http://localhost:8000/api/admin/plugins
```

The admin API exposes a `/api/admin/plugins` endpoint that lists all discovered
plugins, their status (loaded, disabled, error), and version information.
