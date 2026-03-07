# Tritium-Edge Management Server

**Software Defined IoT** -- Requirements specification for the Tritium-Edge Management
Server. A FastAPI platform for managing heterogeneous edge device fleets (ESP32, STM32,
ARM Linux SBCs, and future families) with multi-tenant support, OTA delivery, remote
configuration, and real-time monitoring.

---

## 1. Overview

The Tritium-Edge Management Server is a self-hosted FastAPI application that provides
centralized fleet management for heterogeneous edge devices. ESP32-S3 is the first
hardware family, but the server manages any device that speaks the heartbeat protocol --
STM32 sensor nodes, nRF52 BLE beacons, ARM Linux gateways, and future families. Devices
self-describe capabilities; the server adapts. Designed to run on-premise with zero cloud
dependencies.

### Core Capabilities

| Capability | Description |
|---|---|
| Multi-tenant organizations | Isolated orgs with role-based access control |
| Software-defined configuration | Product profiles push config to devices at scale |
| OTA firmware delivery | 7 pathways covering every deployment scenario |
| Real-time monitoring | Heartbeat protocol with drift detection and alerting |
| Admin portal | CYBERCORE-themed web dashboard for fleet operations |
| Device provisioning | Self-registration and certificate-based identity |

### OTA Delivery Pathways

```
  Pathway          Transport        Direction    Use Case
  ─────────────────────────────────────────────────────────────────
  WiFi Pull        HTTPS GET        Device→Srv   Standard fleet OTA
  WiFi Push        WebSocket cmd    Srv→Device   Urgent patches
  SD Card          FAT32 file       Offline      Air-gapped sites
  Serial           UART/USB-CDC     Wired        Dev and factory
  BLE              GATT service     Wireless     Proximal single-device
  ESP-NOW Mesh     Peer relay       Wireless     No-infrastructure sites
  USB MSC          Mass storage     Wired        Field service via laptop
```

### System Context

```
  ┌──────────────┐         HTTPS/WS          ┌──────────────────┐
  │  Admin Portal │◄────────────────────────►│  Management Srv  │
  │  (Browser)    │                           │  (FastAPI)       │
  └──────────────┘                           └────────┬─────────┘
                                                      │
                              Heartbeat (HTTPS)       │  File Store
                              ┌───────────────────────┤  ┌─────────┐
                              │                       └─►│ data/   │
                    ┌─────────┴──────────┐               └─────────┘
                    │  ESP32-S3 Devices   │
                    │  (Fleet)            │
                    └────────────────────┘
```

---

## 2. API Endpoints

All endpoints return JSON. Org-scoped endpoints require a valid JWT with membership in
the target organization. Device-facing endpoints use device tokens.

### 2.1 Authentication

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/auth/login` | Authenticate with email + password. Returns JWT access token (15 min) and refresh token (7 day). |
| POST | `/api/auth/refresh` | Exchange a valid refresh token for a new access token. |
| POST | `/api/auth/logout` | Invalidate the refresh token. |
| GET | `/api/auth/me` | Return the authenticated user's profile and org memberships. |

### 2.2 Organizations

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/orgs` | List organizations. `super_admin` sees all; others see only their orgs. |
| POST | `/api/orgs` | Create a new organization. `super_admin` only. |
| GET | `/api/orgs/{org_id}` | Get organization details and settings. |
| PUT | `/api/orgs/{org_id}` | Update organization name or settings. |
| DELETE | `/api/orgs/{org_id}` | Delete organization and all associated data. `super_admin` only. |

### 2.3 Users

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/orgs/{org_id}/users` | List users in the organization. |
| POST | `/api/orgs/{org_id}/users` | Invite a user to the organization with a specified role. |
| PUT | `/api/orgs/{org_id}/users/{user_id}` | Update a user's role within the organization. |
| DELETE | `/api/orgs/{org_id}/users/{user_id}` | Remove a user from the organization. |

### 2.4 Devices

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/orgs/{org_id}/devices` | List devices. Filterable by `profile`, `status`, `version`, `board`, `tag`. Supports pagination. |
| GET | `/api/orgs/{org_id}/devices/{device_id}` | Full device detail: config, health, recent events. |
| PUT | `/api/orgs/{org_id}/devices/{device_id}` | Update device metadata (name, tags) or assign a profile. |
| DELETE | `/api/orgs/{org_id}/devices/{device_id}` | Decommission device. Revokes device token and removes from org. |
| POST | `/api/orgs/{org_id}/devices/{device_id}/command` | Send a command to the device. Queued for next heartbeat or pushed via WebSocket. |
| GET | `/api/orgs/{org_id}/devices/{device_id}/events` | Paginated device event log. Filterable by type and date range. |
| GET | `/api/orgs/{org_id}/devices/{device_id}/config` | Return effective config (profile defaults merged with device overrides). |
| PUT | `/api/orgs/{org_id}/devices/{device_id}/config` | Set device-level config overrides. Merged on top of profile defaults. |

