# Fleet Server Requirements and Features

## Overview

The Fleet Server is a FastAPI-based management console for ESP32-S3 device fleets. It provides a dark-themed single-page admin panel, REST API for device heartbeat and control, firmware repository with OTA header parsing, remote OTA push (single device or fleet-wide), USB serial commissioning, firmware attestation, and event logging. Devices communicate via periodic heartbeat POST requests; the server responds with pending commands (OTA, reboot).

## Architecture

```
                    +----------------------------+
                    |     Admin SPA (browser)    |
                    |   Dashboard | Devices |    |
                    |   Firmware | Commission |  |
                    |   Events | Security       |
                    +------------+---------------+
                                 |
                    HTML (Jinja2) + REST API (JSON)
                                 |
                    +------------+---------------+
                    |   FastAPI v2.2 (uvicorn)   |
                    |                            |
                    |  APIKeyMiddleware           |
                    |  Rate limiter (per-IP)      |
                    |  OTA header parser          |
                    |  Firmware attestation       |
                    +------------+---------------+
                                 |
                    +------------+---------------+
                    |       FleetStore            |
                    |  (JSON files on disk)       |
                    |                            |
                    |  fleet_data/               |
                    |    devices/*.json          |
                    |    firmware/*.json + .ota  |
                    |    certs/<id>/device.json  |
                    |    events/YYYY-MM-DD.jsonl |
                    |    .api_key                |
                    +----------------------------+
                                 |
              +------------------+------------------+
              |                  |                  |
         ESP32 Device      ESP32 Device       ESP32 Device
         (heartbeat)       (heartbeat)        (heartbeat)
```

### Data Store

`FleetStore` persists all state as JSON files on disk under a configurable data directory (default `./fleet_data`):

| Directory | Contents |
|-----------|----------|
| `devices/` | One `<device_id>.json` per device (identity, status, pending commands) |
| `firmware/` | One `<fw_id>.json` metadata file + `.ota` or `.bin` binary per firmware |
| `certs/` | Per-device provisioning packages (`<device_id>/device.json`, `factory_wifi.json`) |
| `events/` | Daily JSONL event logs (`YYYY-MM-DD.jsonl`) |
| `.api_key` | Auto-generated API key (mode 0600) |

All IDs are validated against `^[a-zA-Z0-9_\-]+$` to prevent path traversal.

### Middleware

- **APIKeyMiddleware** (Starlette `BaseHTTPMiddleware`): Intercepts all `/api/` requests. Device paths (heartbeat, firmware download) are exempt. All other routes require `X-API-Key` header or `api_key` query parameter.
- Rate limiting on authentication failures: per-IP tracking, 5-minute sliding window, lockout after 10 failed attempts (HTTP 429).

## Admin Panel SPA

The admin panel is a single HTML page (`templates/admin.html`) served at `/` with client-side routing, dark neon theme, and auto-refresh every 15 seconds.

### Dashboard Page

- **Stats row**: Total devices, online count, offline count, pending OTA count, firmware image count, signed firmware count.
- **Recent devices table**: Status badge (ON/OFF), device name/ID, version, last seen age.
- **Activity log**: 10 most recent events with timestamp, type, and detail.

### Devices Page

- **Device table**: Status, device ID, name + tags, board, version, IP, RSSI, last seen, OTA status, action buttons (OTA push, Edit).
- **Device detail modal**: Identity block (ID, name, board, tags, registered date), Status block (online badge, last seen, IP, RSSI), Firmware block (version, partition, pending OTA), Hardware block (uptime, free heap, MAC). Action buttons: Push OTA, Edit, Reboot, Clear OTA, Delete.
- **Edit device modal**: Device name, tags (comma-separated), group, notes. PATCH update to server.
- **OTA push dialog**: Firmware selector dropdown showing version, board, signed status, upload date. Push to single device or all devices.

### Firmware Page

- **Upload zone**: Drag-and-drop or click-to-browse for `.bin` / `.ota` files. Version and board auto-detected from OTA header. Manual override fields for version, board, notes.
- **Firmware table**: ID, version, board, size (KB), signed badge, deploy count, upload date, actions (Deploy, Download, Delete).

