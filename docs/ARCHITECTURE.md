# TRITIUM-EDGE System Architecture

```
 _____ ____  ___ _____ ___ _   _ __  __       _____ ____   ____ _____
|_   _|  _ \|_ _|_   _|_ _| | | |  \/  |     | ____|  _ \ / ___| ____|
  | | | |_) || |  | |  | || | | | |\/| |_____|  _| | | | | |  _|  _|
  | | |  _ < | |  | |  | || |_| | |  | |_____| |___| |_| | |_| | |___
  |_| |_| \_\___| |_| |___|\___/|_|  |_|     |_____|____/ \____|_____|

  Software Defined IoT — Edge device management for the real world.
```

Created by Matthew Valancy / Copyright 2026 Valpatel Software LLC / AGPL-3.0

---

## 1. Project Identity

**Tritium-Edge** is a **Software Defined IoT** platform for managing
heterogeneous edge device fleets. The same management server handles ESP32
microcontrollers, STM32 sensor nodes, ARM Linux SBCs, and any future hardware
family that speaks the heartbeat protocol. Devices self-describe their
capabilities; the server adapts.

ESP32-S3 Waveshare development boards are the **first hardware family** -- six
boards spanning AMOLED QSPI, LCD QSPI, and RGB parallel display technologies.
The management server handles fleet-scale device operations: provisioning, OTA
updates, remote configuration, monitoring, and multi-tenant organization.

Peripheral drivers (sensors, codecs, displays, I/O expanders) are written once
in platform-agnostic C and shared across hardware families. A thin Platform
Abstraction Layer (PAL) isolates chip-specific APIs (I2C, GPIO, networking,
storage) so the same QMI8658 IMU driver works on ESP32 today and STM32
tomorrow. See [HARDWARE-ABSTRACTION.md](HARDWARE-ABSTRACTION.md) for the full
multi-family design.

The project follows the patterns established by **tritium-sc**: modular
`src/` layout with engine and domain packages, Pydantic settings, loguru
logging, router-per-domain FastAPI architecture, CYBERCORE neon UI aesthetic,
vanilla JS frontend, TDD with tiered test suites, and shell-script entry
points.

### Design Principles

| Principle | Description |
|-----------|-------------|
| Software Defined IoT | Same hardware, different behavior. Product profiles define what a device does. Change the profile, change the device. No reflash needed for config changes. |
| Multi-family hardware | Three-layer abstraction: PAL (chip-specific), Drivers (peripheral-specific, shared), BSP (board-specific pin maps). Write a sensor driver once, use it everywhere. |
| Compile-time hardware selection | Board and app chosen via `-DBOARD_*` / `-DAPP_*` build flags. Zero runtime dispatch overhead. |
| Capability-driven UI | Devices report capabilities in heartbeat. Server shows relevant config options. No camera settings for a temperature node. |
| HAL abstraction | Each peripheral gets a self-contained library under `lib/hal_*`. Dual-mode I2C supports both Arduino Wire and LovyanGFX backends. |
| Server modularity | One router per domain, services for business logic, store for persistence. No monoliths. |
| Multi-tenant first | Organizations, users, roles, and device scoping baked into the data model from the start. |
| Config-as-code | Pydantic settings with environment variable overrides. No magic config files. |
| CYBERCORE aesthetic | Cyan `#00f0ff`, magenta `#ff2a6d`, green `#05ffa1`, yellow `#fcee0a`. No frontend frameworks. |
| Transport agnostic | The mesh communicates through *any* available channel. WiFi, BLE, LoRa, ESP-NOW, 4G, Zigbee, sound, light — if it can carry bits, it's a transport. |
| Self-replicating | Every node carries what fits — its own firmware, its family's firmware, or the whole ecosystem. Nodes seed new nodes. The mesh grows itself. |
| AGPL-3.0 by design | The license reinforces the architecture. Code must remain open. Improvements are shared. The mesh spreads, and so does the source. |

### Self-Replication: Every Node is a Seed

What a node can **compute** and what it can **carry** are independent. A node's
replication role depends on its storage, not its processing power.

| Tier | Storage | Carries | Role |
|------|---------|---------|------|
| **Minimal** | 16-24MB flash | Own firmware + config | Leaf — receives updates, carries itself |
| **Light** | 1-4GB flash/SD | Firmware for its hardware family | Relay — passes firmware to its neighbors |
| **Vault** | 32-256GB SD | Multi-family firmware, models, configs, docs | Seed — bootstraps new nodes from any family |
| **Full mirror** | SSD/NVMe | Everything | Mirror — complete ecosystem replica, seeds anything |

A solar-powered ESP32 with a 128GB SD card can't run a vision model, but it
**carries** that model. When a Jetson joins the mesh, the ESP32 seeds it —
even over slow Bluetooth. The Jetson boots, loads the model, and starts
inference. The ESP32 made that possible without running a single GPU cycle.

**Implementation impact on tritium-edge:**

- `hal_sdcard` already supports high-capacity SDMMC. Vault functionality
  builds on this with a structured layout: `sd:/tritium/firmware/`,
  `sd:/tritium/models/`, `sd:/tritium/config/`, `sd:/tritium/src/`.
- `hal_provision` gains a `seed_from_peer()` function — receive ecosystem
  data from any available transport.
- `hal_ota` gains `serve_firmware()` — make stored firmware available to
  peers requesting it over ESP-NOW, BLE, or WiFi.
- The heartbeat reports a `storage_tier` field so the server knows each
  node's replication capability.
- Vault inventory (what firmware/models a node carries) is published to
  `tritium/{site}/mesh/{device_id}/inventory`.

The replication is opportunistic. Nodes don't push everything everywhere —
they advertise what they have, and peers pull what they need. Background
sync fills vaults incrementally when bandwidth is available.

