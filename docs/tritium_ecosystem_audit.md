# Tritium Ecosystem Audit

Conducted 2026-03-07. All findings based on reading actual source code and config files.

---

## 1. tritium-edge (`/home/scubasonar/Code/esp32-hardware/`)

### Identity

ESP32-S3 multi-board display firmware + a Python fleet management server. GitHub: `Valpatel/tritium-edge`. AGPL-3.0.

### Git State

- Branch: `main`, clean working tree
- Latest commit: `9796f54 Fix firmware attestation — image-aware SHA-256 hash`
- 20+ commits covering fleet connection, OTA, heartbeat, mesh, device provisioning, admin portal

### Apps (`apps/`)

| App | Description |
|-----|-------------|
| `starfield` | Default demo app (star animation) |
| `camera` | Camera preview (3.5B-C only) |
| `system` | Full hardware dashboard (IMU, RTC, power, audio, camera, mesh) |
| `ui_demo` | LVGL UI demo |
| `wifi_setup` | WiFi configuration |
| `effects` | Visual effects |
| `ota` | OTA update manager |
| `test` | Test app |
| `_template` | Scaffold template for `make new-app` |

### HAL Libraries (`lib/`)

30 libraries total:

| Library | Purpose |
|---------|---------|
| `hal_audio` | ES8311 codec + I2S |
| `hal_ble` | BLE |
| `hal_ble_ota` | BLE-based OTA |
| `hal_camera` | OV5640 DVP via esp_camera |
| `hal_debug` | Debug logging |
| `hal_espnow` | ESP-NOW + multi-hop mesh |
| `hal_fs` | LittleFS wrapper |
| `hal_heartbeat` | HTTP heartbeat to fleet server |
| `hal_imu` | QMI8658 IMU |
| `hal_io_expander` | TCA9554 I/O expander |
| `hal_mqtt` | MQTT client + AI bridge |
| `hal_ntp` | NTP time sync |
| `hal_ota` | OTA updates (WiFi/SD/dual partition) |
| `hal_power` | AXP2101 PMIC |
| `hal_provision` | Device provisioning |
| `hal_rtc` | PCF85063 RTC |
| `hal_sdcard` | SDMMC 1-bit |
| `hal_sleep` | Light/deep sleep |
| `hal_touch` | Touch HAL |
| `hal_ui` | LVGL init + theme + widgets |
| `hal_voice` | VAD + keyword spotting |
| `hal_watchdog` | Task watchdog |
| `hal_webserver` | HTTP server with dashboard |
| `hal_wifi` | Multi-network WiFi with NVS |
| `Panel_AXS15231B` | Custom LovyanGFX panel driver |
| `Bus_SPI_AllQuad` | Custom QSPI bus driver |
| `StarField` | Starfield animation engine |
| `GfxEffects` | Graphics effects library |
| `BatteryMonitor` | Battery monitoring |
| `BatteryWidget` | Battery UI widget |

### PlatformIO Environments

**6 app presets** defined as reusable sections: `[app_starfield]`, `[app_ui_demo]`, `[app_wifi_setup]`, `[app_ota]`, `[app_system]` (no `[app_camera]` or `[app_effects]` visible in platformio.ini).

**6 boards**, each with `-DBOARD_*` flag and force-included board header:

| Board Env | Board Flag | Display |
|-----------|-----------|---------|
| `touch-amoled-241b` | `BOARD_TOUCH_AMOLED_241B` | RM690B0 QSPI 450x600 |
| `amoled-191m` | `BOARD_AMOLED_191M` | RM67162 QSPI 240x536 |
| `touch-amoled-18` | `BOARD_TOUCH_AMOLED_18` | SH8601Z QSPI 368x448 |
| `touch-lcd-35bc` | `BOARD_TOUCH_LCD_35BC` | AXS15231B QSPI 320x480 |
| `touch-lcd-43c-box` | `BOARD_TOUCH_LCD_43C_BOX` | ST7262 RGB 800x480 |
| `touch-lcd-349` | `BOARD_TOUCH_LCD_349` | AXS15231B QSPI 172x640 |

**22 total environments** including:
- 6 base board envs (starfield default)
- 4 fleet-connected variants (`*-fleet` with hardcoded WiFi creds + server URL)
- 6 OTA variants (`*-ota` using `partitions_ota_16MB.csv`)
- 2 starfield+OTA variants
- 2 board+app combos (`touch-amoled-241b-ui`, `touch-amoled-241b-wifi`, `touch-lcd-35bc-system`)
- 1 simulator

### Simulator

Native SDL2 desktop simulator at `sim/`:
- `sim/sim_main.cpp` — entry point
- `sim/sim_display.h` — SDL display abstraction
- `sim/sim_hal.h` — HAL stubs

