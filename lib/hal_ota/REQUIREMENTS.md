# OTA System Requirements and Features

## Overview

The OTA (Over-The-Air) firmware update system provides secure, verified firmware distribution across 7 delivery pathways, with cryptographic signing, encryption, attestation, and fleet-scale management.

## Architecture

```
                        +------------------+
                        |  Fleet Server    |
                        |  (FastAPI v2.2)  |
                        +--------+---------+
                                 |
                    HTTPS/TLS    | REST API
                                 |
              +------------------+------------------+
              |                  |                  |
         WiFi Pull          WiFi Push          Heartbeat
         (OTA_URL)         (HTTP /update)     (attestation)
              |                  |                  |
              v                  v                  v
         +--------------------------------------------+
         |              ESP32-S3 Device               |
         |                                            |
         |  OtaHAL  <-- validates header, CRC, sig   |
         |  OtaVerify <-- ECDSA P-256, AES-256-CTR   |
         |  OtaMesh  <-- P2P firmware distribution    |
         |  OtaAuditLog <-- persistent event log      |
         |                                            |
         +---+-------+-------+-------+-------+-------+
             |       |       |       |       |
          Serial   SD Card  BLE    ESP-NOW  USB MSC
          (USB)    (.ota)  (NimBLE) (Mesh)  (drag&drop)
```

## Firmware Image Format

### OTA Header (64 bytes, v1 unsigned)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | `0x4154304F` ('OT0A' LE) |
| 4 | 2 | header_version | 1=unsigned, 2=signed |
| 6 | 2 | flags | Bit0: signed, Bit1: encrypted |
| 8 | 4 | firmware_size | Payload size after header |
| 12 | 4 | firmware_crc32 | CRC32 of payload |
| 16 | 24 | version | Semver string, null-terminated |
| 40 | 16 | board | Target board ID, null-terminated |
| 56 | 4 | build_timestamp | Unix epoch (LE), set by packaging tool |
| 60 | 4 | reserved | Zero-filled |

### OTA Signature Block (64 bytes, appended for v2)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 64 | 32 | signature_r | ECDSA P-256 r component (big-endian) |
| 96 | 32 | signature_s | ECDSA P-256 s component (big-endian) |

Signature covers: **firmware data only** (not header). Header integrity is protected by CRC32.

### Encrypted Payload Format

When FLAG_ENCRYPTED is set, the firmware payload is:
```
[IV: 16 bytes][ciphertext: N bytes]
```
- `firmware_size` = 16 (IV) + N (ciphertext)
- Algorithm: AES-256-CTR
- Pattern: Encrypt-then-sign (CRC32 and signature cover the ciphertext+IV)
- Decryption happens in-place after CRC/signature verification passes

## Delivery Pathways

### 1. WiFi Pull OTA
- Device downloads `.ota` file from HTTP/HTTPS URL
- Triggered via serial command `OTA_URL <url>` or fleet server heartbeat response
- Streaming: header parsed from first 128 bytes, firmware streamed to flash
- Stall timeout: 30 seconds
- Status: **Implemented, tested**

### 2. WiFi Push OTA (HTTP Upload)
- HTTP server on device (default port 8080)
- Dark-themed upload page at `/update`
- Supports both raw `.bin` and `.ota` with header verification
- AJAX upload with progress bar
- Status: **Implemented, tested**

### 3. SD Card OTA
- Boot-time check for `/firmware.ota` or `/firmware.bin` on SD
- Also triggered via serial command `OTA_SD`
- After successful flash, file renamed to `.bak`
- Status: **Implemented, tested**

### 4. Serial USB OTA
- Binary protocol over USB CDC serial (115200 baud)
- Commands: `OTA_BEGIN <size> <crc32>`, binary chunks with `OTA_ACK`, `OTA_END`
- Optional: `OTA_SIG <r_hex> <s_hex>` for signed firmware
- PC-side tool: `tools/serial_ota.py`
- Status: **Implemented, tested**

### 5. BLE OTA
- NimBLE service with data + control characteristics
- MTU 517 for fast transfer (write-no-response)
- Deferred ECDSA signature verification after full transfer
- PC-side tool: `tools/ble_ota.py`
- Status: **Implemented, tested**

### 6. ESP-NOW Mesh OTA (`OtaMesh`)
- Peer-to-peer firmware distribution over ESP-NOW mesh network
- Chunked protocol: 200-byte chunks, 4-chunk sliding window
- Session-based: ANNOUNCE -> REQUEST -> CHUNK -> ACK -> STATUS
- Supports multi-hop relay through mesh network
- Supports signed + encrypted firmware
- Board target validation before accepting transfers
- Retransmit on timeout (max 5 retries)
- Serial commands: `MESH_OTA_SEND`, `MESH_OTA_RECV`, `MESH_OTA_STOP`, `MESH_OTA_STATUS`
- Status: **Implemented, compiles, needs hardware testing with 2+ devices**

### 7. USB Mass Storage (Drag & Drop)
- SD card exposed as USB thumb drive via TinyUSB USBMSC
- User copies `.ota` file to SD, device picks it up on next boot
- FAT filesystem unmounted during MSC mode to prevent conflicts
- Serial command: `USB_MSC`
- Status: **Implemented, needs SD card for testing**

## Security Features

### Cryptographic Signing
- Algorithm: ECDSA P-256 (secp256r1) with SHA-256
- Key generation: `tools/generate_signing_key.py`
- Signing: `tools/package_firmware.py --sign <key.pem>`
- Verification: mbedtls on ESP32 (hardware-accelerated SHA-256)
- Both one-shot and streaming verification supported
- Build flag: `-DOTA_REQUIRE_SIGNATURE` to reject unsigned firmware

