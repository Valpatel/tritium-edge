# OTA Application Requirements and Features

## Overview

The OTA application (`OtaApp`) is the primary user-facing firmware for managing over-the-air updates on ESP32-S3 devices. It integrates all OTA delivery pathways into a single application with a display-based status UI, serial command interface, fleet server heartbeat, and peer-to-peer firmware distribution. The app initializes and orchestrates OtaHAL (core OTA operations), OtaMesh (robust chunked mesh transfer), OtaAuditLog (persistent event logging), BleOtaHAL (NimBLE firmware transfer), EspNowHAL (peer-to-peer mesh), SDCardHAL (SD card access), WifiManager (multi-network WiFi), and ProvisionHAL (device identity and provisioning).

On startup, the app initializes all subsystems, checks the SD card for firmware files, enables BLE advertising and ESP-NOW mesh receive, and begins periodic fleet heartbeats. It supports simultaneous listening on all OTA pathways with mutual exclusion ensuring only one active transfer at a time.

## Architecture

```
                        +------------------+
                        |  Fleet Server    |
                        |  (REST API)      |
                        +--------+---------+
                                 |
                    HTTP POST    | /api/devices/{id}/status
                                 |
              +------------------+------------------+
              |                  |                  |
         WiFi Pull          Heartbeat          Server-
         (OTA_URL)         (30s interval)      Scheduled OTA
              |                  |                  |
              v                  v                  v
         +--------------------------------------------+
         |              OtaApp (ota_app.h/.cpp)       |
         |                                            |
         |  +----------+  +-----------+  +---------+  |
         |  | OtaHAL   |  | OtaMesh   |  | BleOta  |  |
         |  | (core)   |  | (ESP-NOW) |  | (NimBLE)|  |
         |  +----------+  +-----------+  +---------+  |
         |  +----------+  +-----------+  +---------+  |
         |  |OtaAudit  |  | OtaVerify |  |SDCardHAL|  |
         |  | (log)    |  | (crypto)  |  | (SD/USB)|  |
         |  +----------+  +-----------+  +---------+  |
         |  +----------+  +-----------+               |
         |  |Provision |  |WifiManager|               |
         |  | (identity)|  | (NVS)    |               |
         |  +----------+  +-----------+               |
         +---+-------+-------+-------+-------+-------+
             |       |       |       |       |
          Serial   SD Card  BLE    ESP-NOW  USB MSC
          (USB)    (.ota)  (NimBLE) (P2P)   (drag&drop)
```

### Component Responsibilities

| Component | Role |
|-----------|------|
| `OtaHAL` | Core OTA operations: partition management, WiFi pull/push, SD card flash, rollback, firmware hash, app confirmation |
| `OtaMesh` | Robust chunked P2P firmware distribution over ESP-NOW with sliding window flow control, retransmit, session management |
| `OtaAuditLog` | Persistent JSON-line log on LittleFS recording every OTA attempt with source, version, board, success/failure, error detail |
| `BleOtaHAL` | NimBLE OTA service with data+control characteristics, MTU 517 write-no-response, progress callbacks |
| `OtaVerify` | ECDSA P-256 signature verification (one-shot and streaming), AES-256-CTR decryption, CRC32 streaming |
| `EspNowHAL` | ESP-NOW mesh networking: broadcast, unicast, multi-hop relay, peer discovery |
| `SDCardHAL` | SD card init, file operations, USB Mass Storage mode via TinyUSB |
| `WifiManager` | Multi-network WiFi with NVS persistence, auto-reconnect |
| `ProvisionHAL` | Device identity, USB provisioning mode, factory reset |

## Initialization Sequence

1. Create PSRAM-backed display sprite for status UI
2. Init `OtaHAL` -- partition info, version, max firmware size
3. Record app confirmation timer (boot loop protection, delayed 30s)
4. Init `ProvisionHAL` -- load device identity from NVS
5. Init `WifiManager` -- auto-connect to saved networks
6. Init `SDCardHAL` -- check for `/firmware.ota` or `/firmware.bin`
7. Init `EspNowHAL` -- register mesh receive callback for P2P OTA messages
8. If firmware file found on SD, attempt immediate SD card OTA flash
9. Init `BleOtaHAL` -- start NimBLE advertising as "ESP32-OTA"
10. Init `OtaMesh` -- enable auto-receive, register progress and result callbacks
11. Init `OtaAuditLog` -- load persistent log from LittleFS
12. Print `OTA_READY` to serial, signaling readiness

