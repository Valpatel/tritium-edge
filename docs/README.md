# Tritium-Edge Documentation Index

**Where you are:** `tritium-edge/docs/` — documentation for the ESP32-S3 firmware (Tritium-OS) and fleet management server.

**Parent:** [../CLAUDE.md](../CLAUDE.md) | [../../CLAUDE.md](../../CLAUDE.md) (tritium root)

## Navigation

```
tritium/
└── tritium-edge/             ← YOU ARE HERE
    ├── CLAUDE.md             # Dev conventions, pitfalls, build commands
    ├── README.md             # Project overview, quick start
    ├── docs/                 # ← This index
    ├── lib/README.md         # HAL library reference with dependency graph
    ├── apps/README.md        # App catalog and creation guide
    ├── include/README.md     # Header organization
    ├── src/README.md         # Firmware entry point docs
    ├── scripts/README.md     # Build script reference
    ├── server/               # Fleet management server
    ├── sim/README.md         # Desktop simulator
    └── tools/README.md       # Utility tools
```

## Documents by Category

### Getting Started

| Document | Description |
|----------|-------------|
| [GETTING_STARTED.md](GETTING_STARTED.md) | Environment setup, first build, first flash, troubleshooting |
| [boards.md](boards.md) | Detailed specs, pin assignments, and wiki links for all 6 boards |
| [ADDING_A_BOARD.md](ADDING_A_BOARD.md) | How to add support for a new ESP32-S3 display board |
| [ADDING_AN_APP.md](ADDING_AN_APP.md) | Step-by-step app creation guide with app presets |

### Architecture & Design

| Document | Description |
|----------|-------------|
| [TRITIUM-OS-VISION.md](TRITIUM-OS-VISION.md) | **Master vision** — OS phases 1-6, design language, branding, implementation status |
| [ARCHITECTURE.md](ARCHITECTURE.md) | System design: principles, communications, self-replication, transports |
| [HARDWARE-ABSTRACTION.md](HARDWARE-ABSTRACTION.md) | PAL layer design, multi-family strategy |
| [DEVICE-PROTOCOL.md](DEVICE-PROTOCOL.md) | Heartbeat v2, config sync, commands |

### Integration & Protocols

| Document | Description |
|----------|-------------|
| [INTEGRATION.md](INTEGRATION.md) | Connecting to tritium-sc and tritium-lib |
| [MESHTASTIC_INTEGRATION.md](MESHTASTIC_INTEGRATION.md) | LoRa radio integration for extended range |
| [ACOUSTIC_MODEM.md](ACOUSTIC_MODEM.md) | Audio modem design for underwater/acoustic comms |
| [GIS_WEBSERVER_INTEGRATION.md](GIS_WEBSERVER_INTEGRATION.md) | Map tiles and GIS features on-device |
| [COMMISSIONING.md](COMMISSIONING.md) | Device provisioning and commissioning |

### Fleet Management

| Document | Description |
|----------|-------------|
| [MANAGEMENT-SYSTEM.md](MANAGEMENT-SYSTEM.md) | Fleet provisioning, OTA, monitoring |
| [MULTI-TENANT.md](MULTI-TENANT.md) | Org/user/role management |
| [PLUGIN-SYSTEM.md](PLUGIN-SYSTEM.md) | Server plugin architecture |
| [api_migration_guide.md](api_migration_guide.md) | API evolution tracking |

### Research & Reference

| Document | Description |
|----------|-------------|
| [ESP32_ECOSYSTEM_RESEARCH.md](ESP32_ECOSYSTEM_RESEARCH.md) | Component libraries survey |
| [ESP32_LIBRARY_ECOSYSTEM.md](ESP32_LIBRARY_ECOSYSTEM.md) | Detailed library evaluation |
| [lvgl_integration_guide.md](lvgl_integration_guide.md) | LVGL framework integration |
| [panel_driver_research.md](panel_driver_research.md) | Display panel driver investigation |
| [DISPLAY_LESSONS.md](DISPLAY_LESSONS.md) | Display debugging lessons learned |
| [tritium_ecosystem_audit.md](tritium_ecosystem_audit.md) | Full ecosystem audit |

## Related References

- **HAL Libraries:** [../lib/README.md](../lib/README.md) — 40+ libraries with dependency graph
- **Apps:** [../apps/README.md](../apps/README.md) — 10 apps with build flags
- **Build System:** [../CLAUDE.md](../CLAUDE.md) — Makefile targets and PlatformIO
- **Parent System:** [../../docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) — Full Tritium architecture
- **Shared Library:** [../../tritium-lib/CLAUDE.md](../../tritium-lib/CLAUDE.md) — Models and MQTT topics
- **Command Center:** [../../tritium-sc/CLAUDE.md](../../tritium-sc/CLAUDE.md) — SC integration
