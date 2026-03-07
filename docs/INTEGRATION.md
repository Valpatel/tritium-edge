# Tritium-Edge Integration Guide

Copyright 2026 Valpatel Software LLC. Created by Matthew Valancy. Licensed under AGPL-3.0.

This document describes how Tritium-Edge fits into the broader Tritium ecosystem,
integration patterns with tritium-sc, MQTT topic conventions, and the migration
path from standalone deployment to full ecosystem integration.

---

## Table of Contents

- [Tritium Ecosystem](#tritium-ecosystem)
- [Integration Patterns](#integration-patterns)
  - [Pattern 1: Git Submodule](#pattern-1-git-submodule)
  - [Pattern 2: Standalone + MQTT Bridge](#pattern-2-standalone--mqtt-bridge)
  - [Pattern 3: Shared tritium-lib](#pattern-3-shared-tritium-lib-future)
- [MQTT Topic Mapping](#mqtt-topic-mapping)
- [ESP32 as Sensor Node](#esp32-as-sensor-node)
- [Data Flow Architecture](#data-flow-architecture)
- [Authentication and Security](#authentication-and-security)
- [Migration Path](#migration-path)
- [Troubleshooting](#troubleshooting)

---

## Tritium Ecosystem

The Tritium ecosystem consists of three components at different levels of the stack:

```
tritium-sc          Battlespace management, Commander Amy, security monitoring
tritium-edge        Edge device fleet management, OTA updates, remote configuration
tritium-lib         Shared library (future): event bus, MQTT topics, config patterns
```

**tritium-sc** is the central command and control platform. It manages sensor networks,
runs analysis pipelines (YOLO object detection, Whisper STT), and provides
Commander Amy as the operator interface. Models are tools in the loop, not the system itself.

**tritium-edge** manages the ESP32 device fleet at the edge. It handles device
registration, firmware OTA updates, remote configuration, heartbeat monitoring, and
telemetry collection.

**tritium-lib** is a planned shared library that will extract common patterns used by
both tritium-sc and tritium-edge into a reusable Python package.

### Component Responsibilities

| Component | Scope | Key Features |
|-----------|-------|-------------|
| tritium-sc | Central platform | Sensor fusion, model pipelines, target tracking, Amy |
| tritium-edge | Edge fleet | Device registration, OTA, heartbeat, telemetry |
| tritium-lib | Shared code | Event bus, MQTT codecs, config models, auth |

---

## Integration Patterns

### Pattern 1: Git Submodule

tritium-sc includes tritium-edge as a git submodule, running it as a plugin within
the tritium-sc process.

```
tritium-sc/
  plugins/
    tritium-edge/  <-- git submodule -> git@github.com:Valpatel/tritium-edge.git
      plugin.json
      server/
      docs/
```

In this pattern:

- tritium-edge runs as a plugin within tritium-sc (see [PLUGIN-SYSTEM.md](PLUGIN-SYSTEM.md))
- The edge server shares the same FastAPI application via plugin route registration
- ESP32 devices appear as sensor nodes in tritium-sc's device graph
- OTA management, heartbeat monitoring, and config push all operate through tritium-sc's API
- A single deployment runs both platforms

**Submodule setup:**

```bash
cd tritium-sc
git submodule add git@github.com:Valpatel/tritium-edge.git plugins/tritium-edge
git submodule update --init --recursive
```

**Plugin manifest for submodule integration:**

```json
{
  "name": "tritium-edge",
  "version": "1.0.0",
  "description": "ESP32 edge device fleet management",
  "author": "Valpatel Software LLC",
  "entry": "server/plugin_entry.py",
  "requires": ["tritium-sc>=1.0"],
  "hooks": {
    "routes": true,
    "commands": true,
    "heartbeat": true,
    "ui_pages": true
  }
}
```

When loaded as a tritium-sc plugin, tritium-edge registers its device management
routes under `/api/plugins/tritium-edge/`, exposes ESP32 command types to Amy,
and feeds heartbeat data into the tritium-sc event bus.

### Pattern 2: Standalone + MQTT Bridge

tritium-edge runs as an independent service and bridges to tritium-sc via MQTT.
This is the recommended pattern for distributed deployments where the edge server
runs on-site near the ESP32 devices.

```
                    MQTT Broker
                        |
    +-------------------+-------------------+
    |                                       |
tritium-edge server                    tritium-sc
    |                                       |
ESP32 devices                     Model pipelines, Amy
```

The `mqtt-bridge` plugin (see [PLUGIN-SYSTEM.md](PLUGIN-SYSTEM.md)) handles the
MQTT connection and topic mapping.

**MQTT bridge configuration:**

```yaml
# tritium-edge server config
plugins_enabled: true

# plugins/mqtt-bridge/config.yaml
mqtt:
  broker: mqtt.example.com
  port: 1883
  username: edge-server-01
  password: ${MQTT_PASSWORD}
  site_id: site-alpha
  qos: 1
  topics:
    heartbeat: "tritium/{site}/edge/{device_id}/telemetry"
    commands: "tritium/{site}/edge/{device_id}/commands"
    status: "tritium/{site}/edge/{device_id}/status"
```

**tritium-sc consumer configuration:**

```python
# tritium-sc sensor node definition
class EdgeDeviceNode(MQTTSensorNode):
    topic_pattern = "tritium/+/edge/+/telemetry"
    node_type = "esp32-edge"

    def on_message(self, topic: str, payload: dict):
        device_id = topic.split("/")[3]
        self.update_telemetry(device_id, payload)
```

### Pattern 3: Shared tritium-lib (Future)

When integration patterns between tritium-sc and tritium-edge stabilize, common
code will be extracted into `tritium-lib` as an installable Python package.

```
tritium-lib/
  tritium_lib/
    events.py       Event bus interface
    mqtt.py         Topic conventions, codecs, connection management
    config.py       Pydantic settings base classes
    models.py       Shared data models (Device, Sensor, Command)
    auth.py         JWT token utilities, API key validation
```

**Planned modules:**

| Module | Purpose | Used By |
|--------|---------|---------|
| `tritium_lib.events` | Event bus abstraction (in-process and MQTT-backed) | sc, edge |
| `tritium_lib.mqtt` | Topic string builders, payload codecs, QoS policies | sc, edge |
| `tritium_lib.config` | Pydantic `BaseSettings` subclass with env/file loading | sc, edge |
| `tritium_lib.models` | Shared Pydantic models for devices, sensors, commands | sc, edge |
| `tritium_lib.auth` | JWT encode/decode, API key hashing, permission scopes | sc, edge |

**Usage after extraction:**

```python
# In tritium-edge
from tritium_lib.models import Device, HeartbeatPayload
from tritium_lib.mqtt import TopicBuilder
from tritium_lib.config import TritiumSettings

topics = TopicBuilder(site="site-alpha")
topic = topics.edge_telemetry(device_id="esp32-001")
# -> "tritium/site-alpha/edge/esp32-001/telemetry"
```

Both tritium-sc and tritium-edge would declare tritium-lib as a dependency:

```toml
# pyproject.toml
[project]
dependencies = [
    "tritium-lib>=1.0",
]
```

---

## MQTT Topic Mapping

The following table maps tritium-edge data streams to MQTT topics and their
corresponding consumers in tritium-sc.

| Edge Data | MQTT Topic | tritium-sc Consumer |
|-----------|-----------|---------------------|
| Device heartbeat | `tritium/{site}/edge/{id}/telemetry` | MQTTSensorNode |
| Camera frame | `tritium/{site}/cameras/{id}/frame` | YOLO pipeline |
| Audio stream | `tritium/{site}/audio/{id}/stream` | Whisper STT |
| IMU data | `tritium/{site}/sensors/{id}/imu` | Target tracker |
| Mesh status | `tritium/{site}/mesh/{id}/peers` | Network graph |
| OTA status | `tritium/{site}/edge/{id}/ota` | Fleet dashboard |
| Device commands | `tritium/{site}/edge/{id}/commands` | Edge server |
| Config updates | `tritium/{site}/edge/{id}/config` | Edge server |

### Topic Conventions

- All topics begin with `tritium/{site_id}/` for multi-site isolation.
- Device-specific topics include the device ID as a path segment.
- Telemetry flows from edge to sc (publish from edge, subscribe in sc).
- Commands flow from sc to edge (publish from sc, subscribe in edge).
- QoS 1 (at least once) for telemetry; QoS 2 (exactly once) for commands and OTA.

### Heartbeat Payload Format

```json
{
  "device_id": "esp32-001",
  "mac": "20:6E:F1:9A:12:00",
  "firmware_version": "1.2.0",
  "board_type": "touch-lcd-35bc",
  "uptime_s": 86400,
  "heap_free": 180000,
  "heap_min": 120000,
  "rssi": -52,
  "ip": "192.168.1.100",
  "timestamp": "2026-03-07T12:00:00Z",
  "sensors": {
    "imu": true,
    "camera": true,
    "audio": true,
    "rtc": true
  }
}
```

### Camera Frame Payload

Camera frames are published as binary JPEG payloads with metadata in MQTT user
properties (MQTT v5) or a JSON header followed by binary data (MQTT v3.1.1).

```json
{
  "device_id": "esp32-001",
  "resolution": "VGA",
  "format": "jpeg",
  "quality": 80,
  "timestamp": "2026-03-07T12:00:00Z",
  "frame_size": 28450
}
```

---

## ESP32 as Sensor Node

When integrated with tritium-sc, ESP32 devices managed by tritium-edge become
first-class sensor nodes in the tritium-sc device graph. Each ESP32 maps to a
`SensorNode` instance, similar to how tritium-sc manages BCC950 cameras, IP cameras,
and other networked sensors.

### Capability Mapping

| ESP32 Capability | tritium-sc Role | Pipeline |
|-----------------|-----------------|----------|
| OV5640 camera | Visual sensor | YOLO object detection |
| ES8311 audio | Audio sensor | Whisper STT, VAD |
| QMI8658 IMU | Motion sensor | Target tracker |
| ESP-NOW mesh | Network relay | Mesh topology graph |
| GPIO/ADC | Environmental sensor | Custom analytics |

### Camera Integration

Camera-equipped ESP32s (e.g., touch-lcd-35bc with OV5640) feed the YOLO detection
pipeline in tritium-sc:

1. ESP32 captures JPEG frames at configured interval.
2. Frames are published to `tritium/{site}/cameras/{id}/frame`.
3. tritium-sc's camera ingestion service subscribes and decodes frames.
4. YOLO pipeline runs detection, publishes results to target tracker.
5. Amy receives detection events and can task the ESP32 for higher resolution
   captures or pan/tilt adjustments.

### Audio Integration

Audio-equipped ESP32s (e.g., touch-lcd-35bc with ES8311) feed the Whisper STT
pipeline:

1. ESP32 runs voice activity detection (VAD) locally.
2. When speech is detected, audio chunks stream to `tritium/{site}/audio/{id}/stream`.
3. tritium-sc's Whisper service transcribes the audio.
4. Transcriptions are routed to Amy for command interpretation or logging.

### IMU Integration

ESP32s with QMI8658 IMU provide motion and orientation data:

1. ESP32 samples accelerometer and gyroscope at configured rate.
2. Processed motion events (threshold crossings, orientation changes) are published
   to `tritium/{site}/sensors/{id}/imu`.
3. tritium-sc's target tracker consumes IMU events for motion detection and
   activity classification.

### Command Dispatch

Amy and tritium-sc operators can dispatch commands to ESP32 devices through the
edge server:

```python
# tritium-sc dispatching a command via MQTT
mqtt.publish(
    topic=f"tritium/{site}/edge/{device_id}/commands",
    payload={
        "cmd": "capture_photo",
        "params": {"resolution": "VGA", "quality": 90},
        "request_id": "abc-123"
    }
)
```

The edge server receives the command, validates it against the device's capabilities,
and queues it for delivery on the device's next heartbeat or via a push channel.

---

## Data Flow Architecture

### Standalone Deployment

```
ESP32 fleet
    |
    | HTTP heartbeat + WebSocket
    v
tritium-edge server
    |
    | REST API
    v
Admin dashboard
```

### Integrated Deployment (MQTT Bridge)

```
ESP32 fleet
    |
    | HTTP heartbeat + WebSocket
    v
tritium-edge server
    |
    | MQTT publish
    v
MQTT broker
    |
    | MQTT subscribe
    v
tritium-sc
    |
    +-- YOLO pipeline (camera frames)
    +-- Whisper STT (audio streams)
    +-- Target tracker (IMU + detections)
    +-- Amy (command dispatch, alerts)
    +-- Dashboard (fleet overview)
```

### Integrated Deployment (Submodule)

```
ESP32 fleet
    |
    | HTTP heartbeat + WebSocket
    v
tritium-sc (with tritium-edge plugin)
    |
    +-- Edge device management (registration, OTA, config)
    +-- YOLO pipeline (camera frames)
    +-- Whisper STT (audio streams)
    +-- Target tracker (IMU + detections)
    +-- Amy (unified command interface)
    +-- Dashboard (combined view)
```

---

## Authentication and Security

### Device Authentication

ESP32 devices authenticate with the edge server using device-specific API keys
issued during provisioning (see [DEVICE-PROTOCOL.md](DEVICE-PROTOCOL.md)).

### Server-to-Server Authentication

When tritium-edge communicates with tritium-sc via MQTT, authentication uses:

- **MQTT credentials**: Username/password or TLS client certificates for broker access.
- **Payload signing**: HMAC-SHA256 signatures on command payloads to prevent injection.
- **JWT tokens**: For REST API calls between servers, short-lived JWTs issued by a
  shared secret or certificate authority.

### Plugin Route Authentication

Plugin API routes inherit the edge server's authentication middleware. Plugins can
add additional authorization checks:

```python
from tritium_edge.auth import require_scope

@router.get("/sensitive-data")
@require_scope("plugin:alert-email:read")
async def get_sensitive_data():
    return {"data": "..."}
```

---

## Migration Path

The recommended migration path moves from standalone operation to full ecosystem
integration in four stages.

### Stage 1: Standalone

Start with tritium-edge running independently. The edge server manages the ESP32
fleet on its own with no external dependencies.

```
ESP32 fleet <-> tritium-edge server <-> Admin dashboard
```

At this stage:
- Device registration, heartbeat, and OTA are fully functional.
- The admin dashboard provides fleet visibility.
- No connection to tritium-sc.

### Stage 2: MQTT Bridge Plugin

Install the `mqtt-bridge` plugin to begin flowing data to tritium-sc. Both systems
run independently but share telemetry via MQTT.

```
ESP32 fleet <-> tritium-edge --MQTT--> tritium-sc
```

At this stage:
- Heartbeat telemetry is mirrored to tritium-sc.
- ESP32 devices appear as sensor nodes in tritium-sc.
- Amy can view device status but cannot yet dispatch commands.
- tritium-edge remains the authoritative device manager.

### Stage 3: Submodule Integration

Embed tritium-edge as a git submodule within tritium-sc. The edge server runs as
a tritium-sc plugin, sharing the same process and database.

```
ESP32 fleet <-> tritium-sc (with tritium-edge plugin)
```

At this stage:
- Single deployment manages both platforms.
- Amy has full command dispatch to ESP32 devices.
- Camera and audio streams feed directly into model pipelines.
- Unified dashboard shows fleet status alongside sensor fusion data.

### Stage 4: Extract tritium-lib

Once integration patterns stabilize, extract shared code into `tritium-lib`:

```
tritium-lib (shared package)
    |
    +-- tritium-sc (imports tritium-lib)
    +-- tritium-edge (imports tritium-lib)
```

At this stage:
- Common models, event bus, MQTT codecs, and auth utilities live in one package.
- Both projects stay decoupled but share a stable interface contract.
- New Tritium ecosystem components can import tritium-lib for instant compatibility.

---

## Troubleshooting

### MQTT Bridge Not Connecting

1. Verify broker address and credentials in `plugins/mqtt-bridge/config.yaml`.
2. Check that the MQTT broker allows the configured topic prefixes.
3. Inspect edge server logs for connection errors: `grep mqtt logs/server.log`.
4. Test broker connectivity: `mosquitto_pub -h broker -t test -m hello`.

### Devices Not Appearing in tritium-sc

1. Confirm the mqtt-bridge plugin is loaded: `GET /api/admin/plugins`.
2. Verify topic patterns match between the bridge config and tritium-sc subscriber.
3. Check that `site_id` matches in both configurations.
4. Monitor the MQTT broker for published messages: `mosquitto_sub -t "tritium/#" -v`.

### Submodule Version Mismatch

When updating the tritium-edge submodule within tritium-sc:

```bash
cd tritium-sc/plugins/tritium-edge
git fetch origin
git checkout v1.2.0
cd ../..
git add plugins/tritium-edge
git commit -m "Update tritium-edge submodule to v1.2.0"
```

Ensure the tritium-edge version satisfies the `requires` constraint in its
`plugin.json` manifest.
