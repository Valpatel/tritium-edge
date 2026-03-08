# Tritium-Edge Device Protocol

Version: 2.0
Status: Draft
Last updated: 2026-03-07

---

## Table of Contents

1. [Overview](#1-overview)
2. [Heartbeat Protocol v2](#2-heartbeat-protocol-v2)
3. [Config Synchronization](#3-config-synchronization)
4. [Command Protocol](#4-command-protocol)
5. [Device Provisioning](#5-device-provisioning)
6. [OTA Firmware Updates](#6-ota-firmware-updates)
7. [Security Considerations](#7-security-considerations)
8. [Backward Compatibility](#8-backward-compatibility)
9. [ESP32 Implementation Notes](#9-esp32-implementation-notes)

---

## 1. Overview

The Tritium-Edge device protocol defines the communication contract between
edge devices and the Tritium-Edge Management Server. The protocol is
hardware-family-agnostic -- any device that can POST JSON over HTTPS can
participate. ESP32-S3 is the first implementation; STM32, nRF52, and Linux
SBCs use the same protocol. Devices self-describe their family, board, and
capabilities. The server treats them uniformly.

The protocol enables:

- **Health monitoring** -- periodic heartbeat with hardware telemetry
- **Config synchronization** -- server-driven desired state with drift detection
- **Command delivery** -- server-initiated actions via heartbeat piggyback
- **Firmware attestation** -- SHA-256 hash reporting and trust verification
- **Device provisioning** -- zero-touch onboarding of new devices
- **OTA orchestration** -- firmware update directives with rollback support

### Communication Model

```
+------------------+          HTTPS/TLS           +---------------------+
|   ESP32-S3       | ----- POST /heartbeat -----> | Tritium-Edge Server |
|   Edge Device    | <---- JSON response --------- |                     |
|                  |                               |                     |
|                  | ----- POST /provision ------> |                     |
|                  | <---- token + config --------- |                     |
+------------------+                               +---------------------+

All communication is device-initiated (pull model).
The server never opens connections to devices.
```

---

## 2. Heartbeat Protocol v2

The heartbeat is the primary communication channel. Devices poll the server at
a configurable interval, reporting state and receiving instructions.

### 2.1 Request: POST /api/device/heartbeat

| Field | Type | Required | Description |
|---|---|---|---|
| device_id | string | yes | MAC address (unique device identifier) |
| device_token | string | yes | JWT issued during provisioning |
| firmware_version | string | yes | Semantic version of running firmware |
| firmware_hash | string | yes | SHA-256 of firmware binary |
| board | string | yes | Board identifier (e.g. `touch-lcd-35bc`) |
| uptime_s | integer | yes | Seconds since last boot |
| free_heap | integer | yes | Free heap memory in bytes |
| wifi_rssi | integer | yes | WiFi signal strength in dBm |
| ip_address | string | yes | Current IPv4 address |
| boot_count | integer | yes | Total boot count from NVS |
| reported_config | object | yes | Current device configuration state |
| command_acks | array | no | Acknowledgments for previously received commands |
| ota_status | string | yes | One of: `idle`, `downloading`, `verifying`, `applying`, `failed` |
| mesh_peers | integer | no | Number of ESP-NOW mesh peers (0 if mesh disabled) |
| timestamp | integer | yes | Unix epoch seconds (from NTP or monotonic) |

**Full request example:**

```json
{
  "device_id": "20:6E:F1:9A:12:00",
  "device_token": "eyJhbG...",
  "firmware_version": "1.2.0",
  "firmware_hash": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2",
  "board": "touch-lcd-35bc",
  "uptime_s": 86400,
  "free_heap": 245760,
  "wifi_rssi": -42,
  "ip_address": "192.168.1.100",
  "boot_count": 7,
  "reported_config": {
    "modules": {
      "camera": { "enabled": true, "resolution": "QVGA" },
      "audio": { "enabled": true, "sample_rate": 16000 },
      "imu": { "enabled": true, "poll_rate_hz": 100 },
      "display": { "enabled": true, "brightness": 200 },
      "mesh": { "enabled": false }
    },
    "settings": {
      "heartbeat_interval_s": 60,
      "ntp_server": "pool.ntp.org",
      "display_brightness": 200
    },
    "gpio": {
      "2": { "mode": "output", "value": 1 },
      "4": { "mode": "input", "value": 0 }
    }
  },
  "command_acks": [
    { "id": "cmd-uuid-1", "status": "ok", "result": { "pin": 2, "value": 0 } }
  ],
  "ota_status": "idle",
  "mesh_peers": 3,
  "timestamp": 1709827200
}
```

### 2.2 Response: Heartbeat Reply

| Field | Type | Always Present | Description |
|---|---|---|---|
| status | string | yes | `ok` or `error` |
| server_time | integer | yes | Server Unix epoch for clock sync |
| heartbeat_interval_s | integer | yes | Interval for next heartbeat |
| desired_config | object | no | Included only when config drift detected |
| commands | array | no | Pending commands for this device |
| ota | object | no | OTA directive when firmware update needed |
| attestation | string | yes | Firmware trust status |

**Full response example:**

```json
{
  "status": "ok",
  "server_time": 1709827200,
  "heartbeat_interval_s": 60,
  "desired_config": {
    "modules": {
      "camera": { "enabled": true, "resolution": "VGA" },
      "mesh": { "enabled": true }
    },
    "settings": {
      "display_brightness": 128
    }
  },
  "commands": [
    {
      "id": "cmd-uuid-2",
      "type": "gpio_set",
      "payload": { "pin": 2, "value": 0 }
    },
    {
      "id": "cmd-uuid-3",
      "type": "reboot",
      "payload": {}
    }
  ],
  "ota": null,
  "attestation": "trusted"
}
```

### 2.3 Heartbeat Timing

| Condition | Behavior |
|---|---|
| Normal operation | Heartbeat every `heartbeat_interval_s` (default: 60s) |
| Connection failure | Exponential backoff: 60s, 120s, 240s, max 600s |
| After config change | Immediate heartbeat to confirm new state |
| After command execution | Immediate heartbeat with command acknowledgment |
| Server override | Server can adjust interval via response field |
| Minimum interval | 10 seconds (rate limit enforced server-side) |

### 2.4 HTTP Status Codes

| Code | Meaning | Device Action |
|---|---|---|
| 200 | Success | Process response normally |
| 401 | Token invalid/expired | Re-provision (see Section 5) |
| 429 | Rate limited | Back off, retry after header value |
| 503 | Server unavailable | Exponential backoff |

---

## 3. Config Synchronization

### 3.1 Config Cascade

The server computes the effective desired configuration by merging layers:

```
Profile default_config
        |
        v
   deep_merge
        |
        v
Device config_overrides
        |
        v
Effective desired_config
```

- **Profile default_config**: Baseline settings from the device's assigned profile.
- **Device config_overrides**: Per-device overrides set by administrators.
- **Effective desired_config**: The merged result that the device should match.

### 3.2 Drift Detection

The server compares `reported_config` from the heartbeat against the effective
`desired_config`:

1. If any keys differ, drift is detected.
2. The server includes only the **changed keys** in `desired_config` (partial update).
3. The device applies the changes and reports the new config on the next heartbeat.
4. If config matches, `desired_config` is omitted from the response (null/absent).

This approach minimizes bandwidth -- most heartbeat responses carry no config payload.

### 3.3 Module Enable/Disable

Each HAL module has an `enabled` boolean flag within `reported_config.modules`:

```json
{
  "modules": {
    "camera":  { "enabled": true, "resolution": "QVGA" },
    "audio":   { "enabled": true, "sample_rate": 16000 },
    "imu":     { "enabled": true, "poll_rate_hz": 100 },
    "rtc":     { "enabled": true },
    "display": { "enabled": true, "brightness": 200 },
    "mesh":    { "enabled": false },
    "mqtt":    { "enabled": false }
  }
}
```

Device behavior on config change:

| Transition | Action |
|---|---|
| `enabled: false` to `true` | Initialize HAL with provided settings |
| `enabled: true` to `false` | Deinitialize HAL, free resources |
| Settings change (enabled stays true) | Apply new params at runtime or reinit |

### 3.4 GPIO Remote Control

GPIO pins can be configured and controlled remotely via the `gpio` config block:

```json
{
  "gpio": {
    "2":  { "mode": "output", "value": 1 },
    "4":  { "mode": "input", "pull": "up" },
    "15": { "mode": "pwm", "value": 128, "freq": 5000 }
  }
}
```

| Mode | Fields | Description |
|---|---|---|
| `output` | `value` (0/1) | Digital output, high or low |
| `input` | `pull` (up/down/none) | Digital input with optional pull resistor |
| `pwm` | `value` (0-255), `freq` (Hz) | PWM output with duty cycle and frequency |

- GPIO changes are applied immediately upon receipt.
- Current GPIO state is reported in the next heartbeat.
- Pins used by HAL modules (I2C, SPI, camera DVP) must not be overridden via GPIO config.

---

## 4. Command Protocol

Commands are one-shot instructions delivered via the heartbeat response. They
differ from config in that they trigger an action rather than set desired state.

### 4.1 Command Types

| Type | Payload | Description |
|---|---|---|
| `reboot` | `{}` | Restart device |
| `gpio_set` | `{ "pin": int, "value": int }` | Set digital GPIO pin |
| `gpio_read` | `{ "pin": int }` | Read GPIO pin, result in next ack |
| `config_update` | `{ "path": string, "value": any }` | Update a specific config key |
| `ota_url` | `{ "url": string }` | Trigger WiFi pull OTA from URL |
| `ota_rollback` | `{}` | Roll back to previous firmware partition |
| `identify` | `{}` | Flash LED or display device ID on screen |
| `sleep` | `{ "duration_s": int }` | Enter deep sleep for specified duration |
| `wifi_add` | `{ "ssid": string, "password": string }` | Add WiFi network to NVS |
| `mesh_send` | `{ "firmware_path": string }` | Start ESP-NOW mesh OTA broadcast |
| `custom` | `{ "command": string }` | Arbitrary serial command passthrough |

### 4.2 Command Lifecycle

```
                                    +-------+
                                    | ACKED |
                          ack ok    +-------+
                        +---------->
    +---------+    delivered    +-----------+
    | PENDING | -------------> | DELIVERED |
    +---------+                +-----------+
         |                        |
         | TTL expired            | ack error
         v                        v
    +---------+              +--------+
    | EXPIRED |              | FAILED |
    +---------+              +--------+
```

1. **PENDING** -- Command created by user or server automation.
2. **DELIVERED** -- Command included in heartbeat response to device.
3. **ACKED** -- Device reports successful execution.
4. **FAILED** -- Device reports execution error with detail.
5. **EXPIRED** -- Device did not acknowledge within TTL (default: 5 minutes).

### 4.3 Command Acknowledgment

The device includes a `command_acks` array in the next heartbeat after execution:

```json
{
  "command_acks": [
    {
      "id": "cmd-uuid-2",
      "status": "ok",
      "result": { "pin": 2, "value": 0 }
    },
    {
      "id": "cmd-uuid-3",
      "status": "error",
      "error": "reboot_delayed"
    }
  ]
}
```

| Field | Type | Description |
|---|---|---|
| id | string | Command UUID from original delivery |
| status | string | `ok` or `error` |
| result | object | Command-specific result data (optional) |
| error | string | Error description when status is `error` |

The device retains pending acks in NVS until the server confirms receipt by
no longer including the command in subsequent heartbeat responses.

---

## 5. Device Provisioning

Provisioning is the process by which a new device obtains its identity token
and initial configuration from the server.

### 5.1 Provisioning Flow

```
 ESP32-S3 Device                          Tritium-Edge Server
 ===============                          ====================

 1. Boot (no device_token in NVS)
 2. Connect to WiFi (NVS or hardcoded)
 3. POST /api/device/provision ---------> 4. Validate MAC, create device record
    {                                        Assign to default org (or specified)
      mac, board, firmware_version,          Generate JWT device_token
      firmware_hash, capabilities
    }
 6. Store device_token in NVS <---------- 5. Return token + initial config
    Apply initial desired_config             {
                                               device_token, org_id,
 7. Begin heartbeat cycle                      device_id, heartbeat_interval_s,
                                               desired_config
                                             }
```

### 5.2 Provision Request: POST /api/device/provision

```json
{
  "mac": "20:6E:F1:9A:12:00",
  "board": "touch-lcd-35bc",
  "firmware_version": "1.0.0",
  "firmware_hash": "a1b2c3d4...",
  "capabilities": ["camera", "audio", "imu", "display", "touch"]
}
```

The `capabilities` array is derived from the board's `HAS_*` compile-time defines.

### 5.3 Provision Response

```json
{
  "device_token": "eyJhbG...",
  "org_id": "org-uuid",
  "device_id": "20:6E:F1:9A:12:00",
  "heartbeat_interval_s": 60,
  "desired_config": {
    "modules": {
      "camera": { "enabled": true, "resolution": "QVGA" },
      "audio": { "enabled": false },
      "display": { "enabled": true, "brightness": 200 }
    },
    "settings": {
      "ntp_server": "pool.ntp.org"
    }
  }
}
```

### 5.4 Re-provisioning

A device re-provisions when:

- The stored `device_token` is rejected with HTTP 401.
- The token JWT has expired.
- The organization has been deleted.

The server may require **admin approval** for new devices (configurable per org).
During the approval-pending state, the device displays provisioning status on
screen and retries every 30 seconds.

---

## 6. OTA Firmware Updates

OTA directives are delivered via the heartbeat response `ota` field.

### 6.1 OTA Directive

```json
{
  "ota": {
    "url": "https://server.example.com/api/device/firmware/1.3.0",
    "version": "1.3.0",
    "sha256": "b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c3",
    "size": 1048576,
    "force": false
  }
}
```

| Field | Type | Description |
|---|---|---|
| url | string | HTTPS URL to download firmware binary |
| version | string | Target firmware version |
| sha256 | string | Expected SHA-256 of the binary |
| size | integer | Binary size in bytes |
| force | boolean | If true, update even if version matches |

### 6.2 OTA State Machine

```
idle --> downloading --> verifying --> applying --> idle (new version)
  ^          |               |            |
  |          v               v            v
  +------- failed <-------- failed <--- failed
```

The device reports `ota_status` in every heartbeat during the update process.
On failure, the device includes an error detail and remains on the current firmware.

### 6.3 Rollback

If a device fails to heartbeat within 3 intervals after an OTA update, the
server marks it as potentially bricked and queues an `ota_rollback` command.
The device firmware validates boot integrity on startup and can self-rollback
using the ESP-IDF dual-partition scheme.

---

## 7. Security Considerations

### 7.1 Transport Security

All communication uses HTTPS with TLS 1.2+. Devices pin the server CA
certificate stored in the firmware or provisioned via NVS.

### 7.2 Authentication

- **Device tokens** are JWTs containing `device_id` and `org_id` claims.
- The server validates the token signature and org membership on every request.
- Tokens have a configurable expiry (default: 90 days). Devices re-provision
  on token expiry.

### 7.3 Firmware Attestation

| Attestation Value | Meaning |
|---|---|
| `trusted` | Firmware hash matches a known version in the server registry |
| `unknown` | Hash not recognized (custom build or unreported version) |
| `mismatch` | Hash does not match the expected version for this device |

The server maintains a hash-to-version mapping. Administrators can flag
`unknown` or `mismatch` devices for investigation.

### 7.4 Anti-Replay

- Heartbeats include a monotonically increasing `timestamp` field.
- The server rejects heartbeats with timestamps older than the last received.

### 7.5 Rate Limiting

- Maximum 1 heartbeat per 10 seconds per device.
- HTTP 429 returned when rate exceeded, with `Retry-After` header.

### 7.6 Provisioning Controls

- Organizations can enable **admin approval** for new devices.
- MAC allowlists can restrict which devices may provision.
- Provisioning endpoint is rate-limited per source IP.

---

## 8. Backward Compatibility

### 8.1 Migration from v1 Heartbeat

The v1 heartbeat (used by `fleet_server.py`) sends a minimal payload:

| v1 Field | v2 Equivalent |
|---|---|
| device_id | device_id |
| version | firmware_version |
| board | board |
| ip | ip_address |
| firmware_hash | firmware_hash |

Fields added in v2: `device_token`, `uptime_s`, `free_heap`, `wifi_rssi`,
`boot_count`, `reported_config`, `command_acks`, `ota_status`, `mesh_peers`,
`timestamp`.

### 8.2 Compatibility Strategy

- The server accepts both v1 and v2 heartbeat formats.
- v1 devices receive basic responses (OTA directive only, no config sync).
- v2 devices receive the full response with config sync and commands.
- The server detects the protocol version by the presence of `reported_config`.
- Device firmware detects server version from the response format and adjusts.

---

## 9. ESP32 Implementation Notes

### 9.1 NVS Storage Layout

| Key | Namespace | Description |
|---|---|---|
| `device_token` | `ota` | JWT from provisioning |
| `boot_count` | `ota` | Persistent boot counter |
| `cmd_acks` | `ota` | Pending command acknowledgments |
| WiFi credentials | `wifi` | Managed by hal_wifi |

The `reported_config` is not persisted -- it is rebuilt from HAL initialization
state on each boot.

### 9.2 Memory Budget

| Component | Allocation |
|---|---|
| Heartbeat request JSON | ~1 KB |
| Heartbeat response JSON | ~2 KB max |
| ArduinoJson document | 2 KB stack-allocated |
| HTTP client buffer | 4 KB |
| TLS overhead | ~40 KB heap (mbedTLS) |

Config parsing is performed in-place with no dynamic heap allocation beyond
the ArduinoJson document.

### 9.3 Timing and Retry

```
Normal:    |---60s---|---60s---|---60s---|
                                         Connection failure:
Backoff:   |---60s---|--120s--|---240s---|---600s (max)---|
                                         Success:
Recovery:  |---60s---|---60s---|
```

- Default heartbeat interval: 60 seconds.
- Server can override the interval via `heartbeat_interval_s` in the response.
- On connection failure: exponential backoff from 60s to max 600s.
- On recovery: immediately revert to the configured interval.
- Immediate heartbeat triggered after config application or command execution.

### 9.4 Task Architecture

The heartbeat runs as a dedicated FreeRTOS task to avoid blocking the main
application loop:

```
+------------------+     +-------------------+     +------------------+
| Main App Task    |     | Heartbeat Task    |     | OTA Task         |
| (loop + display) |     | (HTTP + config)   |     | (download + flash)|
| Core 1           |     | Core 0            |     | Core 0           |
+------------------+     +-------------------+     +------------------+
        ^                        |                        ^
        |  config changes        |  ota directive         |
        +------------------------+------------------------+
                          Shared state (mutex-protected)
```

---

## Appendix A: Supported Board Capabilities

| Board | camera | audio | imu | rtc | pmic | mesh | touch |
|---|---|---|---|---|---|---|---|
| touch-amoled-241b | no | no | no | no | no | yes | yes |
| amoled-191m | no | no | no | no | no | yes | no |
| touch-amoled-18 | no | no | no | no | no | yes | yes |
| touch-lcd-35bc | yes | yes | yes | yes | yes | yes | yes |
| touch-lcd-43c-box | no | no | no | no | no | yes | yes |
| touch-lcd-349 | no | no | no | no | no | yes | yes |

These capabilities are reported during provisioning and determine which
modules the server can enable for each device.

## Appendix B: Serial Commands

Direct serial interface for diagnostics and testing. Send via USB serial (115200 baud).

| Command | Args | Description |
|---|---|---|
| `IDENTIFY` | — | Returns device ID, board, firmware version, MAC |
| `SD_FORMAT` | — | Formats SD card (if present) |
| `BLE_LIST` | — | Lists known BLE devices |
| `BLE_ADD` | `<name> <mac>` | Adds a named BLE device to tracking list |
| `DIAG` | — | Prints diagnostic snapshot |
| `HEALTH` | — | Prints health JSON |
| `ANOMALIES` | — | Prints detected anomalies |
| `MODEM_SEND` | `<hex>` | Transmit hex data via acoustic modem (FSK over speaker) |
| `MODEM_LISTEN` | `[timeout_ms]` | Listen for acoustic data (default 5000ms) |
| `MODEM_STATS` | — | Print modem stats (frames, CRC errors, SNR) |
| `MODEM_TEST` | — | Send "TRITIUM" and listen for echo |
| `SEED_STATUS` | — | Print self-seed system status |
| `SEED_LIST` | — | List files in seed manifest |
| `SEED_CREATE` | — | Create/update seed manifest from current firmware |
| `COT_SEND` | — | Send CoT SA event via UDP multicast |
| `COT_POS` | `<lat> <lon> [hae]` | Set device position for CoT |
| `COT_STATUS` | — | Print CoT subsystem status |

Commands requiring specific hardware: MODEM_* (HAS_AUDIO_CODEC), SEED_* (HAS_SDCARD),
COT_* (ENABLE_COT build flag + WiFi connected).

## Appendix C: Error Codes

| Code | Context | Description |
|---|---|---|
| `token_expired` | 401 response | Device token JWT has expired |
| `token_invalid` | 401 response | Token signature verification failed |
| `org_not_found` | 401 response | Organization deleted or deactivated |
| `rate_limited` | 429 response | Too many heartbeats, back off |
| `provision_pending` | Provision response | Admin approval required |
| `hash_mismatch` | Attestation | Firmware hash does not match expected |
| `ota_failed` | OTA status | Firmware download or verification failed |
| `cmd_timeout` | Command lifecycle | Command not acknowledged within TTL |