Environment `[env:simulator]` uses `platform = native`, links SDL2, builds only starfield app. HAL libraries that are ESP32-specific (heartbeat, etc.) have `#ifdef SIMULATOR` stubs.

### Fleet Management Server (`server/`)

A FastAPI Python server inside the edge repo:
- `server/app/main.py` — FastAPI entry with lifespan, auth middleware, 11 routers
- **Routers**: devices, firmware, ota, commission, events, stats, profiles, ws (WebSocket), groups, audit, mesh
- **Services**: device_service, ota
- **Auth**: JWT-based (`server/app/auth/jwt.py`)
- **Frontend**: Static HTML/CSS/JS admin portal (`server/app/static/`)
- **Config**: Pydantic settings
- **Data**: JSON file-based store (`FleetStore`)
- **Dependencies**: FastAPI, uvicorn, pydantic-settings, pyjwt, bcrypt, pyserial, cryptography, aiofiles

The server communicates with devices via **HTTP heartbeat** (`POST /api/devices/{id}/status`), not MQTT. Devices poll the server; server responds with pending commands, OTA assignments, and config updates.

**Key finding**: `server/app/models.py` contains a `DeviceHeartbeat` model that is a **duplicate** of `tritium_lib.models.DeviceHeartbeat` (identical fields). The file contains a comment: *"These models are designed for future extraction into tritium-lib."* Similarly, `server/app/auth/jwt.py` says *"Designed for extraction into tritium-lib."* Neither file actually imports from tritium-lib today.

---

## 2. tritium-lib (`/home/scubasonar/Code/tritium-lib/`)

### Identity

Python shared library for the Tritium ecosystem. GitHub: `Valpatel/tritium-lib`. AGPL-3.0. Version 0.1.0.

### Git State

- Branch: `main` (only branch)
- 3 commits total (most recent: `91f7677 Add Part of Tritium section`)
- Very early stage

### Structure

```
tritium-lib/
  pyproject.toml
  README.md
  src/tritium_lib/
    __init__.py
    models/
      __init__.py
      device.py      # Device, DeviceHeartbeat, DeviceCapabilities
      command.py     # Command models
      firmware.py    # Firmware models
      sensor.py      # SensorReading models
    mqtt/
      __init__.py
      topics.py      # TritiumTopics class — topic builder
    auth/
      __init__.py
      jwt.py         # JWT create/decode utilities
    events/
      __init__.py
      bus.py         # Thread-safe pub/sub EventBus
    cot/
      __init__.py
      codec.py       # CoT XML codec (device_to_cot, sensor_to_cot, parse_cot)
    config/
      __init__.py    # Pydantic settings base class
  tests/
```

### Dependencies

- Core: `pydantic>=2.0`, `pydantic-settings>=2.0`, `pyjwt>=2.8.0`
- Optional `[mqtt]`: `paho-mqtt>=2.0`
- Optional `[full]`: adds `bcrypt>=4.1.0`

### Actual Usage

**tritium-lib is not imported by any other repo.** Neither tritium-edge nor tritium-sc contain `from tritium_lib import ...` or `import tritium_lib` in any source file. The library exists as a defined contract but is not yet wired in.

- `tritium-edge/server/app/models.py` duplicates `DeviceHeartbeat` and `Device` from tritium-lib
- `tritium-edge/server/app/auth/jwt.py` duplicates JWT utilities from tritium-lib
- `tritium-sc/src/engine/comms/cot.py` has its own CoT codec (more complex, with unit type registry integration)
- `tritium-sc/src/engine/comms/event_bus.py` has its own EventBus (simpler, queue-based, no wildcard routing)

### What tritium-lib Contains vs. What the Repos Have

| Component | tritium-lib | tritium-edge server | tritium-sc |
|-----------|-------------|-------------------|------------|
| DeviceHeartbeat model | `models/device.py` | `server/app/models.py` (duplicate) | Not used |
| Device model | `models/device.py` | `server/app/models.py` (duplicate) | Not used |
| JWT auth | `auth/jwt.py` | `server/app/auth/jwt.py` (duplicate) | Not examined |
| MQTT topics | `mqtt/topics.py` | Not used (HTTP heartbeat) | Not imported (has own MQTT patterns) |
| CoT codec | `cot/codec.py` (edge-focused) | Not used | `engine/comms/cot.py` (own impl, richer) |
| EventBus | `events/bus.py` (wildcard routing) | Not used | `engine/comms/event_bus.py` (own impl, simpler) |
| Config base | `config/__init__.py` | `server/app/config.py` (own) | `src/app/config.py` (own) |

---