### Commission Page

- **USB Serial Provisioning** (left panel):
  - Scan USB Ports button: detects `/dev/ttyACM*` and `/dev/ttyUSB*`, sends `IDENTIFY\n` to each, shows board type or busy/no-response status.
  - Board type selector (all 6 supported boards).
  - Serial port, device name, WiFi SSID/password, server URL fields.
  - 4-step progress indicator: Flash OTA firmware, Wait for boot, Provision identity, Register with fleet.
  - Three action buttons: Start Full Commission (all 4 steps), Flash Only, Provision Only.
  - Serial output log area.
- **Generate Provisioning Package** (right panel):
  - Device name, server URL, WiFi credentials, MQTT broker fields.
  - Generates `device.json` + `factory_wifi.json` in `certs/<device_id>/` for SD card provisioning.

### Events Page

- Full activity log with datetime, event type, device ID, and detail.
- Refresh button loads up to 100 events.

### Security Page

- Planned addition to the sidebar navigation.
- Firmware attestation overview: attested vs unattested vs no-hash device counts.
- Per-device attestation status and firmware hash display.

## REST API Reference

### Device Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/api/devices` | Admin | List all devices with computed fields (`_online`, `_age`) |
| `GET` | `/api/devices/{device_id}` | Admin | Get single device with computed fields |
| `POST` | `/api/devices/{device_id}/status` | None | Device heartbeat — auto-registers unknown devices, updates status, checks attestation, returns pending OTA |
| `PATCH` | `/api/devices/{device_id}` | Admin | Update device metadata (name, tags, notes, group) |
| `DELETE` | `/api/devices/{device_id}` | Admin | Delete device |
| `POST` | `/api/devices/{device_id}/reboot` | Admin | Schedule reboot command (picked up on next heartbeat) |

#### Heartbeat Request Body (`POST /api/devices/{id}/status`)

```json
{
    "version": "1.2.0",
    "board": "touch-lcd-349",
    "partition": "ota_0",
    "ip": "192.168.1.42",
    "mac": "20:6E:F1:9A:24:E8",
    "uptime_s": 3600,
    "free_heap": 245760,
    "rssi": -52,
    "fw_hash": "<64-char SHA-256 hex of running firmware>",
    "ota_result": {
        "status": "success",
        "version": "1.2.0",
        "error": ""
    }
}
```

All fields are optional. `fw_hash` triggers firmware attestation. `ota_result` reports OTA outcome and clears pending OTA on success.

#### Heartbeat Response

```json
{
    "status": "ok",
    "ota": {
        "firmware_id": "fw-abc12345",
        "version": "1.3.0",
        "size": 1048576,
        "url": "/api/firmware/fw-abc12345/download",
        "scheduled_at": "2026-03-07T12:00:00+00:00"
    }
}
```

The `ota` field is only present when an OTA update is pending for the device.

### Firmware Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/api/firmware` | Admin | List all firmware, sorted by upload date (newest first) |
| `POST` | `/api/firmware/upload` | Admin | Upload firmware binary (multipart form: `file`, `version`, `board`, `notes`) |
| `GET` | `/api/firmware/{fw_id}/download` | None | Download firmware binary (device access) |
| `DELETE` | `/api/firmware/{fw_id}` | Admin | Delete firmware metadata + binary |

#### Upload Response

```json
{
    "id": "fw-abc12345",
    "filename": "firmware_ota",
    "version": "1.3.0",
    "board": "touch-lcd-349",
    "size": 1048512,
    "total_size": 1048576,
    "crc32": "0xDEADBEEF",
    "signed": true,
    "encrypted": false,
    "sha256": "a1b2c3...",
    "uploaded_at": "2026-03-07T12:00:00+00:00",
    "notes": "Bug fix release",
    "deploy_count": 0
}
```

Upload constraints: minimum 256 bytes, maximum 16 MB. OTA header is auto-parsed if present; version and board are extracted from the header when not provided in the form. Filenames are sanitized (non-alphanumeric characters replaced with `_`).