**Blind courier storage:** Nodes with spare capacity can store encrypted
data for the mesh without being able to read it. A desktop with 2TB free
holds encrypted model weights or firmware archives. It can't decrypt them.
When another node needs the data, it pulls and decrypts locally. If the
courier needs space, it evicts the oldest chunks and the mesh re-replicates
them to other nodes with free capacity.

- Each chunk is AES-256 encrypted by the source node before distribution.
- Chunk metadata (hash, size, owner, replica count) is stored in cleartext
  so the mesh can track what's where without exposing content.
- The heartbeat reports `free_storage` so the server can identify courier
  candidates.
- Eviction is graceful: the courier announces eviction intent, the mesh
  finds alternative hosts, then the chunk is released.
- Minimum replica count (default: 2) ensures no data is lost on eviction.
- Published to `tritium/{site}/mesh/{device_id}/storage` (capacity,
  chunks held, eviction queue).

### Communications Philosophy: Every Channel is a Transport

Tritium treats communication as a **multi-modal, opportunistic mesh**. The system
does not privilege any single radio or protocol. Every available channel — whether
it's a purpose-built radio or an improvised signaling path — is a potential
transport for the feedback loop.

**Purpose-built transports** (first-class drivers):

| Transport | Range | Bandwidth | HAL | Use Case |
|-----------|-------|-----------|-----|----------|
| WiFi (802.11) | ~100m | High | hal_wifi | Primary data, OTA, streaming |
| BLE 5.0 | ~50m | Medium | hal_ble | Provisioning, short-range mesh |
| ESP-NOW | ~200m | Low-Medium | hal_espnow | Low-latency mesh, no AP needed |
| LoRa | 2-15km | Very Low | future | Long-range telemetry, alerts |
| Zigbee | ~100m | Low | future | Sensor mesh, home automation |
| 4G/LTE | Cellular | High | future | WAN backhaul, remote sites |
| Ethernet | Local | Very High | future | Fixed nodes, servers, gateways |
| Meshtastic | 2-15km | Very Low | future | Community LoRa mesh relay |
| Tailscale | Internet | High | future | Secure WAN overlay (servers, SBCs) |

**Improvised transports** (the mesh finds a way):

| Channel | Encoding | Sensor | Bandwidth | Use Case |
|---------|----------|--------|-----------|----------|
| Speaker → Microphone | FSK/DTMF audio tones | hal_audio | ~100 bps | Acoustic data link, air-gapped relay |
| LED/Display → Camera | Flashing patterns, QR codes | hal_camera | ~10-1000 bps | Visual data link, optical relay |
| IR LED → IR receiver | Modulated IR | future | ~1 kbps | Line-of-sight covert channel |
| Vibration motor → IMU | Encoded vibration patterns | hal_imu | ~1-10 bps | Contact-based data transfer |

The principle: **if a device has an output and another device has a matching
sensor, that's a communication channel.** A speaker and a microphone are a
modem. A flashing screen and a camera are a fiber-optic link without the fiber.
An IMU pressed against a vibrating motor is a telegraph.

> *"Life finds a way."* — And so will Tritium.

This is cybernetics in its purest form — the feedback loop doesn't care about
the medium. The mesh degrades gracefully across transport failures by falling
back to whatever channel is available. A node that loses WiFi can still relay
through ESP-NOW. If radio is jammed, it can encode data as audio tones or
visual patterns. The mesh finds a way.

#### Transport Abstraction Layer

All transports present a unified interface to the mesh:

```
┌─────────────────────────────────────────────────────────┐
│                    MESH ROUTING LAYER                     │
│  Destination-based routing, transport selection,          │
│  multi-path redundancy, store-and-forward                 │
├──────────┬──────────┬──────────┬──────────┬──────────────┤
│  WiFi    │  BLE     │  ESP-NOW │  LoRa    │  Improvised  │
│  MQTT    │  GATT    │  Flood   │  Mesh    │  Audio/Vis   │
│          │          │  P2P     │  tastic  │  IR/Vibrate  │
├──────────┴──────────┴──────────┴──────────┴──────────────┤
│                  TRANSPORT ABSTRACTION                     │
│  send(dest, payload, priority, constraints)               │
│  recv() → (source, payload, transport, rssi)              │
│  available_transports() → [{type, quality, bandwidth}]    │
│  on_transport_change(callback)                            │
└─────────────────────────────────────────────────────────┘
```

Each transport driver implements:
- **send(dest, payload)** — transmit to a peer or broadcast
- **recv(callback)** — receive incoming data
- **quality()** — current link quality (RSSI, loss rate, latency)
- **bandwidth()** — estimated throughput in bytes/sec
- **available()** — whether the transport is currently usable

The routing layer selects transports based on message priority, payload size,
and available channel quality. High-priority alerts use the fastest available
path. Large payloads (OTA, camera frames) prefer high-bandwidth channels.
Telemetry and heartbeats can use any channel, including low-bandwidth ones.

#### Multi-Path Mesh Topology

```
Node A ──WiFi──── Node B ──LoRa───── Node D
  │                 │                   │
  │ ESP-NOW         │ BLE               │ Audio
  │                 │                   │ (speaker→mic)
  v                 v                   v
Node C ──────────────────────────── Node E
         (visual: LED flash→camera)
```

Any node can reach any other node through multiple independent paths. The mesh
maintains a **transport topology map** — each node advertises which transports
it has and what peers it can reach through each one. Routing decisions happen
per-message, adapting in real-time to transport availability.

When a primary transport fails, the mesh **automatically falls back** to the
next available channel without application-layer awareness. The heartbeat
protocol, command delivery, and telemetry all work identically regardless of
the underlying transport.

#### MQTT Topic Convention for Transports

Transport status is published to the mesh via MQTT (or relayed through
whatever transport is available):

```
tritium/{site}/mesh/{device_id}/transports    ← Available transports + quality
tritium/{site}/mesh/{device_id}/peers         ← Reachable peers per transport
tritium/{site}/mesh/{device_id}/routing       ← Current routing table
```