## 3. tritium-sc (`/home/scubasonar/Code/tritium-sc/`)

### Identity

Battlespace management system with AI Commander "Amy". GitHub: `Valpatel/tritium-sc`. AGPL-3.0. Python 3.12+, FastAPI, YOLO, Ollama, MQTT.

### Git State

- Latest commit: `63b59fa Retake hero screenshots with distinct visual styles`
- Active development (layer browser, mission generator, screenshots)

### What It Is

A full-stack tactical command-and-control system for Nerf battles and neighborhood security:
- **Backend**: FastAPI server on port 8000, SQLite, async
- **Frontend**: Vanilla JS, Canvas 2D tactical map, Three.js 3D, CYBERCORE CSS
- **AI**: YOLOv8 object detection, ByteTrack, Ollama fleet (local LLMs), whisper.cpp STT, Piper TTS
- **Commander Amy**: 4-layer AI consciousness (reflex/instinct/awareness/deliberation)
- **Simulation**: 10Hz tick engine, wave-based combat, projectile physics
- **Comms**: MQTT bridge, CoT/TAK integration, WebSocket real-time

### Relationship to tritium-edge

tritium-sc is "the brain" and tritium-edge is "the nervous system". They are designed to communicate via MQTT, but the current integration is:

1. **MQTT bridge** in tritium-sc (`src/engine/comms/mqtt_bridge.py`) — publishes/subscribes to `tritium/{site}/...` topics
2. **MQTT topics** defined in tritium-sc match the patterns in tritium-lib's `TritiumTopics`:
   - `tritium/{site}/cameras/{id}/detections`
   - `tritium/{site}/robots/{id}/telemetry`
   - `tritium/{site}/robots/{id}/command`
   - `tritium/{site}/amy/alerts`
   - `tritium/{site}/escalation/change`
3. **ESP32 firmware** (`hal_mqtt`) has a MQTT client but uses different topic patterns (`esp32/{clientId}/audio/stream`, `ai/{clientId}/text/response`)
4. **Heartbeat** is HTTP-based (edge devices POST to fleet server), not MQTT
5. **No direct code sharing** between the two repos today

### Integration Points (Current)

| Channel | Direction | Protocol | Status |
|---------|-----------|----------|--------|
| Device heartbeat | edge -> edge server | HTTP POST | Working |
| OTA delivery | edge server -> edge | HTTP download | Working |
| Config sync | edge server -> edge | HTTP (heartbeat response) | Working |
| MQTT bridge | sc <-> broker | MQTT | Working in sc, not connected to edge |
| MQTT AI bridge | edge <-> AI server | MQTT | In hal_mqtt, different topic namespace |
| WebSocket | sc -> browser | WS | Working |
| CoT/TAK | sc -> TAK server | TCP/TLS | Implemented |

---

## 4. tritium (top-level `/home/scubasonar/Code/tritium/`)

### Identity

Top-level monorepo umbrella. GitHub: `Valpatel/tritium`. Contains only a README and 3 git submodules.

### Git State

- 8 commits total (most recent: `1ce7c34 Tighten README`)
- Purely documentation/vision repo

### Submodules (`.gitmodules`)

```
[submodule "tritium-sc"]    -> git@github.com:Valpatel/tritium-sc.git
[submodule "tritium-edge"]  -> git@github.com:Valpatel/tritium-edge.git
[submodule "tritium-lib"]   -> git@github.com:Valpatel/tritium-lib.git
```

### Structure

```
tritium/
  README.md          # Vision doc: "Distributed Cybernetic Operating System"
  tritium-sc/        # Submodule
  tritium-edge/      # Submodule
  tritium-lib/       # Submodule
```

### Key Claims in README

- tritium-sc = "The Brain" (command, vision, models, TAK)
- tritium-edge = "The Nervous System" (fleet management, OTA, config)
- tritium-lib = "The Spine" (shared models, MQTT, auth, CoT)
- Communication: MQTT between sc and edge
- Quick start mentions `tritium-edge/server` as a separate server on port 8080

There is no server component in the top-level repo itself. No CI/CD, no Makefile, no docker-compose.

---

## 5. Integration Points and Shared Code Analysis

### Code That IS Duplicated Today

| Code | Location 1 | Location 2 | Notes |
|------|-----------|-----------|-------|
| `DeviceHeartbeat` model | `tritium-lib/src/tritium_lib/models/device.py` | `tritium-edge/server/app/models.py` | Field-for-field identical |
| `Device` model | `tritium-lib/src/tritium_lib/models/device.py` | `tritium-edge/server/app/models.py` | Field-for-field identical |
| JWT utilities | `tritium-lib/src/tritium_lib/auth/jwt.py` | `tritium-edge/server/app/auth/jwt.py` | Both noted as "designed for extraction" |
| CoT codec | `tritium-lib/src/tritium_lib/cot/codec.py` | `tritium-sc/src/engine/comms/cot.py` | Different implementations; sc version is richer |
| EventBus | `tritium-lib/src/tritium_lib/events/bus.py` | `tritium-sc/src/engine/comms/event_bus.py` | Different implementations; lib has wildcard routing, sc is simpler |