### 2.5 Product Profiles

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/orgs/{org_id}/profiles` | List all product profiles in the organization. |
| POST | `/api/orgs/{org_id}/profiles` | Create a new product profile with default config. |
| GET | `/api/orgs/{org_id}/profiles/{profile_id}` | Get profile detail including full default config. |
| PUT | `/api/orgs/{org_id}/profiles/{profile_id}` | Update profile config or metadata. Propagates to assigned devices on next heartbeat. |
| DELETE | `/api/orgs/{org_id}/profiles/{profile_id}` | Delete profile. Devices assigned to it retain their last effective config. |

### 2.6 Firmware

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/orgs/{org_id}/firmware` | List firmware images. Filterable by `board` and `version`. |
| POST | `/api/orgs/{org_id}/firmware` | Upload a firmware image (`.bin` or `.ota`). Server computes SHA-256 and validates signature if required. |
| GET | `/api/orgs/{org_id}/firmware/{fw_id}` | Get firmware metadata (version, board, size, hash, signature status). |
| DELETE | `/api/orgs/{org_id}/firmware/{fw_id}` | Delete firmware image and associated file. |
| GET | `/api/orgs/{org_id}/firmware/{fw_id}/download` | Download the `.ota` file. Used by admin tools and SD card preparation. |
| POST | `/api/orgs/{org_id}/firmware/{fw_id}/rollout` | Start a fleet rollout. Accepts target filter (all devices, by profile, by tag, specific device list). |

