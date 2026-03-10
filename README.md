# Tritium-Edge

Firmware for ESP32-S3 IoT devices running Tritium-OS.

## What it does

- **Custom real-time OS with touchscreen shell** — LVGL-based launcher, settings, status bar, and toast notifications across all supported displays
- **16 built-in services** — WiFi (multi-network failover), BLE scanner, ESP-NOW mesh networking, OTA updates, heartbeat, diagnostics, LoRa, and more
- **REST API and web dashboard** — 90+ endpoints for remote control, file management, screenshots, and OTA — browse it at `http://<device-ip>/`
- **6 Waveshare ESP32-S3 boards** — AMOLED, LCD, and RGB parallel displays from 1.8" to 4.3", all from one codebase
- **Built on ESP-IDF 5.5.2** with PlatformIO — no external framework dependencies

## Supported Boards

| Board | Resolution | Display | Status |
|---|---|---|---|
| ESP32-S3-Touch-AMOLED-2.41-B | 450x600 | RM690B0 QSPI | HW Verified |
| ESP32-S3-AMOLED-1.91-M | 240x536 | RM67162 QSPI | Needs verification |
| ESP32-S3-Touch-AMOLED-1.8 | 368x448 | SH8601Z QSPI | Needs verification |
| ESP32-S3-Touch-LCD-3.5B-C | 320x480 | AXS15231B QSPI | HW Verified |
| ESP32-S3-Touch-LCD-4.3C-BOX | 800x480 | ST7262 RGB | Pin-verified |
| ESP32-S3-Touch-LCD-3.49 | 172x640 | AXS15231B QSPI | HW Verified |

All boards: ESP32-S3 dual-core 240 MHz, 16 MB flash, 8 MB PSRAM, WiFi, BLE 5, USB-C.

## Quick Start

1. **Install PlatformIO**
   ```bash
   ./scripts/setup.sh
   ```

2. **Build**
   ```bash
   make build BOARD=touch-lcd-35bc
   ```

3. **Flash**
   ```bash
   make flash BOARD=touch-lcd-35bc
   ```

Use `make list-boards` to see all boards and `make list-apps` to see available apps.

## Project Structure

```
tritium-edge/
├── src/              Entry point (main.cpp)
├── include/          Shared headers and per-board pin definitions
├── lib/              HAL libraries, display drivers, OS services (see lib/README.md)
├── apps/             Application implementations (starfield, system, camera, ...)
├── scripts/          Dev workflow (build, flash, monitor, identify)
├── docs/             Architecture, board guides, known issues
├── platformio.ini    Build configuration (per-board + per-app environments)
└── Makefile          Build automation
```

## Where to go next

- [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) — Setup, first build, first flash
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — System design and service architecture
- [docs/ADDING_AN_APP.md](docs/ADDING_AN_APP.md) — How to create a new app
- [docs/ADDING_A_BOARD.md](docs/ADDING_A_BOARD.md) — How to add board support
- [docs/boards.md](docs/boards.md) — Detailed board specs and links

---

Copyright 2026 Valpatel Software LLC. Licensed under [AGPL-3.0](LICENSE).