## Main Loop

The loop executes the following in order each iteration:

1. **USB Provisioning mode** -- If active, delegate serial to `ProvisionHAL.processUSBProvision()` and skip all other processing. Still redraws display status.
2. **Delayed app confirmation** -- After 30 seconds of stable operation, call `confirmApp()` to mark the current firmware as good (boot loop protection).
3. **Serial command handling** -- Process serial input for commands or binary OTA data.
4. **BLE OTA processing** -- Poll `BleOtaHAL.process()` for incoming BLE data.
5. **ESP-NOW processing** -- Poll `EspNowHAL.process()` and `OtaMesh.process()`. Periodically offer firmware to mesh peers (every 5s). Check P2P transfer timeout (10s).
6. **Fleet heartbeat** -- If provisioned and WiFi connected, send heartbeat every 30 seconds.
7. **Display refresh** -- Redraw status UI every 250ms.

Note: During serial binary receive mode (`SerialOtaState::RECEIVING`), steps 4-7 are skipped to prevent debug output from corrupting the binary data stream.

## Serial Command Reference

All commands are newline-terminated ASCII. Responses are newline-terminated. JSON responses are single-line.

### Device Information

| Command | Response | Description |
|---------|----------|-------------|
| `IDENTIFY` | `{"board":"...","display":"WxH","interface":"...","app":"OTA","provisioned":bool,"device_id":"..."}` | Return device identity JSON including board type, display dimensions, interface type, provisioning status, and device ID |
| `OTA_INFO` | `{"version":"...","partition":"...","max_size":N,"sd":bool,"usb_msc":bool,"wifi":bool,"espnow":bool,"ble":bool,"board":"...","signing":bool,"sig_required":bool,"encryption":bool,"mesh_ota":{...},"fw_hash":"...","audit_entries":N,"rollback":bool}` | Comprehensive OTA status JSON with all capabilities, mesh OTA statistics, firmware hash, audit entry count, and rollback availability |

### Firmware Update Commands

| Command | Response | Description |
|---------|----------|-------------|
| `OTA_SD` | `OTA_SD_START` then `OTA_SD_OK` or `OTA_SD_FAIL <reason>` | Trigger SD card OTA update. Re-initializes SD if needed. Looks for `/firmware.ota` (preferred) or `/firmware.bin`. Reboots on success. |
| `OTA_URL <url>` | `OTA_URL_START <url>` then `OTA_URL_OK` or `OTA_URL_FAIL <reason>` | WiFi pull OTA from HTTP/HTTPS URL. Requires WiFi connection. URL must be at least 10 characters. Subject to rate limiting. Reboots on success. |
| `OTA_ROLLBACK` | `OTA_ROLLBACK_OK` or `OTA_ROLLBACK_FAIL <reason>` | Roll back to previous firmware partition. Reboots on success. |
| `OTA_REBOOT` | `OTA_REBOOTING` | Immediate device reboot after 200ms delay. |

### Serial Binary OTA Protocol

| Command | Response | Description |
|---------|----------|-------------|
| `OTA_BEGIN <size> <crc32>` | `OTA_READY` or `OTA_FAIL <reason>` | Start serial binary OTA transfer. `size` is firmware bytes, `crc32` is expected CRC32. Device enters binary receive mode. Claims mutual exclusion lock. |
| `OTA_SIG <r_hex> <s_hex>` | `OTA_SIG_OK` or `OTA_SIG_FAIL <reason>` | Provide ECDSA P-256 signature before `OTA_BEGIN`. `r_hex` and `s_hex` are 64-character hex strings (32 bytes each). **Must be sent before `OTA_BEGIN`** because the device enters binary mode after BEGIN. |

**Binary transfer flow after `OTA_READY`:**

1. PC sends raw binary data in chunks (device reads up to 4096 bytes at a time, matching flash page size)
2. Device writes chunk to flash, responds `OTA_NEXT <total_received>`
3. Repeat until all bytes received
4. Device verifies CRC32, then ECDSA signature if provided
5. Device responds `OTA_OK` (success, reboots after 1s) or `OTA_FAIL <reason>`

Timeout: 10 seconds of no data aborts the transfer.

### SD Card Write