This feeds into tritium-sc's situational awareness — the Command Center can
visualize the mesh topology, see which links are active, and understand the
communication health of the entire fleet.

---

## 2. System Architecture Overview

```
+===========================================================================+
|                        TRITIUM-EDGE MANAGEMENT SERVER                     |
|                           (FastAPI + Uvicorn)                             |
|                                                                           |
|  +------------------+  +------------------+  +------------------+        |
|  |   Auth Module    |  |   OTA Pipeline   |  |  Config Engine   |        |
|  |  JWT + RBAC      |  |  Upload, Sign,   |  |  Desired vs      |        |
|  |  Device Tokens   |  |  Push, Rollback  |  |  Reported State   |        |
|  +--------+---------+  +--------+---------+  +--------+---------+        |
|           |                      |                      |                 |
|  +--------+----------------------+----------------------+---------+       |
|  |                     Service Layer                              |       |
|  |  DeviceService | OTAService | ConfigService | CommandService   |       |
|  +--------+----------------------+----------------------+---------+       |
|           |                      |                      |                 |
|  +--------+----------------------+----------------------+---------+       |
|  |                    FleetStore (JSON on disk)                   |       |
|  |  data/orgs/{org_id}/devices/ | firmware/ | events/ | config/  |       |
|  +---------------------------------------------------------------+       |
+===========================================================================+
            |                      |                      |
            | REST API             | WebSocket             | Firmware
            | (heartbeat,          | (real-time             | Download
            |  config sync)        |  dashboard)            | (OTA pull)
            |                      |                      |
+-----------+----------+-----------+----------+-----------+----------+
|                      |                      |                      |
|   +==============+   |   +==============+   |   +==============+   |
|   | ESP32-S3     |   |   | ESP32-S3     |   |   | ESP32-S3     |   |
|   | Node A       |   |   | Node B       |   |   | Node C       |   |
|   |              |   |   |              |   |   |              |   |
|   | AMOLED 2.41  |   |   | LCD 3.5B-C   |   |   | LCD 3.49     |   |
|   | 450x600      |   |   | 320x480      |   |   | 172x640      |   |
|   | RM690B0 QSPI |   |   | AXS15231B    |   |   | AXS15231B    |   |
|   +==============+   |   +==============+   |   +==============+   |
|                      |                      |                      |
+----------------------+----------------------+----------------------+
                    DEVICE FLEET (ESP32-S3 Nodes)

            +----------------------------------------------+
            |              ADMIN PORTAL                     |
            |          (CYBERCORE Neon UI)                  |
            |                                              |
            |  Login -> Org Switcher -> Dashboard          |
            |  Devices | Firmware | Commission | Events    |
            |  Profiles | Users | Settings                 |
            |                                              |
            |  Vanilla JS + CYBERCORE CSS                  |
            +----------------------------------------------+
```

### Heartbeat Flow

```
ESP32 Device                          Management Server
     |                                       |
     |  POST /api/devices/{id}/status        |
     |  {                                    |
     |    identity, fw_hash,                 |
     |    reported_config,                   |
     |    module_status,                     |
     |    health_metrics                     |
     |  }                                    |
     | ------------------------------------> |
     |                                       |  - Update device record
     |                                       |  - Check config drift
     |                                       |  - Check pending commands
     |                                       |  - Check OTA queue
     |  {                                    |
     |    desired_config (if drift),         |
     |    pending_commands [],               |
     |    ota_directive (if queued)           |
     |  }                                    |
     | <------------------------------------ |
     |                                       |
     |  Apply config changes                 |
     |  Execute commands                     |
     |  Begin OTA if directed                |
     |                                       |
```

### Multi-Tenant Organization

```
+-------------------+     +-------------------+     +-------------------+
|   Org: AcmeCorp   |     |   Org: BuildCo    |     |   Org: Personal   |
|                   |     |                   |     |                   |
| Users:            |     | Users:            |     | Users:            |
|   alice (admin)   |     |   bob (admin)     |     |   matt (admin)    |
|   carol (user)    |     |   dave (user)     |     |                   |
|                   |     |                   |     |                   |
| Profiles:         |     | Profiles:         |     | Profiles:         |
|   kiosk-display   |     |   sensor-node     |     |   dev-board       |
|   status-board    |     |                   |     |                   |
|                   |     |                   |     |                   |
| Devices:          |     | Devices:          |     | Devices:          |
|   esp32-aabbcc    |     |   esp32-112233    |     |   esp32-a1b2c3    |
|   esp32-ddeeff    |     |   esp32-445566    |     |   esp32-d4e5f6    |
|                   |     |   esp32-778899    |     |                   |
|                   |     |                   |     |                   |
| Firmware:         |     | Firmware:         |     | Firmware:         |
|   v1.3.0-kiosk    |     |   v2.0.0-sensor   |     |   v1.2.0-dev      |
+-------------------+     +-------------------+     +-------------------+
```

---

## 3. Repository Structure (Target)

The repository serves two codebases: the **Python management server** and the
**C++ ESP32 firmware**. During the transition period, server code lives in
`server/`. The target layout moves server code to `src/` and firmware to
`firmware/`.

### Current Layout (Transition)