### OTA Push Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `POST` | `/api/ota/push/{device_id}` | Admin | Schedule OTA for a single device |
| `POST` | `/api/ota/push-all` | Admin | Schedule OTA for all registered devices |
| `POST` | `/api/ota/clear/{device_id}` | Admin | Clear pending OTA for a device |

#### Push Request Body

```json
{
    "firmware_id": "fw-abc12345"
}
```

Or use `{"latest": true}` to auto-select the newest firmware (optionally matching the device's board type).

#### Push-All Response

```json
{
    "status": "scheduled",
    "devices_updated": 5,
    "firmware_id": "fw-abc12345"
}
```

Deploy count on the firmware metadata is incremented for each device targeted.

### Commissioning Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/api/commission/ports` | Admin | Scan USB serial ports, identify connected ESP32 devices |
| `POST` | `/api/commission/flash` | Admin | Flash firmware via PlatformIO (`pio run -e <board>-ota -t upload`) |
| `POST` | `/api/commission/provision` | Admin | Provision device identity over serial USB |
| `POST` | `/api/provision/generate` | Admin | Generate provisioning package for SD card / offline use |

#### Port Scan Response

```json
[
    {
        "port": "/dev/ttyACM0",
        "device": {"board": "touch-lcd-349", "app": "ota"},
        "busy": false
    }
]
```

Port detection sends `IDENTIFY\n` at 115200 baud and parses JSON responses containing `"board"`.

#### Flash Request

```json
{
    "port": "/dev/ttyACM0",
    "board": "touch-lcd-349"
}
```

Port paths are validated against `^/dev/tty(ACM|USB)\d+$`. Board names are validated against `^[a-zA-Z0-9_\-]+$`. Flash timeout: 120 seconds. Output is truncated to the last 2000 characters.

#### Provision Request

```json
{
    "port": "/dev/ttyACM0",
    "device_name": "kitchen-display",
    "server_url": "http://fleet.local:8080",
    "wifi_ssid": "HomeNet",
    "wifi_pass": "secret",
    "mqtt_broker": "broker.local",
    "mqtt_port": 8883
}
```

Provisioning protocol: sends `PROVISION_BEGIN\n`, waits for `PROVISION_READY`, then sends identity JSON. Device responds with `"ok"` or `Provisioned`. On success, the device is registered in the fleet with `tags: ["new"]`.

#### Generate Package Response

```json
{
    "device_id": "esp32-a1b2c3d4e5f6",
    "identity": {
        "device_id": "esp32-a1b2c3d4e5f6",
        "device_name": "kitchen-display",
        "server_url": "http://fleet.local:8080",
        "mqtt_broker": "broker.local",
        "mqtt_port": 8883,
        "provisioned": true
    }
}
```

Files written to `certs/<device_id>/device.json` (and `factory_wifi.json` if WiFi credentials provided). Device is registered in fleet with `tags: ["pending"]`, `provisioned: false`.

### Events Endpoint

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/api/events` | Admin | List events, optional `limit` (default 50) and `device_id` query params |

#### Event Object

```json
{
    "id": "a1b2c3d4e5f6",
    "ts": "2026-03-07T12:00:00+00:00",
    "type": "ota_scheduled",
    "device_id": "esp32-abc123",
    "detail": "Firmware 1.3.0 (fw-abc12345)"
}
```

Event types: `device_registered`, `device_updated`, `device_deleted`, `device_provisioned`, `firmware_uploaded`, `firmware_deleted`, `ota_scheduled`, `ota_fleet_push`, `ota_success`, `ota_failure`, `commission_flash`, `provision_generated`, `reboot_scheduled`, `attestation_unknown`.

### Stats Endpoint

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/api/stats` | Admin | Aggregate fleet statistics |

#### Stats Response

```json
{
    "total_devices": 12,
    "online_devices": 8,
    "offline_devices": 4,
    "pending_ota": 2,
    "total_firmware": 5,
    "signed_firmware": 3,
    "boards": ["touch-lcd-349", "touch-amoled-241b"],
    "versions": ["1.2.0", "1.3.0"],
    "attested_devices": 7,
    "unattested_devices": 1,
    "no_attestation": 4
}
```

## Authentication

### API Key

- All `/api/` routes except device paths require authentication.
- Key provided via `X-API-Key` HTTP header or `api_key` query parameter.
- Key sources (in priority order): `--api-key` CLI argument, existing `.api_key` file in data directory, auto-generated UUID hex (written to `.api_key` with mode 0600).
- `--no-auth` flag disables authentication entirely (development only).
- Admin panel stores the key in `localStorage` and prompts on 401 responses.

### Device Paths (No Auth Required)

Devices access the following paths without an API key:

- `POST /api/devices/{id}/status` — heartbeat
- `GET /api/firmware/{id}/download` — firmware binary download

### Rate Limiting

- Per-IP tracking of failed authentication attempts.
- Sliding window: 5 minutes (`AUTH_FAIL_WINDOW = 300`).
- Lockout threshold: 10 failures (`AUTH_FAIL_MAX = 10`).
- Locked-out IPs receive HTTP 429 until the window expires.

## Device Management

### Auto-Registration

Devices that send a heartbeat (`POST /api/devices/{id}/status`) with an unknown ID are automatically registered with `tags: ["auto-registered"]`. A `device_registered` event is logged. Registration is capped at 100 devices to prevent abuse.

### Device Fields

| Field | Source | Description |
|-------|--------|-------------|
| `device_id` | Registration | Unique identifier (validated: alphanumeric + dash + underscore) |
| `device_name` | Admin edit | Human-readable name |
| `board` | Heartbeat | Board type (e.g., `touch-lcd-349`) |
| `version` | Heartbeat | Running firmware version |
| `partition` | Heartbeat | Active OTA partition |
| `ip` | Heartbeat | WiFi IP address |
| `mac` | Heartbeat | MAC address |
| `uptime_s` | Heartbeat | Uptime in seconds |
| `free_heap` | Heartbeat | Free heap memory in bytes |
| `rssi` | Heartbeat | WiFi signal strength (dBm) |
| `tags` | Admin edit | String array for categorization |
| `group` | Admin edit | Group name |
| `notes` | Admin edit | Free-text notes |
| `registered_at` | Registration | ISO 8601 timestamp |
| `last_seen` | Heartbeat | ISO 8601 timestamp of last heartbeat |
| `pending_ota` | OTA push | Pending firmware update descriptor |
| `pending_command` | Admin action | Pending device command (e.g., reboot) |
| `fw_hash` | Heartbeat | SHA-256 of running firmware |
| `fw_attested` | Server | `true` if hash matches known firmware, `false` if unknown |
| `fw_attested_id` | Server | Firmware ID that matched the hash |

### Online Status

Computed at query time. A device is considered online if its `last_seen` timestamp is less than 120 seconds old. The `_age` field provides a human-readable relative time string (e.g., `42s ago`, `3m ago`, `2h ago`, `5d ago`, `never`).

## Firmware Management

### OTA Header Parsing

Uploaded firmware files are checked for the OTA header (magic `0x4154304F`):

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | `0x4154304F` ('OT0A' LE) |
| 4 | 2 | header_version | 1 = unsigned, 2 = signed |
| 6 | 2 | flags | Bit 0: signed, Bit 1: encrypted |
| 8 | 4 | firmware_size | Payload size after header |
| 12 | 4 | firmware_crc32 | CRC32 of payload |
| 16 | 24 | version | Semver string, null-terminated |
| 40 | 16 | board | Target board ID, null-terminated |
| 56 | 4 | build_timestamp | Unix epoch (LE), reserved for build tooling |
| 60 | 4 | reserved | Zero-filled |

If the header is present (64 bytes, valid magic), version, board, signed status, and encrypted status are extracted automatically. The signature block (64 bytes) follows the header for signed firmware (header version 2, flag bit 0 set).

Files with OTA header are stored as `.ota`; raw binaries as `.bin`.

### Build Timestamp

Bytes 56-59 of the OTA header are reserved for a 32-bit LE Unix timestamp set by the packaging tool (`tools/package_firmware.py`). This enables replay detection and build ordering on both the device and server side.

### Firmware Metadata

Each firmware entry tracks:

- `id`: Unique identifier (`fw-<8 hex chars>`)
- `filename`: Sanitized original filename
- `version`, `board`: From OTA header or manual input
- `size`: Firmware payload size (after header)
- `total_size`: Total file size including header
- `crc32`: CRC32 of firmware payload
- `sha256`: SHA-256 of entire file (used for attestation matching)
- `signed`, `encrypted`: Boolean flags from OTA header
- `uploaded_at`: ISO 8601 timestamp
- `notes`: User-provided release notes
- `deploy_count`: Number of times pushed to devices

### Latest Firmware Selection

`get_latest_firmware(board)` returns the most recently uploaded firmware. When a board filter is provided, it performs fuzzy matching: exact match, substring in either direction, or falls back to the newest firmware overall. Board value `"any"` matches all devices.

## OTA Push

### Single Device

`POST /api/ota/push/{device_id}` with `{"firmware_id": "..."}` or `{"latest": true}`. Sets `pending_ota` on the device record with firmware ID, version, size, download URL, and schedule timestamp. The device picks this up on its next heartbeat response.

### Fleet-Wide

`POST /api/ota/push-all` with the same body. Iterates all registered devices and sets `pending_ota` on each. Returns the count of devices updated.

### Clear Pending

`POST /api/ota/clear/{device_id}` removes `pending_ota` from the device record. Used to cancel a scheduled update before the device picks it up.

### OTA Result Reporting

Devices report OTA outcomes in the heartbeat body via `ota_result`:
- `status`: `"success"` or `"failure"`
- `version`: Firmware version attempted
- `error`: Error message (on failure)

On `"success"`, the server clears `pending_ota` and logs an `ota_success` event. On failure, `ota_failure` is logged with the error detail.

## Commissioning

### USB Serial Detection

`GET /api/commission/ports` scans `/dev/ttyACM*` and `/dev/ttyUSB*`. For each port, it opens a serial connection at 115200 baud, sends `IDENTIFY\n`, waits 1 second, and parses the response for JSON containing `"board"`. Returns port path, detected device info, and busy status.

### Flash via PlatformIO

`POST /api/commission/flash` runs `pio run -e <board>-ota -t upload --upload-port <port>` in the project root directory. Timeout is 120 seconds. The output (stdout + stderr) is captured and truncated to the last 2000 characters.

### Serial Provisioning

`POST /api/commission/provision` performs a 2-step serial protocol:
1. Send `PROVISION_BEGIN\n`, wait for `PROVISION_READY`
2. Send provisioning JSON (device_id, device_name, server_url, mqtt_broker, mqtt_port), wait for confirmation

On success, a new device record is created with `provisioned: true` and `tags: ["new"]`.

### Provisioning Package Generation

`POST /api/provision/generate` creates a device identity without serial communication:
- Generates a unique device ID (`esp32-<12 hex chars>`)
- Writes `device.json` to `certs/<device_id>/`
- Optionally writes `factory_wifi.json` if WiFi credentials provided
- Registers device in fleet with `provisioned: false` and `tags: ["pending"]`

The generated files are intended for SD card provisioning via `hal_provision`.

## Firmware Attestation

Devices include `fw_hash` (64-character SHA-256 hex string) in their heartbeat. The server checks this hash against the `sha256` field of all known firmware:

- **Match found**: `fw_attested = true`, `fw_attested_id` set to the matching firmware ID.
- **No match**: `fw_attested = false`, `fw_attested_id = null`. An `attestation_unknown` event is logged (once per new hash, tracked via `_prev_fw_hash`).
- **No hash reported**: `fw_attested` field absent from device record.

The `/api/stats` endpoint reports aggregate counts: `attested_devices`, `unattested_devices`, `no_attestation`.

## Event / Activity Logging

Events are appended to daily JSONL files (`events/YYYY-MM-DD.jsonl`). Each event has a 12-character hex ID, UTC ISO 8601 timestamp, event type, optional device ID, and detail string. Extra fields from the `extra` dict are merged into the event object.

Listing reads files in reverse chronological order, scanning lines in reverse within each file, returning up to the requested limit.

## Security Features

### Input Validation

- **Device IDs**: Validated against `^[a-zA-Z0-9_\-]+$` (rejects path traversal, special characters).
- **Serial port paths**: Validated against `^/dev/tty(ACM|USB)\d+$` (rejects arbitrary paths).
- **Board names**: Validated against `^[a-zA-Z0-9_\-]+$` (prevents command injection in PlatformIO invocations).
- **Firmware filenames**: Sanitized with `re.sub(r'[^\w.\-]', '_', filename)` to prevent path injection in metadata.
- **Firmware size**: Minimum 256 bytes (rejects empty/trivial uploads), maximum 16 MB (matches ESP32-S3 flash capacity).
- **HTML output**: All user-supplied values escaped via `esc()` function in the SPA (prevents XSS).

### Path Traversal Protection

`FleetStore._safe_id()` validates all ID parameters before constructing file paths. Any ID containing characters outside `[a-zA-Z0-9_\-]` raises `ValueError`, preventing directory traversal attacks via `../` or null bytes.

### Rate Limiting

- Authentication failure tracking per client IP.
- 5-minute sliding window, 10-failure lockout threshold.
- Returns HTTP 429 to locked-out IPs.
- Device auto-registration capped at 100 devices.

### TLS Support

- `--ssl-cert` and `--ssl-key` CLI arguments enable HTTPS via uvicorn.
- Certificate generation helper: `server/generate_tls_cert.sh`.
- When TLS is enabled, all device-to-server communication (heartbeats, firmware downloads) is encrypted.

### API Key Storage

- Auto-generated key written to `.api_key` with file mode 0600 (owner-only read/write).
- Key displayed on server startup for initial admin access.

## Configuration

### CLI Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `--host` | `localhost` | Bind address |
| `--port` | `8080` | Bind port |
| `--data-dir` | `./fleet_data` | Data directory for all persistent state |
| `--api-key` | Auto-generated | API key for admin endpoints |
| `--no-auth` | `false` | Disable authentication (development only) |
| `--ssl-cert` | None | TLS certificate file (.pem) |
| `--ssl-key` | None | TLS private key file (.pem) |

### Data Directory Structure

```
fleet_data/
    .api_key                          # API key (mode 0600)
    devices/
        esp32-a1b2c3d4e5f6.json       # Device records
    firmware/
        fw-abc12345.json              # Firmware metadata
        fw-abc12345.ota               # Firmware binary (OTA header present)
        fw-def67890.json
        fw-def67890.bin               # Firmware binary (raw, no header)
    certs/
        esp32-a1b2c3d4e5f6/
            device.json               # Provisioning identity
            factory_wifi.json         # WiFi credentials (optional)
    events/
        2026-03-07.jsonl              # Daily event log
```

### Dependencies

```
fastapi>=0.104.0
uvicorn[standard]>=0.24.0
python-multipart>=0.0.6
jinja2>=3.1.2
pyserial>=3.5
cryptography>=41.0.0
aiofiles>=23.2.1
```

### Running

```bash
cd server && .venv/bin/python fleet_server.py
.venv/bin/python fleet_server.py --port 9000 --host 0.0.0.0
.venv/bin/python fleet_server.py --ssl-cert certs/server.pem --ssl-key certs/server-key.pem
```

## File Structure

```
server/
    fleet_server.py           # Main server application
    requirements.txt          # Python dependencies
    generate_tls_cert.sh      # TLS certificate generation helper
    provision_device.py       # Standalone provisioning script
    REQUIREMENTS.md           # This file
    templates/
        admin.html            # Admin panel SPA (HTML + CSS + JS)
    fleet_data/               # Persistent data (created at runtime)
    certs/                    # TLS certificates
```