| Command | Response | Description |
|---------|----------|-------------|
| `OTA_SD_WRITE <size> [path]` | `OTA_READY` then `OTA_NEXT <offset>` per chunk, then `OTA_SD_WRITE_OK <size>` or `OTA_FAIL <reason>` | Write firmware to SD card via serial binary transfer. Default path is `/firmware.bin`. Path must be absolute (start with `/`) and must not contain `..` (path traversal protection). Uses same chunked binary flow as serial OTA. |

### Firmware Attestation

| Command | Response | Description |
|---------|----------|-------------|
| `FW_HASH` | `FW_HASH <sha256_hex>` or `FW_HASH_FAIL` | Compute and return SHA-256 hash of the running firmware partition. Used for firmware attestation and integrity verification. |

### Audit Log

| Command | Response | Description |
|---------|----------|-------------|
| `OTA_AUDIT` | JSON-line log entries or `OTA_AUDIT_EMPTY` | Read the persistent OTA audit log from LittleFS. Each line is a JSON object with source, version, board, success/failure, error detail, and timestamp. Max 2048 bytes returned. |
| `OTA_AUDIT_CLEAR` | `OTA_AUDIT_CLEAR_OK` | Clear the OTA audit log. |

### Mesh OTA Commands

| Command | Response | Description |
|---------|----------|-------------|
| `MESH_OTA_SEND` | `MESH_OTA_SEND_OK` or `MESH_OTA_FAIL <reason>` | Start sending `/firmware.ota` (or `.bin`) from SD card to mesh peers via OtaMesh. Requires ESP-NOW initialized and SD card available. |
| `MESH_OTA_RECV` | `MESH_OTA_RECV_OK` or `MESH_OTA_FAIL <reason>` | Enable receiving firmware from mesh peers. Requires ESP-NOW initialized. |
| `MESH_OTA_STOP` | `MESH_OTA_STOP_OK` | Stop both mesh OTA send and receive. |
| `MESH_OTA_STATUS` | `{"sending":bool,"receiving":bool,"transfer":bool,"chunks_sent":N,"chunks_received":N,"retransmits":N,"completed":N,"failed":N}` | Get mesh OTA transfer statistics including send/receive state, active transfer, chunk counts, retransmit count, and completed/failed tallies. |

### USB Mass Storage

| Command | Response | Description |
|---------|----------|-------------|
| `USB_MSC` | `USB_MSC_ON`, `USB_MSC_OFF`, or `USB_MSC_FAIL` | Toggle USB Mass Storage mode. When active, the SD card appears as a USB thumb drive for drag-and-drop firmware deployment. Re-initializes SD if needed. |

### Verification Self-Test

| Command | Response | Description |
|---------|----------|-------------|
| `VERIFY_TEST` | `VERIFY_TEST direct: PASS/FAIL` and `VERIFY_TEST streaming: PASS/FAIL` | Run ECDSA P-256 verification self-test using known test vectors. Tests both one-shot and streaming verification paths. |

### WiFi Management

| Command | Response | Description |
|---------|----------|-------------|
| `WIFI_STATUS` | `{"connected":bool,"ssid":"...","ip":"...","rssi":N}` | Get current WiFi connection status including SSID, IP address, and signal strength. |
| `WIFI_ADD <ssid> <password>` | `WIFI_ADD_OK <ssid>` or `WIFI_ADD_FAIL [reason]` | Add a WiFi network to NVS storage. SSID is the first token after the command, password is the remainder. Auto-triggers reconnect. |
| `WIFI_CONNECT` | `WIFI_CONNECT_OK` | Trigger WiFi reconnect to saved networks. |

### Device Provisioning

| Command | Response | Description |
|---------|----------|-------------|
| `PROVISION_BEGIN` | `PROVISION_READY` | Enter USB provisioning mode. After this response, the ProvisionHAL takes ownership of the serial port. All other serial commands are suspended until provisioning completes. The main loop delegates to `processUSBProvision()`. |
| `PROVISION_STATUS` | `{"provisioned":bool,"device_id":"...","server_url":"..."}` | Get current provisioning status including device ID and fleet server URL. |
| `PROVISION_RESET` | `PROVISION_RESET_OK` or `PROVISION_RESET_FAIL` | Factory reset provisioning data. Clears device identity, server URL, and all provisioning state from NVS. |

## Fleet Server Heartbeat Protocol

When the device is both provisioned (has a device identity) and WiFi-connected, it sends an HTTP POST heartbeat every 30 seconds.