```
tritium-edge/                           # Repository root
├── pyproject.toml                      # Python package (management server)
├── platformio.ini                      # ESP32 firmware builds (6 boards x N apps)
├── Makefile                            # Unified build automation
├── src/                                # Firmware source (C++) -- moves to firmware/ later
│   └── main.cpp                        # Firmware entry point
├── server/                             # Management server (Python) -- moves to src/ later
│   ├── fleet_server.py                 # Current monolith (being decomposed)
│   ├── requirements.txt                # Python dependencies
│   ├── provision_device.py             # Standalone provisioning script
│   ├── generate_tls_cert.sh            # TLS certificate helper
│   └── templates/
│       └── admin.html                  # Admin panel SPA
├── include/                            # Firmware headers
│   ├── boards/                         # Per-board pin definitions (one .h per board)
│   │   ├── esp32_s3_touch_amoled_241b.h
│   │   ├── esp32_s3_amoled_191m.h
│   │   ├── esp32_s3_touch_amoled_18.h
│   │   ├── esp32_s3_touch_lcd_35bc.h
│   │   ├── esp32_s3_touch_lcd_43c_box.h
│   │   └── esp32_s3_touch_lcd_349.h
│   ├── display_init.h                  # Board-specific LovyanGFX display initialization
│   └── app.h                           # Base App class interface
├── lib/                                # Firmware libraries and HALs
│   ├── Panel_AXS15231B/               # Custom LovyanGFX panel driver
│   ├── hal_audio/                      # ES8311 codec + I2S
│   ├── hal_ble/                        # BLE connectivity
│   ├── hal_ble_ota/                    # BLE-based OTA
│   ├── hal_camera/                     # OV5640 DVP via esp_camera
│   ├── hal_debug/                      # Debug utilities
│   ├── hal_espnow/                     # ESP-NOW mesh networking
│   ├── hal_fs/                         # LittleFS file system
│   ├── hal_imu/                        # QMI8658 IMU
│   ├── hal_io_expander/                # TCA9554 I/O expander
│   ├── hal_mqtt/                       # MQTT client
│   ├── hal_ntp/                        # NTP time sync
│   ├── hal_ota/                        # OTA update system
│   ├── hal_power/                      # AXP2101 PMIC
│   ├── hal_provision/                  # Device provisioning
│   ├── hal_rtc/                        # PCF85063 RTC
│   ├── hal_sdcard/                     # SD card (SDMMC)
│   ├── hal_sleep/                      # Sleep modes
│   ├── hal_touch/                      # Touch input
│   ├── hal_ui/                         # LVGL integration
│   ├── hal_voice/                      # VAD + keyword spotting
│   ├── hal_watchdog/                   # Task watchdog
│   ├── hal_webserver/                  # HTTP server
│   └── hal_wifi/                       # Multi-network WiFi + NVS
├── apps/                               # Firmware applications
│   ├── starfield/                      # Default demo (particles)
│   ├── camera/                         # Camera preview (3.5B-C only)
│   ├── system/                         # Hardware dashboard
│   ├── ui_demo/                        # LVGL UI demo
│   ├── wifi_setup/                     # WiFi configuration
│   ├── effects/                        # Visual effects
│   ├── ota/                            # OTA management app
│   ├── test/                           # Hardware test app
│   └── _template/                      # App scaffold template
├── tools/                              # CLI utilities (Python)
│   └── detect_boards.py                # USB board detection
├── scripts/                            # Shell scripts
│   ├── flash.sh                        # Build + flash firmware
│   ├── monitor.sh                      # Serial monitor
│   ├── flash-monitor.sh                # Flash then monitor
│   ├── build.sh                        # Build firmware
│   ├── identify.sh                     # Detect connected boards
│   └── new-app.sh                      # Scaffold new app
├── docs/                               # Documentation
│   ├── ARCHITECTURE.md                 # This file
│   ├── GETTING_STARTED.md
│   ├── ADDING_A_BOARD.md
│   ├── ADDING_AN_APP.md
│   └── boards.md
├── references/                         # Official Waveshare demo code
├── sim/                                # Desktop simulator
├── keys/                               # Signing keys
└── data/                               # Runtime data (gitignored)
    └── orgs/                           # Per-org device data
```

### Target Layout (Post-Migration)

```
tritium-edge/
├── pyproject.toml                      # Python package (management server)
├── platformio.ini                      # ESP32 firmware builds
├── Makefile                            # Unified build automation
├── src/                                # Management server (Python)
│   ├── app/                            # FastAPI application
│   │   ├── main.py                     # Entry point, lifespan events
│   │   ├── config.py                   # Pydantic settings (env vars)
│   │   ├── models.py                   # Pydantic data models
│   │   ├── auth/                       # Authentication + authorization
│   │   │   ├── jwt.py                  # JWT encode/decode, token types
│   │   │   ├── rbac.py                 # Role-based access control
│   │   │   └── middleware.py           # Auth middleware (JWT + API key fallback)
│   │   ├── routers/                    # API endpoints (one per domain)
│   │   │   ├── devices.py              # Device CRUD, heartbeat, status
│   │   │   ├── firmware.py             # Firmware upload, download, list
│   │   │   ├── orgs.py                 # Organization management
│   │   │   ├── users.py                # User management, login
│   │   │   ├── profiles.py            # Product profile CRUD
│   │   │   ├── commands.py             # Remote command queue
│   │   │   └── ws.py                   # WebSocket real-time events
│   │   ├── services/                   # Business logic layer
│   │   │   ├── device_service.py       # Device lifecycle, heartbeat processing
│   │   │   ├── ota_service.py          # OTA scheduling, rollback, fleet deploy
│   │   │   ├── config_service.py       # Config merge, drift detection
│   │   │   └── command_service.py      # Command queue, ack tracking
│   │   └── store/                      # Persistence layer
│   │       ├── fleet_store.py          # Device + firmware file store
│   │       └── org_store.py            # Org-scoped data directories
│   └── frontend/                       # Admin portal (static files)
│       ├── admin.html                  # SPA entry point
│       ├── js/                         # Modular vanilla JS
│       │   ├── app.js                  # Router, auth state
│       │   ├── api.js                  # Fetch wrapper with JWT
│       │   ├── pages/                  # One JS module per page
│       │   └── components/             # Reusable UI components
│       └── css/                        # CYBERCORE styles
│           └── cybercore.css           # Neon theme variables + base
├── firmware/                           # ESP32 firmware source (was src/)
│   └── main.cpp                        # Firmware entry point
├── include/                            # Firmware headers
├── lib/                                # Firmware libraries + HALs
├── apps/                               # Firmware applications
├── tools/                              # CLI tools (Python)
├── scripts/                            # Shell scripts
│   ├── start.sh                        # Start management server
│   ├── setup.sh                        # Install dependencies + dev mode
│   ├── test.sh                         # Tiered test runner
│   ├── flash.sh                        # Flash firmware to device
│   └── monitor.sh                      # Serial monitor
├── tests/                              # All tests
│   ├── server/                         # Management server tests
│   │   ├── unit/                       # Pure logic tests (no I/O)
│   │   ├── integration/                # API tests with test client
│   │   └── e2e/                        # Playwright browser tests
│   └── firmware/                       # Firmware test stubs
├── docs/                               # Documentation
│   ├── ARCHITECTURE.md                 # This file
│   ├── MANAGEMENT-SYSTEM.md            # Server requirements
│   ├── DEVICE-PROTOCOL.md              # Heartbeat v2, config sync
│   ├── MULTI-TENANT.md                 # Org/user/role model
│   └── OTA-SYSTEM.md                   # OTA pipeline
└── data/                               # Runtime data (gitignored)
    └── orgs/                           # Per-org device data
        └── {org_id}/
            ├── devices/
            ├── firmware/
            ├── config/
            └── events/
```