### Firmware Encryption
- Algorithm: AES-256-CTR
- Key generation: `tools/generate_signing_key.py --encryption`
- Encryption: `tools/package_firmware.py --encrypt <key_file>`
- Decryption: mbedtls AES on ESP32
- Conditional compilation: only linked if `ota_encryption_key.h` exists (`__has_include`)
- Streaming decryption in all update paths

### Board Target Validation
- Header contains target board identifier (e.g., "touch-lcd-349")
- Device compares against compile-time board define
- Rejects firmware intended for a different board
- "any" board target accepted by all devices

### Anti-Rollback Protection
- Build flag: `-DOTA_ANTI_ROLLBACK`
- Semver comparison: `major.minor.patch`
- Rejects firmware with lower version number
- Same version allowed (re-flash)

### Boot Loop Protection
- ESP-IDF OTA rollback with delayed app confirmation
- 30-second stability window after boot
- If device crashes within window, ESP-IDF auto-rolls back on next boot
- `confirmApp()` called after stability period

### Rate Limiting
- 5-second cooldown between OTA attempts
- 30-second cooldown after 3 consecutive failures
- Max 3 failures before extended cooldown
- Prevents flash wear from rapid retry loops

### Firmware Attestation
- SHA-256 hash of running firmware partition
- Reported in fleet heartbeat for server-side verification
- Server compares against known firmware hashes
- Unknown hashes flagged as security events
- Serial command: `FW_HASH`

### OTA Audit Log
- Persistent JSON-line log on LittleFS (`/ota_audit.log`)
- Records: source, version, board, success/failure, error detail, timestamp
- Auto-trimmed at 8KB to prevent storage exhaustion
- Serial commands: `OTA_AUDIT`, `OTA_AUDIT_CLEAR`

### Additional Protections
- Minimum firmware size check (32KB) — rejects truncated/corrupt files
- Download stall timeout (30s) — prevents hung connections
- Mutual exclusion between OTA sources — only one active at a time
- Path traversal protection on SD write commands
- CRC32 integrity check on every path (never skipped)
- Build timestamp in header for replay detection

## Serial Command Reference

| Command | Description |
|---------|-------------|
| `IDENTIFY` | Return device identity JSON |
| `OTA_INFO` | Return OTA status JSON (version, capabilities, mesh stats, hash) |
| `OTA_SD` | Trigger SD card OTA update |
| `OTA_URL <url>` | WiFi pull OTA from URL |
| `OTA_ROLLBACK` | Roll back to previous firmware |
| `OTA_REBOOT` | Reboot device |
| `OTA_BEGIN <size> <crc32>` | Start serial OTA binary transfer |
| `OTA_SIG <r_hex> <s_hex>` | Provide ECDSA signature for serial OTA |
| `OTA_SD_WRITE <size> [path]` | Write firmware to SD card via serial |
| `FW_HASH` | Get SHA-256 hash of running firmware |
| `OTA_AUDIT` | Read OTA audit log |
| `OTA_AUDIT_CLEAR` | Clear OTA audit log |
| `MESH_OTA_SEND` | Send firmware.ota from SD to mesh peers |
| `MESH_OTA_RECV` | Enable receiving firmware from mesh |
| `MESH_OTA_STOP` | Stop mesh OTA send/receive |
| `MESH_OTA_STATUS` | Get mesh OTA transfer statistics |
| `USB_MSC` | Toggle USB Mass Storage mode for SD card |
| `VERIFY_TEST` | Run ECDSA verification self-test |
| `WIFI_STATUS` | Get WiFi connection status |
| `WIFI_ADD <ssid> <pass>` | Add WiFi network to NVS |
| `WIFI_CONNECT` | Reconnect WiFi |
| `PROVISION_BEGIN` | Start USB provisioning mode |
| `PROVISION_STATUS` | Get provisioning status |
| `PROVISION_RESET` | Factory reset provisioning |

## PC-Side Tools

| Tool | Description |
|------|-------------|
| `tools/package_firmware.py` | Package, sign, encrypt, verify .ota files |
| `tools/generate_signing_key.py` | Generate ECDSA P-256 keypair + AES-256 key |
| `tools/serial_ota.py` | Push firmware over serial USB |
| `tools/ble_ota.py` | Push firmware over BLE, scan for devices |
| `tools/test_ota.py` | Comprehensive OTA test suite (9+ tests) |

## Build Environments

Each board has an OTA-specific PlatformIO environment:
- `touch-lcd-349-ota`
- `touch-lcd-35bc-ota`
- `touch-amoled-241b-ota`
- `amoled-191m-ota`
- `touch-amoled-18-ota`
- `touch-lcd-43c-box-ota`

OTA environments use `partitions_ota_16MB.csv` with dual OTA partitions.

## File Structure

```
lib/hal_ota/
  hal_ota.h / .cpp          — Core OTA HAL (WiFi push/pull, SD, rollback, attestation)
  ota_header.h              — OTA firmware header struct + semver compare
  ota_verify.h / .cpp       — ECDSA P-256 verification + AES-256-CTR decryption + CRC32
  ota_mesh.h / .cpp         — ESP-NOW mesh OTA protocol
  ota_audit.h / .cpp        — Persistent OTA audit log on LittleFS
  ota_public_key.h          — Embedded ECDSA public key (generated, not committed)
  ota_encryption_key.h      — Embedded AES-256 key (generated, not committed, optional)
  README.md                 — Usage examples
  REQUIREMENTS.md           — This file

apps/ota/
  ota_app.h / .cpp          — OTA application with all pathways integrated

tools/
  package_firmware.py       — Packaging, signing, encryption, verification
  generate_signing_key.py   — Key generation
  serial_ota.py             — Serial OTA client
  ble_ota.py                — BLE OTA client
  test_ota.py               — Test suite
```