### MQTT Topic Namespace Mismatch

tritium-lib defines the canonical topic structure as `tritium/{site}/edge/{device_id}/...`, and tritium-sc uses `tritium/{site}/cameras/...`, `tritium/{site}/robots/...`.

The ESP32 firmware's `hal_mqtt` and `mqtt_ai_bridge` use a **completely different** namespace: `esp32/{clientId}/audio/stream`, `ai/{clientId}/text/response`. These do not match either tritium-lib or tritium-sc patterns.

### What EXISTS as Shared Abstractions

| Abstraction | Where It Lives | Notes |
|-------------|---------------|-------|
| MQTT topic conventions | tritium-lib only | Not imported anywhere |
| Device/Heartbeat models | tritium-lib + edge server (duplicated) | Not imported anywhere |
| CoT XML codec | tritium-lib + tritium-sc (separate impls) | Not imported anywhere |
| EventBus | tritium-lib + tritium-sc (separate impls) | Not imported anywhere |
| JWT auth | tritium-lib + edge server (separate impls) | Not imported anywhere |
| Pydantic config base | tritium-lib only | Not imported anywhere |

### What Does NOT Exist as Shared Abstractions

| Gap | Notes |
|-----|-------|
| Display/UI abstraction | Entirely in tritium-edge (`display_init.h`, `hal_ui`, LovyanGFX, LVGL). No display code in tritium-lib. |
| ESP-IDF / panel driver abstraction | `Panel_AXS15231B`, `Bus_SPI_AllQuad` live in tritium-edge `lib/`. C++ only. |
| LVGL configuration | `lv_conf.h` and `hal_ui` are tritium-edge only |
| Firmware build system | PlatformIO config is tritium-edge only |
| Sensor data models (C++ side) | No shared C/C++ headers across repos. The heartbeat JSON payload is hand-built in `hal_heartbeat.cpp`. |
| C/C++ shared protocol headers | The edge firmware builds its JSON payloads manually with `snprintf`. There are no shared `.h` files that match tritium-lib's Python models. |

### The Python Server in tritium-edge

The fleet management server (`tritium-edge/server/`) is a Python FastAPI app that:
- Shares no code with tritium-sc (separate codebase, separate patterns)
- Duplicates models from tritium-lib but does not import them
- Runs on port 8080 (vs. tritium-sc on port 8000)
- Has its own auth, config, and store implementations
- Has its own static frontend (CYBERCORE admin portal)

### Language Boundary

- **tritium-lib**: Python only
- **tritium-sc**: Python + vanilla JS (frontend)
- **tritium-edge firmware**: C++17 (Arduino/ESP-IDF)
- **tritium-edge server**: Python (FastAPI)

tritium-lib cannot be used by the ESP32 firmware directly (Python vs. C++). The shared contract only applies to Python-side consumers (the edge server and tritium-sc), neither of which actually imports it today.

---

## Summary of Current State

1. **tritium-lib exists but is unused.** Zero imports from any other repo. All three of its consumers (edge server, tritium-sc, ESP32 firmware) have their own implementations.

2. **The edge server duplicates tritium-lib models.** `DeviceHeartbeat` and `Device` are copy-pasted with comments saying "designed for future extraction." The extraction target (tritium-lib) already has these models, but nobody wired them up.

3. **MQTT topics are fragmented.** Three different topic namespaces exist: tritium-lib's canonical `tritium/{site}/...`, tritium-sc's actual usage, and the ESP32 firmware's `esp32/{clientId}/...`.

4. **CoT and EventBus have diverged.** tritium-lib and tritium-sc each have their own CoT codec and EventBus. The sc versions are more mature and integrated with the rest of the sc codebase.

5. **No C/C++ shared code exists.** The ESP32 firmware hand-builds JSON for the heartbeat protocol. There is no header file or code generator that keeps the C++ side in sync with the Python models.

6. **Display and hardware abstractions are tritium-edge only.** LovyanGFX panel drivers, LVGL config, board pin definitions, and HAL libraries are all C++ in tritium-edge. tritium-lib has no display or hardware abstraction layer.

7. **The top-level tritium repo is documentation only.** It defines the three-pillar architecture but contains no code, CI, or orchestration.