### Request

- **Endpoint**: `POST {server_url}/api/devices/{device_id}/status`
- **Content-Type**: `application/json`
- **Timeout**: 5 seconds

**Payload**:
```json
{
    "version": "1.2.3",
    "board": "AXS15231B",
    "partition": "ota_0",
    "ip": "192.168.1.100",
    "uptime_s": 3600,
    "free_heap": 245760,
    "rssi": -55,
    "fw_hash": "a1b2c3d4..."
}
```

| Field | Type | Description |
|-------|------|-------------|
| `version` | string | Current firmware version from OtaHAL |
| `board` | string | Board display driver identifier (`DISPLAY_DRIVER` compile-time define) |
| `partition` | string | Running OTA partition name (e.g., `ota_0`, `ota_1`) |
| `ip` | string | Device's local IP address |
| `uptime_s` | integer | Seconds since boot |
| `free_heap` | integer | Free heap memory in bytes |
| `rssi` | integer | WiFi signal strength in dBm |
| `fw_hash` | string | SHA-256 hash of running firmware partition (64 hex characters) |

### Response Handling

- **HTTP 200**: Check response body for server-scheduled OTA. If the response contains both `"ota"` and `"url"` keys, the app extracts the URL path and prepends the server base URL, then triggers a WiFi pull OTA via `updateFromUrl()`.
- **Other status codes**: Logged at debug level, no action taken.

### Firmware Attestation via Heartbeat

The `fw_hash` field enables server-side firmware attestation. The fleet server can:
- Compare the reported hash against known firmware builds
- Flag devices with unknown hashes as potentially compromised
- Track firmware deployment across the fleet
- Detect unauthorized firmware modifications

## WiFi Push OTA (HTTP Server)

WiFi push OTA is provided by the underlying `OtaHAL` and `hal_webserver` libraries. When WiFi is connected, the device runs an HTTP server (default port 8080) with:
- Upload page at `/update` with dark-themed UI and AJAX progress bar
- Accepts both raw `.bin` and `.ota` files with header verification
- The OTA app does not directly manage the HTTP server; it is initialized by OtaHAL

## BLE OTA Integration

The app initializes `BleOtaHAL` with the advertising name "ESP32-OTA". Integration details:

- **Advertising**: Starts on init, device is discoverable as "ESP32-OTA"
- **Progress callback**: Updates the app's progress bar and status line with percentage, bytes received, and total size
- **Processing**: `_ble_ota.process()` is called each loop iteration to handle incoming BLE data
- **Connection state**: Displayed on the status UI (connected/disconnected)
- **MTU**: 517 bytes for fast write-no-response transfers
- **Signature verification**: Deferred until full transfer completes (handled by BleOtaHAL internally)

## Mesh OTA Integration

### OtaMesh (Robust Chunked Protocol)

The `OtaMesh` subsystem provides robust firmware distribution with:
- Session-based protocol (ANNOUNCE -> REQUEST -> CHUNK -> ACK -> STATUS)
- 200-byte chunks with 4-chunk sliding window
- Automatic retransmit on timeout (max 5 retries)
- Multi-hop relay through ESP-NOW mesh
- Support for signed and encrypted firmware
- Board target validation

**App-level integration:**
- Auto-receive enabled on startup (`_mesh_ota.enableReceive(true)`)
- Progress callback updates app status line
- Result callback: on success, reboots after 2 seconds; on failure, updates status
- Controlled via `MESH_OTA_SEND`, `MESH_OTA_RECV`, `MESH_OTA_STOP`, `MESH_OTA_STATUS` serial commands

### ESP-NOW P2P OTA (Legacy Protocol)

A simpler P2P protocol runs alongside OtaMesh using raw ESP-NOW messages:

**Message Types:**

| Type | Value | Payload | Description |
|------|-------|---------|-------------|
| `OFFER` | `0x10` | `OtaOffer` (36 bytes) | Broadcast firmware availability: size, CRC32, chunk count, version, signed flag |
| `REQUEST` | `0x11` | `OtaChunkRequest` (3 bytes) | Request specific chunk by index |
| `CHUNK` | `0x12` | `OtaChunk` header + up to 220 bytes data | Deliver requested chunk |
| `DONE` | `0x13` | 1 byte | Transfer complete acknowledgment |
| `ABORT` | `0x14` | 1 byte | Abort transfer |
| `SIG` | `0x15` | `OtaSigMsg` (65 bytes) | ECDSA P-256 signature (r[32] + s[32]) |