---

## 4. Multi-Tenant Data Model

### Entity Relationships

```
Organization (Org)
  |
  +-- has many --> Users (with roles)
  |
  +-- has many --> Product Profiles
  |                   |
  |                   +-- applied to --> Devices (default config)
  |
  +-- has many --> Devices
  |                   |
  |                   +-- runs --> Firmware
  |                   +-- has --> reported_config
  |                   +-- has --> desired_config (profile + overrides)
  |
  +-- has many --> Firmware Images
  |
  +-- has many --> Events
```

### Organizations

| Field | Type | Description |
|-------|------|-------------|
| `org_id` | string | Unique identifier (slug format, e.g., `acme-corp`) |
| `name` | string | Display name |
| `created_at` | datetime | Creation timestamp (UTC ISO 8601) |
| `plan` | string | Subscription tier: `free`, `pro`, `enterprise` |
| `max_devices` | int | Device cap for the org (default: 10 for free) |
| `settings` | object | Org-level settings (default server URL, MQTT broker, etc.) |

### Users

| Field | Type | Description |
|-------|------|-------------|
| `user_id` | string | Unique identifier |
| `email` | string | Login identity (unique across system) |
| `password_hash` | string | bcrypt hash |
| `display_name` | string | Human-readable name |
| `memberships` | list | Array of `{org_id, role}` objects |
| `created_at` | datetime | Registration timestamp |
| `last_login` | datetime | Most recent login |

### Roles

| Role | Scope | Permissions |
|------|-------|-------------|
| `super_admin` | System-wide | All operations across all orgs. Create/delete orgs. |
| `org_admin` | Single org | Manage devices, firmware, users, profiles within their org. |
| `user` | Single org | View devices and status. Limited control (reboot, view logs). |

### Devices

| Field | Type | Description |
|-------|------|-------------|
| `device_id` | string | Unique identifier (e.g., `esp32-a1b2c3d4e5f6`) |
| `org_id` | string | Owning organization |
| `profile_id` | string | Product profile (optional) |
| `device_name` | string | Human-readable name |
| `board` | string | Board type (e.g., `touch-lcd-349`) |
| `version` | string | Running firmware version |
| `mac` | string | MAC address |
| `ip` | string | Last known IP |
| `rssi` | int | WiFi signal strength (dBm) |
| `uptime_s` | int | Uptime in seconds |
| `free_heap` | int | Free heap memory (bytes) |
| `fw_hash` | string | SHA-256 of running firmware |
| `fw_attested` | bool | Whether hash matches known firmware |
| `reported_config` | object | Config the device reports it is running |
| `desired_config` | object | Effective config (profile defaults + overrides) |
| `module_status` | object | Per-module health (camera, audio, IMU, etc.) |
| `tags` | list | String array for grouping/filtering |
| `last_seen` | datetime | Last heartbeat timestamp |
| `registered_at` | datetime | First registration timestamp |
| `pending_ota` | object | Pending firmware update descriptor |
| `pending_commands` | list | Queued commands awaiting execution |

### Product Profiles

| Field | Type | Description |
|-------|------|-------------|
| `profile_id` | string | Unique identifier (slug format) |
| `org_id` | string | Owning organization |
| `name` | string | Display name (e.g., "Kiosk Display") |
| `board` | string | Target board type |
| `default_app` | string | Default firmware app to build/flash |
| `default_config` | object | Baseline configuration for all devices with this profile |
| `enabled_modules` | list | HAL modules to activate (e.g., `["wifi", "display", "touch"]`) |
| `created_at` | datetime | Creation timestamp |

### Firmware

| Field | Type | Description |
|-------|------|-------------|
| `fw_id` | string | Unique identifier (`fw-<8 hex>`) |
| `org_id` | string | Owning organization |
| `version` | string | Semantic version |
| `board` | string | Target board (or `any`) |
| `size` | int | Payload size in bytes |
| `sha256` | string | SHA-256 of entire file |
| `crc32` | string | CRC32 of firmware payload |
| `signed` | bool | Whether firmware has signature |
| `encrypted` | bool | Whether firmware is encrypted |
| `uploaded_at` | datetime | Upload timestamp |
| `deploy_count` | int | Number of pushes to devices |
| `notes` | string | Release notes |

---

## 5. Authentication and Authorization

### Token Types

| Token | Lifetime | Audience | Use Case |
|-------|----------|----------|----------|
| Access token | 15 minutes | Admin portal, API clients | Short-lived, carries user identity + org + role |
| Refresh token | 7 days | Admin portal | Silent re-authentication, rotated on use |
| Device token | 365 days | ESP32 devices | Issued during provisioning, scoped to one device |
| API key | No expiry | Scripts, CI/CD | Backward-compatible fallback, stored in `.api_key` |

### JWT Payload (Access Token)