### 2.7 Fleet Operations

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/orgs/{org_id}/fleet/status` | Fleet-wide health summary: device counts by status, version distribution, error rates. |
| POST | `/api/orgs/{org_id}/fleet/command` | Broadcast a command to all devices or a filtered subset. |
| GET | `/api/orgs/{org_id}/fleet/rollouts` | List active and recent rollouts with progress percentages. |

### 2.8 Device-Facing Endpoints

These endpoints are not org-scoped in the URL. The device token's claims determine org
membership and device identity.

| Method | Path | Description |
|--------|------|-------------|
| POST | `/api/device/heartbeat` | Device reports health, config, and firmware version. Server responds with pending commands, desired config, and available firmware. |
| GET | `/api/device/firmware/{version}` | Download firmware binary for self-update. Validates device token and board compatibility. |
| POST | `/api/device/provision` | Device self-registration. Accepts MAC address, board type, and optional provisioning secret. Returns device token. |

### 2.9 WebSocket

| Path | Description |
|------|-------------|
| `WS /ws/live` | Authenticated WebSocket for real-time events. Streams device heartbeats, OTA progress, status changes, and command acknowledgments. Supports subscription filters by org, device, and event type. |

---

## 3. Data Models

### 3.1 Organization

```json
{
  "id": "uuid",
  "name": "string",
  "slug": "string (unique, URL-safe)",
  "created_at": "datetime",
  "settings": {
    "require_signed_firmware": "bool",
    "require_encrypted_firmware": "bool",
    "auto_rollback": "bool",
    "heartbeat_interval_s": "int (default: 60)"
  }
}
```

### 3.2 User

```json
{
  "id": "uuid",
  "email": "string (unique)",
  "name": "string",
  "password_hash": "string (bcrypt)",
  "memberships": [
    {
      "org_id": "string",
      "role": "super_admin | org_admin | user",
      "joined_at": "datetime"
    }
  ],
  "created_at": "datetime"
}
```

### 3.3 Device

```json
{
  "id": "string (MAC address, e.g. 20:6E:F1:9A:12:00)",
  "org_id": "string",
  "name": "string",
  "board": "string (e.g. touch-lcd-35bc)",
  "profile_id": "string | null",
  "firmware_version": "string (semver)",
  "firmware_hash": "string (sha256)",
  "ip_address": "string",
  "last_heartbeat": "datetime",
  "status": "online | offline | updating | error",
  "reported_config": {
    "modules": {},
    "settings": {},
    "gpio": {}
  },
  "config_overrides": {
    "modules": {},
    "settings": {},
    "gpio": {}
  },
  "health": {
    "uptime_s": "int",
    "free_heap": "int",
    "wifi_rssi": "int",
    "temperature": "float",
    "boot_count": "int"
  },
  "provisioned_at": "datetime",
  "tags": ["string"]
}
```

### 3.4 Product Profile

```json
{
  "id": "uuid",
  "org_id": "string",
  "name": "string",
  "description": "string",
  "board": "string",
  "target_version": "string | null",
  "default_config": {
    "modules": {
      "camera":  { "enabled": "bool", "resolution": "string" },
      "audio":   { "enabled": "bool", "sample_rate": "int" },
      "imu":     { "enabled": "bool", "poll_rate_hz": "int" },
      "display": { "enabled": "bool", "brightness": "int" },
      "mesh":    { "enabled": "bool" }
    },
    "settings": {
      "wifi_networks": [{ "ssid": "string", "priority": "int" }],
      "ntp_server": "string",
      "heartbeat_interval_s": "int",
      "sleep_schedule": { "start": "string (HH:MM)", "end": "string (HH:MM)" },
      "display_brightness": "int (0-255)"
    },
    "gpio": {
      "<pin_number>": {
        "mode": "input | output | pwm",
        "value": "int",
        "pull": "up | down | none"
      }
    }
  },
  "auto_update": "bool",
  "created_at": "datetime"
}
```

### 3.5 Firmware

```json
{
  "id": "uuid",
  "org_id": "string",
  "version": "string (semver)",
  "board": "string",
  "filename": "string",
  "size": "int (bytes)",
  "sha256": "string",
  "is_signed": "bool",
  "is_encrypted": "bool",
  "build_timestamp": "datetime",
  "uploaded_at": "datetime",
  "uploaded_by": "string (user_id)",
  "notes": "string"
}
```

### 3.6 Command

```json
{
  "id": "uuid",
  "device_id": "string",
  "type": "reboot | gpio_set | gpio_read | config_update | ota_url | ota_rollback | identify | custom",
  "payload": "object",
  "status": "pending | delivered | acked | failed | expired",
  "created_at": "datetime",
  "delivered_at": "datetime | null",
  "acked_at": "datetime | null",
  "expires_at": "datetime",
  "created_by": "string (user_id)"
}
```

---

## 4. Authentication and Authorization

### 4.1 JWT Token Flow (Admin Users)

1. Client POSTs `email` + `password` to `/api/auth/login`.
2. Server validates the bcrypt password hash.
3. On success, server returns `access_token` (15 min TTL) and `refresh_token` (7 day TTL).
4. Client includes `Authorization: Bearer <access_token>` on all subsequent requests.
5. When the access token expires, client POSTs the refresh token to `/api/auth/refresh` to obtain a new access token.
6. `POST /api/auth/logout` invalidates the refresh token server-side.

### 4.2 Device Token Flow

1. Device sends a provisioning request (serial, heartbeat, or `/api/device/provision`).
2. Server validates the provisioning secret or admin approval.
3. Server issues a `device_token` -- a JWT with 365-day expiry and claims: `{ device_id, org_id, board }`.
4. Device stores the token in NVS and includes it in all heartbeat requests.
5. Server validates the token signature and org membership on every request.

### 4.3 Role-Based Access Control

| Action | super_admin | org_admin | user |
|---|:-:|:-:|:-:|
| List all organizations | Yes | -- | -- |
| Create / delete organizations | Yes | -- | -- |
| Manage org users | Yes | Yes | -- |
| Upload firmware | Yes | Yes | -- |
| Start rollouts | Yes | Yes | -- |
| Send device commands | Yes | Yes | View only |
| View devices | Yes | Yes | Yes |
| View device config | Yes | Yes | Yes |
| Modify device config | Yes | Yes | -- |
| Create / edit profiles | Yes | Yes | -- |
| Delete profiles | Yes | Yes | -- |

All org-scoped endpoints enforce that the authenticated user has a membership in the
target organization with a sufficient role. `super_admin` bypasses org membership checks.

---

## 5. Config Cascade

Device configuration is computed by merging the product profile defaults with
device-specific overrides. This allows fleet-wide configuration via profiles while
supporting per-device customization.

### Merge Rule

```
effective_config = deep_merge(profile.default_config, device.config_overrides)
```

`deep_merge` is a recursive dictionary merge where device overrides take precedence at
the leaf level. A device override value of `null` explicitly removes the corresponding
profile default.

### Drift Detection

On each heartbeat, the server compares the device's `reported_config` against the
computed `effective_config`. If they differ:

1. Server includes `desired_config` in the heartbeat response.
2. Device applies the configuration delta.
3. On the next heartbeat, device reports the updated config.
4. If drift persists for 3 consecutive heartbeats, server logs a `config_drift` event.

```
  Profile defaults         Device overrides        Effective config
  ┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐
  │ display:          │    │ display:          │    │ display:          │
  │   brightness: 128 │ +  │   brightness: 255 │ =  │   brightness: 255 │
  │ camera:           │    │                   │    │ camera:           │
  │   enabled: true   │    │                   │    │   enabled: true   │
  │   resolution: VGA │    │                   │    │   resolution: VGA │
  └──────────────────┘    └──────────────────┘    └──────────────────┘