**Sender behavior:**
- If firmware file exists on SD and device is idle, broadcasts `OFFER` every 5 seconds
- Reads firmware from SD (skipping OTA header), sends requested chunks
- Includes ECDSA signature broadcast if firmware is signed

**Receiver behavior:**
- Accepts `OFFER` if idle and offered version differs from current
- If `OTA_REQUIRE_SIGNATURE` is set, rejects unsigned offers
- For signed firmware: waits for `SIG` message before requesting chunks
- Requests chunks sequentially (one at a time)
- Streams CRC32 and SHA-256 verification during receive
- After all chunks: verifies CRC32, then ECDSA signature if signed
- Reboots on success; sends `ABORT` on verification failure

**Timeout**: 10 seconds of inactivity aborts the transfer.

## OTA Header Validation

The app validates OTA firmware headers (`OtaFirmwareHeader`, 64 bytes) before applying updates:

1. **Magic and version check**: Header must have valid magic number (`0x4154304F`) and supported header version
2. **Board target validation**: If header specifies a board (not empty, not "any"), it is logged but loosely matched (display driver names vary across boards)
3. **Size validation**: Firmware size must not exceed `getMaxFirmwareSize()` (partition size limit)
4. **Anti-rollback** (when `-DOTA_ANTI_ROLLBACK` is defined): Rejects firmware with a semver version lower than the currently running version. Same version is allowed (re-flash).

## Boot Loop Protection

The app implements delayed app confirmation to detect boot loops:

1. On startup, `_app_confirm_timer` is set to the current time. The firmware is **not** immediately confirmed as stable.
2. In the main loop, after **30 seconds** of continuous stable operation (`APP_CONFIRM_DELAY_MS`), `OtaHAL::confirmApp()` is called to mark the current firmware as good.
3. If the device crashes or reboots within the 30-second window, ESP-IDF's OTA rollback mechanism automatically reverts to the previous firmware partition on the next boot.
4. This protects against firmware that boots successfully but crashes shortly after -- a common failure mode in OTA updates.

## Rate Limiting

OTA attempts are rate-limited to prevent flash wear from rapid retries:

| Parameter | Value | Description |
|-----------|-------|-------------|
| `OTA_COOLDOWN_MS` | 5,000 ms | Minimum interval between OTA attempts |
| `OTA_FAIL_COOLDOWN_MS` | 30,000 ms | Extended cooldown after max failures reached |
| `OTA_MAX_FAILS` | 3 | Consecutive failure count before extended cooldown |

Behavior:
- Each OTA attempt (SD, URL, serial) checks `_otaRateLimitOk()` first
- If within the cooldown window, the attempt is rejected with a "rate limited" message
- Successful OTA resets the failure counter to zero
- Failed OTA increments the failure counter
- After 3 consecutive failures, the cooldown extends to 30 seconds

## Mutual Exclusion

Only one OTA source can be active at a time. The app tracks the active source via `_ota_source`:

| Source | Enum | Description |
|--------|------|-------------|
| None | `NONE` | No active OTA |
| Serial | `SRC_SERIAL` | USB serial binary transfer |
| BLE | `SRC_BLE` | Bluetooth Low Energy transfer |
| P2P | `SRC_P2P` | ESP-NOW peer-to-peer transfer |
| SD | `SRC_SD` | SD card flash |

- `_claimOta(source)`: Acquires the lock. Returns `false` if another source is active.
- `_releaseOta()`: Releases the lock, setting source back to `NONE`.
- All OTA pathways call `_claimOta()` before starting and `_releaseOta()` on completion or error.

## Audit Logging Integration

Every OTA attempt is logged to the persistent audit log via `OtaAuditLog`:

- **SD OTA**: Logs with source `"sd"`, current version on success, `"?"` on failure
- **WiFi pull OTA**: Logs with source `"wifi_pull"`, current version on success
- **Other pathways**: Handled by their respective HALs

Each log entry records:
- Source (sd, wifi_pull, serial, ble, mesh, etc.)
- Firmware version (or "?" if unknown)
- Target board identifier (`DISPLAY_DRIVER`)
- Success/failure boolean
- Error detail string (on failure)
- Timestamp

The audit log is stored as JSON lines on LittleFS (`/ota_audit.log`), auto-trimmed at 8KB.