```json
{
  "sub": "user-abc123",
  "email": "alice@acme.com",
  "org_id": "acme-corp",
  "role": "org_admin",
  "iat": 1741305600,
  "exp": 1741306500,
  "type": "access"
}
```

### JWT Payload (Device Token)

```json
{
  "sub": "esp32-a1b2c3d4e5f6",
  "org_id": "acme-corp",
  "type": "device",
  "iat": 1741305600,
  "exp": 1772841600
}
```

### Auth Flow

```
1. User Login
   POST /api/auth/login {email, password}
   -> Verify bcrypt hash
   -> Return {access_token, refresh_token}

2. API Request
   GET /api/devices  (Authorization: Bearer <access_token>)
   -> Middleware decodes JWT
   -> Checks role against endpoint requirements
   -> Injects user context into request state

3. Token Refresh
   POST /api/auth/refresh {refresh_token}
   -> Verify refresh token validity
   -> Issue new access_token + rotated refresh_token

4. Device Heartbeat
   POST /api/devices/{id}/status  (Authorization: Bearer <device_token>)
   -> Middleware decodes device JWT
   -> Validates device_id matches token subject
   -> Scopes data access to token org_id

5. API Key Fallback (Migration Period)
   GET /api/devices  (X-API-Key: <key>)
   -> Middleware checks against stored API key
   -> Grants super_admin equivalent access
   -> Deprecated: log warning, encourage JWT migration
```

### Endpoint Authorization Matrix

| Endpoint | `super_admin` | `org_admin` | `user` | Device Token | API Key |
|----------|:---:|:---:|:---:|:---:|:---:|
| `GET /api/orgs` | Y | own | -- | -- | Y |
| `POST /api/orgs` | Y | -- | -- | -- | Y |
| `GET /api/devices` | Y | own org | own org | -- | Y |
| `POST /api/devices/{id}/status` | -- | -- | -- | Y (self) | -- |
| `POST /api/ota/push/{id}` | Y | own org | -- | -- | Y |
| `POST /api/firmware/upload` | Y | own org | -- | -- | Y |
| `GET /api/users` | Y | own org | -- | -- | Y |
| `POST /api/commands/{id}` | Y | own org | limited | -- | Y |

---

## 6. Heartbeat Protocol v2

### Request Payload

The heartbeat is the primary device-to-server communication channel. Devices
POST to `/api/devices/{device_id}/status` on a configurable interval (default
60 seconds).

```json
{
  "version": "1.3.0",
  "board": "touch-lcd-349",
  "partition": "ota_0",
  "ip": "192.168.1.42",
  "mac": "20:6E:F1:9A:24:E8",
  "uptime_s": 7200,
  "free_heap": 245760,
  "rssi": -48,
  "fw_hash": "a1b2c3d4...64-char-sha256",

  "reported_config": {
    "heartbeat_interval_s": 60,
    "display_brightness": 80,
    "wifi_networks": ["HomeNet"],
    "ntp_server": "pool.ntp.org",
    "sleep_schedule": null,
    "enabled_modules": ["wifi", "display", "touch", "imu"]
  },

  "module_status": {
    "wifi": {"state": "connected", "rssi": -48},
    "display": {"state": "active", "driver": "AXS15231B"},
    "touch": {"state": "active", "driver": "AXS15231B"},
    "imu": {"state": "active", "accel_range": "4g"},
    "camera": {"state": "disabled"},
    "audio": {"state": "error", "error": "ES8311 init failed"}
  },

  "ota_result": {
    "status": "success",
    "version": "1.3.0",
    "error": ""
  }
}
```

All fields are optional. Devices send what they have. The server merges
partial updates into the stored device record.

### Response Payload

```json
{
  "status": "ok",
  "server_time": "2026-03-07T12:00:00+00:00",

  "desired_config": {
    "heartbeat_interval_s": 30,
    "display_brightness": 100,
    "enabled_modules": ["wifi", "display", "touch", "imu", "camera"]
  },

  "pending_commands": [
    {
      "command_id": "cmd-abc123",
      "type": "gpio_write",
      "params": {"pin": 4, "value": 1},
      "issued_at": "2026-03-07T11:59:00+00:00"
    }
  ],

  "ota": {
    "firmware_id": "fw-def45678",
    "version": "1.4.0",
    "size": 1048576,
    "url": "/api/firmware/fw-def45678/download",
    "scheduled_at": "2026-03-07T12:00:00+00:00"
  }
}
```

### Config Drift Detection

The server computes drift by comparing `reported_config` against
`desired_config` (the effective config from profile defaults merged with
device-level overrides). If any key differs, the full `desired_config` is
included in the heartbeat response.

```
desired_config = merge(profile.default_config, device.config_overrides)

drift = {
    key: desired_config[key]
    for key in desired_config
    if reported_config.get(key) != desired_config[key]
}

if drift:
    response["desired_config"] = desired_config
```

The device applies the desired config and reports the new state on the next
heartbeat. Three consecutive heartbeats with unresolved drift trigger a
`config_drift_alert` event.

### Command Delivery

Commands are queued on the device record and delivered in the heartbeat
response. The device executes each command and reports acknowledgment:

| Command Type | Parameters | Description |
|-------------|------------|-------------|
| `reboot` | `{delay_s}` | Schedule device reboot |
| `gpio_write` | `{pin, value}` | Set digital output |
| `gpio_read` | `{pin}` | Read digital input (result in next heartbeat) |
| `pwm` | `{pin, duty, freq}` | Set PWM output |
| `sleep` | `{mode, duration_s}` | Enter light/deep sleep |
| `identify` | `{}` | Flash display or LED for physical identification |
| `diagnostics` | `{modules}` | Run self-test on specified modules |
| `config_reload` | `{}` | Force re-read of NVS config |

Commands include a `command_id` for deduplication and ack tracking. The device
reports `{"command_id": "cmd-abc123", "status": "done", "result": {...}}` in
the next heartbeat.