```

---

## 6. Persistence

### 6.1 File-Based Storage (Phase 1)

The initial implementation uses JSON files on disk, organized by organization. This
avoids external database dependencies and keeps the system easy to back up and inspect.

```
data/
├── orgs/
│   └── {org_id}/
│       ├── org.json                  # Organization metadata and settings
│       ├── users.json                # User list for this org
│       ├── devices/
│       │   └── {device_id}.json      # Device state and config
│       ├── profiles/
│       │   └── {profile_id}.json     # Product profile definition
│       ├── firmware/
│       │   ├── index.json            # Firmware metadata index
│       │   └── {fw_id}.ota           # Firmware binary files
│       ├── commands/
│       │   └── {device_id}.json      # Pending command queue per device
│       └── events/
│           └── {date}.jsonl          # Daily event log (append-only)
├── users.json                        # Global user index (email -> user_id)
├── auth/
│   └── refresh_tokens.json           # Active refresh tokens
└── server.json                       # Server configuration
```

File operations use atomic writes (write to temp file, then rename) to prevent
corruption on unexpected shutdown.

### 6.2 Future: SQLite Migration

When query complexity warrants it, migrate to SQLAlchemy + aiosqlite. The `store/`
abstraction layer isolates persistence from route handlers, making migration transparent.

---

## 7. Heartbeat Protocol

The heartbeat is the primary communication channel between devices and the server. It
serves as status report, config sync, command delivery, and OTA trigger.

### Request (Device to Server)

```json
{
  "device_id": "20:6E:F1:9A:12:00",
  "firmware_version": "1.2.0",
  "firmware_hash": "sha256:abc123...",
  "board": "touch-lcd-35bc",
  "health": {
    "uptime_s": 86400,
    "free_heap": 245760,
    "wifi_rssi": -42,
    "temperature": 38.5,
    "boot_count": 12
  },
  "reported_config": {
    "modules": { "camera": { "enabled": true } },
    "settings": { "heartbeat_interval_s": 60 }
  }
}
```

### Response (Server to Device)

```json
{
  "status": "ok",
  "commands": [
    { "id": "cmd-uuid", "type": "config_update", "payload": { ... } }
  ],
  "desired_config": { ... },
  "firmware_update": {
    "version": "1.3.0",
    "url": "/api/device/firmware/1.3.0",
    "sha256": "def456...",
    "size": 1572864
  },
  "next_heartbeat_s": 60
}
```

### Status Determination

| Condition | Device Status |
|---|---|
| Heartbeat received within `2 * heartbeat_interval_s` | `online` |
| No heartbeat for `2 * heartbeat_interval_s` | `offline` |
| OTA in progress | `updating` |
| Device reports error or repeated failed commands | `error` |

---

## 8. Admin Portal

The admin portal is a single-page application served by the FastAPI server. It uses the
CYBERCORE design language: dark backgrounds, neon accent colors, monospace type for
data, sharp borders.

### Pages

| Page | Purpose |
|---|---|
| **Login** | Email + password form. JWT stored in localStorage. |
| **Dashboard** | Fleet health gauges, device count by status, recent events timeline, quick action buttons. |
| **Devices** | Filterable, sortable table with status badges. Supports bulk select for commands and profile assignment. |
| **Device Detail** | Tabbed view: Config editor (JSON with diff preview), Command panel, Event log, OTA status and history. |
| **Firmware** | Upload form with drag-and-drop. Version list with board filter. Rollout controls with target selection. |
| **Profiles** | Create and edit product profiles. Config editor with module toggles and JSON override. Device count per profile. |
| **Organizations** | Org list for `super_admin`. User management table with role assignment. Org settings editor. |
| **Settings** | Server configuration: default heartbeat interval, firmware storage path, TLS settings. |
| **Security** | Firmware signing key management. Device attestation status. Certificate upload and revocation. |

### Design Constraints

- All pages are keyboard navigable.
- Responsive layout: functional at 1024px minimum width, optimized for 1440px+.
- WebSocket connection for live updates (device status changes, OTA progress).
- No external CDN dependencies. All assets served locally.

---

## 9. Testing Strategy

| Tier | Scope | Tool | Target |
|------|-------|------|--------|
| 1 | Syntax and imports | `py_compile` | All `.py` files |
| 2 | Unit tests | `pytest` | `services/`, `store/`, `auth/` |
| 3 | API integration | `pytest` + `httpx` | `routers/` (all endpoints) |
| 4 | Admin portal E2E | Playwright | All 9 pages, auth flow, CRUD ops |
| 5 | Device protocol | `pytest` + mock serial | Heartbeat, provisioning, command delivery |
| 6 | OTA pipeline | `pytest` + test firmware | Package, sign, verify, rollback |

### Test Data

A `fixtures/` directory provides deterministic test data: sample orgs, users, devices,
profiles, and firmware binaries. Integration tests use a temporary `data/` directory
that is cleaned between runs.

### CI Pipeline

```
  py_compile (all files)
       │
       ▼
  pytest unit tests
       │
       ▼
  pytest integration tests
       │
       ▼
  Playwright E2E (headless)