## Display Status UI

The app renders a status dashboard on the device display using a PSRAM-backed sprite, refreshed every 250ms:

- **Title**: "OTA Update" in green
- **Version and partition info**: Current firmware version, partition name, max firmware size
- **Capability indicators**: Color-coded (green=OK, red=N/A) for SD, ESP-NOW (with peer count), BLE (with connection state), WiFi (with SSID and IP), Serial
- **Firmware file indicator**: Yellow text if `firmware.bin` or `firmware.ota` found on SD
- **Status line**: Current operation status (e.g., "Ready. Waiting for OTA...", "Serial: receiving 245760 bytes", "P2P: 42/100 chunks")
- **Progress bar**: Shown when an OTA transfer is active, with percentage label
- **Instructions**: Quick-reference for available OTA methods (at bottom, space permitting)

The UI adapts to narrow displays (width < 200px, e.g., the 172px-wide 3.49 board) with smaller text sizes, abbreviated labels, and truncated status lines.

## Build Environments

Each board has a dedicated OTA PlatformIO environment using the OTA partition table:

| Environment | Board |
|-------------|-------|
| `touch-lcd-349-ota` | ESP32-S3-Touch-LCD-3.49 |
| `touch-lcd-35bc-ota` | ESP32-S3-Touch-LCD-3.5B-C |
| `touch-amoled-241b-ota` | ESP32-S3-Touch-AMOLED-2.41-B |
| `amoled-191m-ota` | ESP32-S3-AMOLED-1.91-M |
| `touch-amoled-18-ota` | ESP32-S3-Touch-AMOLED-1.8 |
| `touch-lcd-43c-box-ota` | ESP32-S3-Touch-LCD-4.3C-BOX |

All OTA environments use `partitions_ota_16MB.csv` with dual OTA partitions for A/B update support.

Build and flash:
```bash
make build BOARD=touch-lcd-35bc APP=ota
make flash BOARD=touch-lcd-35bc APP=ota
```

Or directly via PlatformIO:
```bash
pio run -e touch-lcd-35bc-ota
```

## File Structure

```
apps/ota/
  ota_app.h             -- OtaApp class declaration, ESP-NOW protocol structs,
                           serial protocol comments
  ota_app.cpp           -- Full application implementation: setup, loop, serial
                           command handler, fleet heartbeat, P2P OTA, display UI
  REQUIREMENTS.md       -- This file
```

### Dependencies

```
lib/hal_ota/
  hal_ota.h / .cpp          -- Core OTA HAL
  ota_header.h              -- OTA firmware header struct + semver compare
  ota_verify.h / .cpp       -- ECDSA + AES + CRC32
  ota_mesh.h / .cpp         -- ESP-NOW mesh OTA protocol
  ota_audit.h / .cpp        -- Persistent audit log

lib/hal_ble_ota/
  hal_ble_ota.h / .cpp      -- NimBLE OTA service

lib/hal_espnow/
  hal_espnow.h / .cpp       -- ESP-NOW mesh networking

lib/hal_sdcard/
  hal_sdcard.h / .cpp       -- SD card + USB MSC

lib/hal_wifi/
  wifi_manager.h / .cpp     -- Multi-network WiFi

lib/hal_provision/
  hal_provision.h / .cpp    -- Device provisioning
```

## Security Summary

| Feature | Implementation |
|---------|---------------|
| Cryptographic signing | ECDSA P-256 via mbedtls, all pathways |
| Signature enforcement | `-DOTA_REQUIRE_SIGNATURE` build flag rejects unsigned firmware |
| Firmware encryption | AES-256-CTR, conditional compilation via `__has_include` |
| CRC32 integrity | Streaming verification on every transfer path |
| Firmware attestation | SHA-256 hash of running partition in fleet heartbeat |
| Anti-rollback | `-DOTA_ANTI_ROLLBACK` build flag, semver comparison |
| Boot loop protection | 30-second delayed app confirmation with ESP-IDF auto-rollback |
| Rate limiting | 5s cooldown, 30s after 3 failures, prevents flash wear |
| Mutual exclusion | Single-source lock prevents concurrent OTA from different pathways |
| Path traversal protection | `OTA_SD_WRITE` rejects paths containing `..` |
| Minimum size check | 32KB minimum firmware size (in OtaHAL) |
| Download stall timeout | 10s serial timeout, 30s HTTP timeout |