---

## 7. Remote Configuration

### Profile Cascade

Configuration follows a three-level cascade with later levels overriding
earlier ones:

```
Level 1: System Defaults (hardcoded in firmware)
    |
    v
Level 2: Profile Defaults (product profile default_config)
    |
    v
Level 3: Device Overrides (per-device config set by admin)
    |
    v
Effective desired_config = merge(Level 2, Level 3)
```

The firmware always has sane system defaults compiled in. The server only
sends `desired_config` when it differs from `reported_config`, minimizing
heartbeat payload size.

### Module Management

Each HAL module can be enabled or disabled remotely via the
`enabled_modules` config key. The firmware checks this list at boot and on
config update, initializing or shutting down modules accordingly.

| Module | HAL Library | Boards | Description |
|--------|-------------|--------|-------------|
| `wifi` | hal_wifi | All | Multi-network WiFi with NVS |
| `display` | display_init | All | LovyanGFX display driver |
| `touch` | hal_touch | Most | Touch input |
| `imu` | hal_imu | 3.5B-C | QMI8658 accelerometer/gyroscope |
| `power` | hal_power | 3.5B-C | AXP2101 PMIC monitoring |
| `rtc` | hal_rtc | 3.5B-C | PCF85063 real-time clock |
| `camera` | hal_camera | 3.5B-C | OV5640 DVP camera |
| `audio` | hal_audio | 3.5B-C | ES8311 codec + I2S |
| `sdcard` | hal_sdcard | 3.5B-C | SDMMC storage |
| `espnow` | hal_espnow | All | ESP-NOW mesh networking |
| `mqtt` | hal_mqtt | All | MQTT client |
| `ble` | hal_ble | All | BLE connectivity |
| `ntp` | hal_ntp | All | NTP time synchronization |
| `sleep` | hal_sleep | All | Power management |
| `watchdog` | hal_watchdog | All | Task watchdog + heap metrics |

### Configurable Settings

| Category | Keys | Description |
|----------|------|-------------|
| Network | `wifi_networks`, `mqtt_broker`, `mqtt_port`, `server_url` | Connectivity targets |
| Time | `ntp_server`, `timezone`, `heartbeat_interval_s` | Time sync and reporting |
| Display | `display_brightness`, `display_timeout_s`, `default_app` | Visual settings |
| Power | `sleep_schedule`, `sleep_mode`, `wake_sources` | Power management |
| GPIO | `gpio_config` (array of `{pin, mode, value}`) | Remote pin control |

---

## 8. Implementation Phases

### Phase 1: Modularize

Decompose `server/fleet_server.py` (current monolith) into the `src/app/`
package structure.

| Task | Source | Target |
|------|--------|--------|
| FleetStore class | `fleet_server.py:74-179` | `src/app/store/fleet_store.py` |
| Device endpoints | `fleet_server.py` `/api/devices/*` | `src/app/routers/devices.py` |
| Firmware endpoints | `fleet_server.py` `/api/firmware/*` | `src/app/routers/firmware.py` |
| OTA endpoints | `fleet_server.py` `/api/ota/*` | `src/app/routers/firmware.py` |
| Commission endpoints | `fleet_server.py` `/api/commission/*` | `src/app/routers/devices.py` |
| OTA header parser | `fleet_server.py:49-64` | `src/app/services/ota_service.py` |
| API key middleware | `fleet_server.py` | `src/app/auth/middleware.py` |
| Pydantic settings | CLI argparse | `src/app/config.py` |
| Entry point | `fleet_server.py` top-level | `src/app/main.py` |
| Admin HTML | `server/templates/admin.html` | `src/frontend/admin.html` |

**Deliverable**: Same functionality, modular codebase, Pydantic config,
loguru logging.

### Phase 2: Auth

Add JWT authentication alongside the existing API key system.

- `src/app/auth/jwt.py`: Token creation, validation, refresh rotation.
- `src/app/auth/rbac.py`: Role definitions, permission checks.
- `src/app/auth/middleware.py`: Dual-mode auth (JWT primary, API key fallback).
- `src/app/routers/users.py`: Login, register, refresh, change password.
- Password hashing with bcrypt.
- Admin panel login page with token storage.

**Deliverable**: JWT login flow, API key still works, no breaking changes.

### Phase 3: Multi-Tenant

Add organization scoping to all data and endpoints.

- `src/app/routers/orgs.py`: Org CRUD, member management.
- `src/app/store/org_store.py`: Org-scoped directory layout under `data/orgs/`.
- Data migration script: flat `fleet_data/` to `data/orgs/default/`.
- All device, firmware, and event queries scoped to the requesting user's org.
- Org switcher in admin portal header.

**Deliverable**: Full multi-tenant isolation, default org for migration.

### Phase 4: Profiles and Config

Add product profiles and configuration management.

- `src/app/routers/profiles.py`: Profile CRUD.
- `src/app/services/config_service.py`: Config merge, drift detection.
- Heartbeat v2 request/response with `reported_config` and `desired_config`.
- Firmware changes: add `reported_config` to heartbeat payload, apply
  `desired_config` from response.
- Profile management page in admin portal.

**Deliverable**: Remote configuration with drift detection.

### Phase 5: Commands

Add remote command queue and GPIO control.

- `src/app/routers/commands.py`: Issue commands, view queue, ack status.
- `src/app/services/command_service.py`: Queue management, timeout, retry.
- Firmware changes: parse `pending_commands` from heartbeat response,
  execute, report results.
- Command panel in admin device detail view.

**Deliverable**: Remote reboot, GPIO control, diagnostics.

### Phase 6: Polish

Production readiness, testing, and UI refinement.

- Full test suite: unit, integration, e2e (Playwright).
- CYBERCORE UI redesign of admin portal.
- WebSocket real-time dashboard updates.
- Documentation: all docs/ specs written and reviewed.
- `scripts/start.sh`, `scripts/setup.sh`, `scripts/test.sh` entry points.
- `pyproject.toml` packaging.