```

All tiers must pass before merge. Coverage target: 80% line coverage for `services/`
and `store/`.

---

## 10. Non-Functional Requirements

| Requirement | Target |
|---|---|
| Device capacity per org | 1,000+ |
| Heartbeat processing latency | < 50 ms per device |
| Admin portal initial load | < 2 seconds |
| Firmware verification | SHA-256 hash check on every transfer |
| Firmware signing | Ed25519 signatures (optional per org setting) |
| Network dependencies | None. Fully on-premise operation. |
| External telemetry | None. No data leaves the server. |
| Data backup | File-based storage supports `rsync` and filesystem snapshots. |
| TLS | Optional but recommended. Server supports both HTTP and HTTPS. |
| Concurrent admin sessions | 50+ simultaneous users |
| Rollout throughput | 100 devices updated concurrently per rollout |

---

## 11. Project Structure

```
server/
├── main.py                    # FastAPI app, CORS, lifespan
├── config.py                  # Server configuration (env vars, defaults)
├── auth/
│   ├── jwt.py                 # Token creation, validation, refresh
│   └── rbac.py                # Role checks, permission decorators
├── routers/
│   ├── auth.py                # /api/auth/*
│   ├── orgs.py                # /api/orgs/*
│   ├── users.py               # /api/orgs/{org_id}/users/*
│   ├── devices.py             # /api/orgs/{org_id}/devices/*
│   ├── profiles.py            # /api/orgs/{org_id}/profiles/*
│   ├── firmware.py            # /api/orgs/{org_id}/firmware/*
│   ├── fleet.py               # /api/orgs/{org_id}/fleet/*
│   ├── device_api.py          # /api/device/* (device-facing)
│   └── websocket.py           # /ws/live
├── services/
│   ├── device_service.py      # Heartbeat processing, status management
│   ├── config_service.py      # Config cascade, drift detection
│   ├── firmware_service.py    # Upload, signing, verification, rollout
│   ├── command_service.py     # Command queue, delivery, expiration
│   └── provision_service.py   # Device registration, token issuance
├── store/
│   ├── base.py                # Abstract store interface
│   ├── file_store.py          # JSON file backend
│   └── sqlite_store.py        # Future SQLite backend
├── models/
│   ├── org.py                 # Organization schema
│   ├── user.py                # User schema
│   ├── device.py              # Device schema
│   ├── profile.py             # Product profile schema
│   ├── firmware.py            # Firmware schema
│   └── command.py             # Command schema
├── portal/
│   ├── index.html             # SPA entry point
│   ├── static/                # CSS, JS, fonts
│   └── pages/                 # Page components
├── tests/
│   ├── unit/                  # Tier 2 tests
│   ├── integration/           # Tier 3 tests
│   ├── e2e/                   # Tier 4 Playwright tests
│   └── fixtures/              # Test data
└── data/                      # Runtime data (gitignored)
```

---

## 12. Dependencies

| Package | Purpose |
|---|---|
| `fastapi` | HTTP framework |
| `uvicorn` | ASGI server |
| `pydantic` | Data validation and serialization |
| `python-jose[cryptography]` | JWT token handling |
| `passlib[bcrypt]` | Password hashing |
| `python-multipart` | File upload support |
| `aiofiles` | Async file I/O |
| `websockets` | WebSocket support |
| `httpx` | Test client |
| `pytest` | Test framework |
| `playwright` | E2E browser testing |

All dependencies are pinned in `requirements.txt`. No cloud SDKs. No external service
clients.