**Deliverable**: Production-ready management platform.

---

## 9. Migration Strategy

### Server Code

```
BEFORE:  server/fleet_server.py (1 file, ~800 lines)
AFTER:   src/app/ (modular package, ~15 files)

Transition: server/ directory preserved during migration.
            New src/app/ imports from store/ and services/.
            fleet_server.py kept as compatibility shim until cutover.
```

### Data Directory

```
BEFORE:  fleet_data/
           devices/*.json
           firmware/*.json + .ota
           certs/<device_id>/
           events/YYYY-MM-DD.jsonl
           .api_key

AFTER:   data/
           .api_key
           users/*.json
           orgs/
             {org_id}/
               org.json
               devices/*.json
               firmware/*.json + .ota
               config/profiles/*.json
               events/YYYY-MM-DD.jsonl
               certs/<device_id>/
```

A one-time migration script moves existing data into `data/orgs/default/`,
creating a `default` org with all current devices and firmware. Existing
API keys continue to work with super_admin scope.

### Auth Migration

```
Phase 1: API key only (current behavior)
Phase 2: JWT added, API key still accepted (dual-mode)
Phase 3: API key deprecated (log warnings)
Phase 4: API key removed (future, not in initial roadmap)
```

### Firmware Changes

The firmware heartbeat payload requires minimal changes:

| Field | Status | Description |
|-------|--------|-------------|
| `reported_config` | New | Device reports its active configuration |
| `module_status` | New | Per-module health status |
| All existing fields | Unchanged | version, board, ip, mac, etc. |

The heartbeat response gains new optional fields:

| Field | Status | Description |
|-------|--------|-------------|
| `desired_config` | New | Only present when drift detected |
| `pending_commands` | New | Only present when commands queued |
| `ota` | Unchanged | Same OTA directive format |

Devices that do not send `reported_config` or process `desired_config`
continue to work -- the server simply skips config sync for them.

---

## 10. Code Conventions

### Python (Management Server)

| Convention | Example |
|------------|---------|
| Type hints on all functions | `def get_device(self, device_id: str) -> Optional[Device]:` |
| Async/await for I/O | `async def heartbeat(request: Request):` |
| Pydantic models for validation | `class DeviceHeartbeat(BaseModel):` |
| Loguru for logging | `from loguru import logger` |
| Copyright header on all files | See below |
| No emojis in code or logs | Plain text only |
| Snake_case for files and functions | `device_service.py`, `get_latest_firmware()` |
| PascalCase for classes | `FleetStore`, `OTAService` |
| One router per domain | `routers/devices.py`, `routers/firmware.py` |
| Tests mirror src/ structure | `tests/server/unit/test_device_service.py` |

### C++ (Firmware)

| Convention | Example |
|------------|---------|
| C++17, Arduino framework | `#include <Arduino.h>` |
| 4-space indentation | No tabs |
| 100-column line width | Enforced by clang-format |
| SCREAMING_SNAKE_CASE for pins | `#define LCD_BL 48` |
| PascalCase + App suffix for apps | `class CameraApp : public App` |
| snake_case for source files | `camera_app.cpp`, `hal_imu.cpp` |
| Force-included board headers | `-include include/boards/<board>.h` |
| HAS_* feature flags | `#ifdef HAS_CAMERA` |

### File Headers

All source files carry the project copyright:

```python
# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# SPDX-License-Identifier: AGPL-3.0
```

```cpp
// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
```

### Testing

| Tier | Location | Runner | Scope |
|------|----------|--------|-------|
| Unit | `tests/server/unit/` | pytest | Pure logic, no I/O, fast |
| Integration | `tests/server/integration/` | pytest + httpx | FastAPI TestClient, real store |
| E2E | `tests/server/e2e/` | Playwright | Full browser tests against running server |
| Firmware | `tests/firmware/` | PlatformIO test | Hardware-in-the-loop (future) |

Test commands:

```bash
./scripts/test.sh              # Run all tiers
./scripts/test.sh unit         # Unit tests only
./scripts/test.sh integration  # Integration tests only
./scripts/test.sh e2e          # Playwright browser tests
```

---

## Supported Hardware

| Board | Environment | Resolution | Display | Peripherals |
|-------|-------------|-----------|---------|-------------|
| ESP32-S3-Touch-AMOLED-2.41-B | `touch-amoled-241b` | 450x600 | RM690B0 QSPI | Touch |
| ESP32-S3-AMOLED-1.91-M | `amoled-191m` | 240x536 | RM67162 QSPI | -- |
| ESP32-S3-Touch-AMOLED-1.8 | `touch-amoled-18` | 368x448 | SH8601Z QSPI | Touch |
| ESP32-S3-Touch-LCD-3.5B-C | `touch-lcd-35bc` | 320x480 | AXS15231B QSPI | Touch, Camera, Audio, IMU, PMIC, RTC, SD |
| ESP32-S3-Touch-LCD-4.3C-BOX | `touch-lcd-43c-box` | 800x480 | ST7262 RGB | Touch |
| ESP32-S3-Touch-LCD-3.49 | `touch-lcd-349` | 172x640 | AXS15231B QSPI | Touch |

All boards: ESP32-S3, 16MB flash, 8MB PSRAM, Arduino framework, LovyanGFX
display abstraction.

---

## Key References

| Resource | Location |
|----------|----------|
| Current fleet server | `server/fleet_server.py` |
| Server requirements | `server/REQUIREMENTS.md` |
| Admin panel SPA | `server/templates/admin.html` |
| Firmware entry point | `src/main.cpp` |
| Board pin definitions | `include/boards/*.h` |
| Display initialization | `include/display_init.h` |
| HAL libraries | `lib/hal_*/` |
| App implementations | `apps/*/` |
| Official Waveshare demos | `references/` |
| Build automation | `Makefile`, `scripts/` |
| PlatformIO config | `platformio.ini` |
