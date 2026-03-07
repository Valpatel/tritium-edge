# PC-Side OTA Tools Requirements and Reference

## Overview

The `tools/` directory contains Python utilities that form the PC-side complement to the ESP32 OTA firmware update system. Together, these tools handle the full lifecycle of secure firmware distribution: key generation, firmware packaging with cryptographic signing and encryption, delivery over serial USB and BLE, device provisioning, board detection, and end-to-end testing.

All tools target Python 3.8+ and run on Linux (primary), macOS, and Windows.

```
tools/
  generate_signing_key.py   -- ECDSA P-256 keypair + AES-256 encryption key generation
  package_firmware.py       -- Package, sign, encrypt, and verify .ota firmware images
  serial_ota.py             -- Push firmware to device over USB serial (CDC)
  ble_ota.py                -- Push firmware to device over BLE (NimBLE)
  provision_sd.py           -- Generate SD card provisioning contents (certs, identity)
  detect_boards.py          -- Detect connected ESP32-S3 boards via USB
  test_ota.py               -- Comprehensive OTA test suite (11 tests)
  README.md                 -- Brief tool index
  REQUIREMENTS.md           -- This file
```

### Relationship to Firmware

The PC tools mirror the firmware-side OTA system in `lib/hal_ota/`:

| PC Tool | Firmware Component | Function |
|---------|-------------------|----------|
| `generate_signing_key.py` | `ota_public_key.h`, `ota_encryption_key.h` | Key material shared between PC and device |
| `package_firmware.py` | `ota_header.h`, `ota_verify.cpp` | Header format and verification logic |
| `serial_ota.py` | `ota_app.cpp` serial command handler | Serial OTA protocol |
| `ble_ota.py` | `hal_ble_ota.h/.cpp` | BLE OTA service and protocol |
| `provision_sd.py` | `hal_provision.h/.cpp` | SD card provisioning import |
| `detect_boards.py` | `main.cpp` IDENTIFY handler | Board identification |

---

## generate_signing_key.py

Generates cryptographic keys for OTA firmware signing and encryption.

### ECDSA P-256 Signing Keypair

- **Algorithm**: ECDSA with NIST P-256 (secp256r1) curve
- **Private key format**: PKCS#8 PEM, unencrypted, file permissions set to `0o600`
- **Public key format**: Uncompressed X9.62 point (65 bytes: `0x04 || x[32] || y[32]`)
- **C header output**: `ota_public_key.h` with `static const uint8_t OTA_SIGNING_PUBLIC_KEY[65]`
- **Key fingerprint**: SHA-256 of the raw public key bytes, first 16 hex characters displayed

### AES-256 Encryption Key

- **Algorithm**: AES-256 (32-byte random key)
- **Output formats**: Hex text file (`.hex`) and C header (`.h`)
- **File permissions**: Both files set to `0o600`
- **C header output**: `ota_encryption_key.h` with `static const uint8_t OTA_ENCRYPTION_KEY[32]`

### Usage

```bash
# Generate signing keypair only
python3 tools/generate_signing_key.py --output-dir keys/

# Generate signing keypair + encryption key
python3 tools/generate_signing_key.py --output-dir keys/ --encryption
```

### Output Files

| File | Contents | Security |
|------|----------|----------|
| `ota_signing_key.pem` | ECDSA P-256 private key (PKCS#8 PEM) | SECRET -- never commit |
| `ota_public_key.h` | C header with 65-byte public key array | Safe to embed in firmware |
| `ota_encryption_key.hex` | 32-byte AES key as hex string | SECRET -- never commit |
| `ota_encryption_key.h` | C header with 32-byte AES key array | SECRET -- embed in firmware only |

### CLI Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `--output-dir` | `.` | Directory for generated key files |
| `--encryption` | off | Also generate AES-256 encryption key |

### Dependencies

- `cryptography` (for `ec.generate_private_key`, `serialization`)

---

## package_firmware.py

Packages raw `firmware.bin` into `.ota` files with header, optional ECDSA signature, and optional AES-256-CTR encryption. Also verifies existing `.ota` files.

### OTA Header Format (64 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | `magic` | `0x4154304F` (`'OT0A'` little-endian) |
| 4 | 2 | `header_version` | `1` = unsigned, `2` = signed |
| 6 | 2 | `flags` | Bit 0: signed, Bit 1: encrypted |
| 8 | 4 | `firmware_size` | Payload byte count (after header + optional signature block) |
| 12 | 4 | `firmware_crc32` | CRC32 of payload data |
| 16 | 24 | `version` | Semver string, null-padded |
| 40 | 16 | `board` | Target board ID, null-padded (e.g., `touch-lcd-349`) |
| 56 | 4 | `build_timestamp` | Unix epoch (uint32 LE), set at packaging time |
| 60 | 4 | `reserved` | Zero-filled |

### Signature Block (64 bytes, v2 header only)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 64 | 32 | `signature_r` | ECDSA P-256 `r` component (big-endian) |
| 96 | 32 | `signature_s` | ECDSA P-256 `s` component (big-endian) |

The signature covers the **firmware payload only** (not the header). When encryption is enabled, the signature covers the encrypted payload (encrypt-then-sign pattern).

### File Layout

```
Unsigned .ota:  [Header: 64 bytes][Firmware payload]
Signed .ota:    [Header: 64 bytes][Signature: 64 bytes][Firmware payload]
Encrypted:      Payload = [IV: 16 bytes][AES-256-CTR ciphertext]
```

### Encryption

- **Algorithm**: AES-256-CTR with random 16-byte IV
- **Pattern**: Encrypt-then-sign -- CRC32 and signature cover the IV + ciphertext
- **Key file**: 32 bytes as hex text or raw binary
- **IV**: Prepended to ciphertext in the payload

### Packaging

```bash
# Unsigned package (v1 header, 64 bytes overhead)
python3 tools/package_firmware.py firmware.bin \
    --version 1.0.0 --board touch-lcd-349 -o firmware.ota

# Signed package (v2 header, 128 bytes overhead)
python3 tools/package_firmware.py firmware.bin \
    --sign keys/ota_signing_key.pem \
    --version 1.0.0 --board touch-lcd-349 -o firmware.ota

# Signed + encrypted package
python3 tools/package_firmware.py firmware.bin \
    --sign keys/ota_signing_key.pem \
    --encrypt keys/ota_encryption_key.hex \
    --version 1.0.0 --board touch-lcd-349 -o firmware.ota
```

### Verification

```bash
# Verify header + CRC32 only
python3 tools/package_firmware.py firmware.ota --verify

# Verify header + CRC32 + ECDSA signature
python3 tools/package_firmware.py firmware.ota --verify --pubkey keys/ota_public_key.pem

# Verify using the C header public key
python3 tools/package_firmware.py firmware.ota --verify --pubkey include/ota_public_key.h

# Verify using the private key (extracts public key automatically)
python3 tools/package_firmware.py firmware.ota --verify --pubkey keys/ota_signing_key.pem
```

The `--pubkey` argument accepts three formats:
1. PEM public key file (`BEGIN PUBLIC KEY`)
2. PEM private key file (`BEGIN ... PRIVATE KEY`) -- public key is extracted
3. C header file (`.h`) -- hex bytes are parsed from `0xNN` patterns

### CLI Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `firmware` | (required) | Input `.bin` file, or `.ota` file when using `--verify` |
| `-v`, `--version` | `0.0.0` | Firmware version string (max 23 chars) |
| `-b`, `--board` | `any` | Target board identifier (max 15 chars) |
| `-o`, `--output` | auto | Output `.ota` path (defaults to input with `.ota` extension) |
| `--sign` | none | ECDSA P-256 private key PEM file for signing |
| `--encrypt` | none | AES-256 key file (32 bytes hex or raw) for encryption |
| `--verify` | off | Verify mode: validate an existing `.ota` file |
| `--pubkey` | none | Public key for signature verification (PEM, private PEM, or `.h`) |

### Build Timestamp

The `build_timestamp` field is automatically set to `time.time()` (UTC) at packaging time. During verification, timestamps after 2020-09-13 (epoch > 1600000000) are displayed as human-readable UTC strings. This field supports replay detection on the device side.

### Dependencies

- `cryptography` (only when `--sign` or `--encrypt` or `--verify --pubkey` is used)
- Standard library: `struct`, `binascii`, `hashlib`, `time`, `argparse`

---

## serial_ota.py

Pushes firmware to a connected ESP32 device over USB CDC serial. Supports both raw `.bin` files and signed `.ota` packages. Also supports writing firmware to the device's SD card.

### Protocol

The serial OTA protocol is text-command-initiated with binary data transfer and flow control:

```
Phase 1: Device Query (optional)
  PC -> Device:  OTA_INFO\n
  Device -> PC:  {"version":"...", ...}\n

Phase 2: Signature (signed .ota only)
  PC -> Device:  OTA_SIG <r_hex_64chars> <s_hex_64chars>\n
  Device -> PC:  OTA_SIG_OK\n

Phase 3: Begin Transfer
  PC -> Device:  OTA_BEGIN <size_decimal> <crc32_decimal>\n
  Device -> PC:  OTA_READY\n

Phase 4: Binary Transfer (flow-controlled)
  PC -> Device:  <4096 bytes in 64-byte USB CDC pieces>
  Device -> PC:  OTA_NEXT <offset>\n
  (repeat until all bytes sent)

Phase 5: Verification
  Device -> PC:  OTA_OK\n      (success, device reboots)
  Device -> PC:  OTA_FAIL <reason>\n  (failure)
```

### Flow Control

- **Flow chunk**: 4096 bytes (matches flash page size)
- **USB piece**: 64 bytes (USB CDC max packet)
- **Inter-piece delay**: 1ms (prevents USB buffer overflow)
- **ACK timeout**: 10 seconds per chunk
- **Verification timeout**: 30 seconds (allows time for ECDSA verification)

### .ota File Auto-Detection

The tool automatically detects `.ota` files by checking for the magic bytes `0x4154304F`. When an `.ota` file is detected:
1. Header is parsed for version, board, signature presence
2. Signature is extracted and sent via `OTA_SIG` command before `OTA_BEGIN`
3. Only the firmware payload (after header + signature block) is transferred

For raw `.bin` files, CRC32 is computed on the fly and the entire file is transferred.

### SD Card Write Mode

The `--sd` flag switches to SD card write mode, which writes firmware to the device's SD card for boot-time OTA:

```
PC -> Device:  OTA_SD_WRITE <size> <sd_path>\n
Device -> PC:  OTA_READY\n
(binary transfer with OTA_NEXT flow control)
Device -> PC:  OTA_SD_WRITE_OK\n
```

The SD filename defaults to `/firmware.ota` or `/firmware.bin` based on the input file extension.

### Device Reconnection

After a successful OTA, the device reboots. If the serial connection drops (`SerialException`), the tool waits 5 seconds and attempts to reconnect and send `IDENTIFY` to confirm the device is running the new firmware.

### Usage

```bash
# Upload raw .bin
python3 tools/serial_ota.py /dev/ttyACM0 firmware.bin

# Upload signed .ota package
python3 tools/serial_ota.py /dev/ttyACM0 firmware.ota

# Write firmware to SD card for boot-time OTA
python3 tools/serial_ota.py /dev/ttyACM0 firmware.ota --sd

# Custom baud rate
python3 tools/serial_ota.py /dev/ttyACM0 firmware.bin -b 921600
```

### CLI Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `port` | (required) | Serial port path (e.g., `/dev/ttyACM0`) |
| `firmware` | (required) | Path to `.bin` or `.ota` file |
| `-b`, `--baud` | `115200` | Baud rate |
| `--sd` | off | Write to device SD card instead of direct OTA |

### Dependencies

- `pyserial` (`serial.Serial`)
- Standard library: `struct`, `binascii`, `time`, `argparse`

---

## ble_ota.py

Pushes firmware to an ESP32 device over Bluetooth Low Energy using the NimBLE OTA service. Supports device scanning, info queries, and firmware upload.

### BLE Service Definition

| UUID | Type | Description |
|------|------|-------------|
| `fb1e4001-54ae-4a28-9f74-dfccb248601d` | Service | OTA Service |
| `fb1e4002-54ae-4a28-9f74-dfccb248601d` | Characteristic | Control (write + notify) |
| `fb1e4003-54ae-4a28-9f74-dfccb248601d` | Characteristic | Data (write-no-response) |

### Protocol Commands (Control Characteristic)

| Code | Name | Payload | Direction |
|------|------|---------|-----------|
| `0x01` | `CMD_BEGIN` | `uint32 size` + `uint32 crc32` (LE) | PC -> Device |
| `0x02` | `CMD_ABORT` | (none) | PC -> Device |
| `0x03` | `CMD_INFO` | (none) | PC -> Device |
| `0x04` | `CMD_SIG` | `r[32]` + `s[32]` (raw bytes) | PC -> Device |

### Protocol Responses (Control Characteristic Notifications)

| Code | Name | Payload | Description |
|------|------|---------|-------------|
| `0x10` | `RESP_READY` | (none) | Device ready to receive data |
| `0x11` | `RESP_PROGRESS` | `uint8 percent` | Transfer progress (0-100) |
| `0x12` | `RESP_OK` | (none) | OTA complete, device will reboot |
| `0x13` | `RESP_FAIL` | UTF-8 reason string | OTA failed |
| `0x14` | `RESP_INFO` | UTF-8 JSON string | Device info response |

### Transfer Flow

```
1. Scan for devices advertising "OTA" in name
2. Connect, negotiate MTU (target: 517)
3. Subscribe to Control characteristic notifications
4. CMD_INFO -> wait for RESP_INFO
5. CMD_BEGIN (size, crc32) -> wait for RESP_READY
6. CMD_SIG (r, s) -> wait for ACK  (signed .ota only)
7. Write firmware chunks to Data characteristic (write-no-response)
   - Chunk size: MTU - 3 (ATT header), clamped to 20-512 bytes
   - Pacing: 4 chunks per batch, 20ms delay between batches
8. Wait for RESP_OK or RESP_FAIL (up to 120s for CRC + ECDSA verification)
```

### Device Discovery

The scanner looks for BLE devices with "OTA" in the advertised name (case-insensitive). When multiple devices are found, an interactive menu is presented. Devices can also be selected by exact BLE address.

### Usage

```bash
# Scan for OTA devices
python3 tools/ble_ota.py --scan

# Get device info
python3 tools/ble_ota.py --info
python3 tools/ble_ota.py --info --addr AA:BB:CC:DD:EE:FF

# Upload firmware (auto-scan)
python3 tools/ble_ota.py firmware.bin
python3 tools/ble_ota.py firmware.ota

# Upload to specific device
python3 tools/ble_ota.py firmware.ota --addr AA:BB:CC:DD:EE:FF
python3 tools/ble_ota.py firmware.ota --name ESP32-OTA
```

### CLI Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `firmware` | (optional) | Path to `.bin` or `.ota` file (required unless `--scan` or `--info`) |
| `--addr` | auto-scan | BLE device address (`AA:BB:CC:DD:EE:FF`) |
| `--name` | `ESP32-OTA` | BLE device name filter for scanning |
| `--scan` | off | Scan and list OTA devices without connecting |
| `--info` | off | Connect and query device info |

### Dependencies

- `bleak` (async BLE library: `pip install bleak`)
- Standard library: `asyncio`, `struct`, `binascii`, `time`, `argparse`

---

## provision_sd.py

Generates SD card provisioning contents for factory setup of ESP32 devices. Creates a `/provision/` directory structure containing device identity, TLS certificates, and optional factory WiFi credentials.

### Provisioning Directory Structure

```
/provision/
  device.json         -- Device identity and server configuration
  ca.pem              -- CA certificate for TLS verification
  client.crt          -- Client certificate for mTLS
  client.key          -- Client private key for mTLS
  factory_wifi.json   -- Temporary WiFi credentials (optional)
```

### device.json Format

```json
{
  "device_id": "esp32-a1b2c3d4e5f6",
  "device_name": "ESP32-OTA-Kitchen",
  "server_url": "https://api.example.com",
  "mqtt_broker": "broker.example.com",
  "mqtt_port": 8883,
  "provisioned": true
}
```

### Certificate File Resolution

The tool searches for certificates under multiple common naming conventions:

| Target File | Source Names Searched |
|-------------|---------------------|
| `ca.pem` | `ca.pem`, `ca.crt`, `ca-cert.pem` |
| `client.crt` | `client.crt`, `client.pem`, `client-cert.pem` |
| `client.key` | `client.key`, `client-key.pem` |

### Batch Provisioning

Batch mode generates unique device IDs (UUID-based: `esp32-<12 hex chars>`) and distributes them round-robin across multiple SD cards. A `provision_manifest.json` is written to the current directory listing all generated device-to-SD-card mappings.

### Usage

```bash
# Single device
python3 tools/provision_sd.py --sd /media/sdcard --certs ./certs \
    --server https://api.example.com

# With factory WiFi
python3 tools/provision_sd.py --sd /media/sdcard --certs ./certs \
    --server https://api.example.com \
    --wifi-ssid "FactoryNet" --wifi-pass "setup123"

# Batch: 10 devices across 3 SD cards
python3 tools/provision_sd.py --sd /media/sd0 /media/sd1 /media/sd2 \
    --certs ./certs --server https://api.example.com --batch 10

# Dry run (preview without writing)
python3 tools/provision_sd.py --sd /media/sdcard --certs ./certs \
    --server https://api.example.com --dry-run
```

### CLI Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `--sd` | (required) | One or more SD card mount points |
| `--certs` | (required) | Directory containing CA and client certificates |
| `--server` | (required) | Server endpoint URL |
| `--mqtt` | `""` | MQTT broker address |
| `--mqtt-port` | `8883` | MQTT broker port |
| `--device-id` | auto | Device ID (auto-generated UUID if omitted) |
| `--device-name` | same as ID | Human-friendly device name |
| `--batch` | `0` | Generate N unique device configs (round-robin across SDs) |
| `--wifi-ssid` | none | Factory WiFi SSID (cleared after first connection) |
| `--wifi-pass` | none | Factory WiFi password |
| `--dry-run` | off | Preview output without writing files |

### Dependencies

- Standard library only: `json`, `os`, `shutil`, `uuid`, `pathlib`, `argparse`

---

## detect_boards.py

Detects connected Waveshare ESP32-S3 boards via USB serial. Combines udev device enumeration with firmware-level identification.

### Detection Method

1. **USB enumeration**: Scans `/dev/ttyACM*` and queries udev properties via `udevadm info`
2. **Vendor filter**: Only devices with `ID_VENDOR=Espressif` are included
3. **MAC lookup**: `ID_USB_SERIAL_SHORT` is checked against a known MAC-to-board table
4. **Firmware query**: Sends `IDENTIFY\n` at 115200 baud and parses JSON response

### Known MAC Table

| MAC Address | Board |
|-------------|-------|
| `1C:DB:D4:9C:CD:68` | ESP32-S3-Touch-AMOLED-2.41-B |
| `20:6E:F1:9A:24:E8` | ESP32-S3-Touch-LCD-3.49 |
| `20:6E:F1:9A:12:00` | ESP32-S3-Touch-LCD-3.5B-C |

### IDENTIFY Protocol

The firmware responds to `IDENTIFY\n` with a JSON object:

```json
{"board":"touch-lcd-349","display":"AXS15231B","interface":"QSPI","app":"starfield"}
```

Boards not running the project firmware (or in bootloader mode) will not respond.

### Usage

```bash
# Human-readable output
python3 tools/detect_boards.py

# Machine-readable JSON
python3 tools/detect_boards.py --json
```

### CLI Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `--json` | off | Output results as JSON array |

### Dependencies

- `pyserial` (`serial.Serial`)
- System: `udevadm` (Linux only)

---

## test_ota.py

Comprehensive end-to-end OTA test suite that exercises serial commands, BLE communication, packaging/verification, and firmware transfer across multiple pathways.

### Test Functions

| Test Name | Function | Description |
|-----------|----------|-------------|
| `serial-info` | `test_serial_info()` | Sends `IDENTIFY\n`, expects JSON with `"board"` field |
| `serial-ota-info` | `test_serial_ota_info()` | Sends `OTA_INFO\n`, expects JSON with `"version"` field |
| `serial-fw-hash` | `test_serial_fw_hash()` | Sends `FW_HASH\n`, expects 64-char SHA-256 hex string |
| `serial-audit-log` | `test_serial_audit_log()` | Sends `OTA_AUDIT\n`, expects audit entries or `OTA_AUDIT_EMPTY` |
| `serial-mesh-status` | `test_serial_mesh_status()` | Sends `MESH_OTA_STATUS\n`, expects `"sending"` and `"receiving"` fields |
| `package-verify` | `test_package_verify()` | Packages firmware with signing, then verifies the `.ota` file |
| `ble-scan` | `test_ble_scan()` | Scans BLE for devices with "ESP32-OTA" in name |
| `ble-info` | `test_ble_info()` | Connects via BLE and queries device info |
| `serial-ota-signed` | `test_serial_ota_signed()` | Full serial OTA transfer with signed `.ota` package |
| `ble-ota-signed` | `test_ble_ota_signed()` | Full BLE OTA transfer with signed `.ota` package |
| `ble-ota-tampered` | `test_ble_ota_tampered()` | BLE OTA with corrupted payload (expects rejection) |

### Test Execution Flow

1. Package the provided `firmware.bin` into a signed `.ota` using the provided signing key
2. Run each test sequentially with 3-second inter-test delays (device recovery time)
3. Print pass/fail summary with counts
4. Exit code 0 if all tests pass, 1 if any fail

### Tamper Detection Test

`test_ble_ota_tampered()` creates a modified copy of the `.ota` file (flips byte at offset 200) and attempts BLE upload. The test passes if the device rejects the firmware with a CRC32 mismatch or transfer failure.

### Usage

```bash
# Run all tests
python3 tools/test_ota.py --port /dev/ttyACM0 \
    --firmware .pio/build/touch-lcd-349-ota/firmware.bin \
    --key keys/ota_signing_key.pem

# Run a specific test
python3 tools/test_ota.py --port /dev/ttyACM0 \
    --firmware firmware.bin --key keys/ota_signing_key.pem \
    --test serial-info

# Skip BLE tests (serial + packaging only)
python3 tools/test_ota.py --port /dev/ttyACM0 \
    --firmware firmware.bin --key keys/ota_signing_key.pem \
    --skip-ble
```

### CLI Arguments

| Argument | Default | Description |
|----------|---------|-------------|
| `--port` | (required) | Serial port for connected device |
| `--firmware` | (required) | Path to `firmware.bin` to test with |
| `--key` | `keys/ota_signing_key.pem` | ECDSA signing key for packaging |
| `--test` | all | Run only the named test |
| `--skip-ble` | off | Skip all BLE-based tests |

### Dependencies

- `pyserial` (for serial tests)
- `bleak` (for BLE tests, skippable with `--skip-ble`)
- `cryptography` (for packaging/verification test)

---

## Dependencies and Setup

### Python Package Requirements

```bash
# Core (required for all tools)
pip install pyserial

# Cryptographic operations (signing, encryption, verification)
pip install cryptography

# BLE OTA (only needed for ble_ota.py)
pip install bleak
```

### Minimum Versions

| Package | Minimum | Used For |
|---------|---------|----------|
| `pyserial` | 3.4+ | Serial communication (`serial_ota.py`, `detect_boards.py`, `test_ota.py`) |
| `cryptography` | 3.0+ | ECDSA P-256, AES-256-CTR (`package_firmware.py`, `generate_signing_key.py`) |
| `bleak` | 0.19+ | BLE GATT client (`ble_ota.py`) |

### System Requirements

- **Linux**: `udevadm` for USB device detection (`detect_boards.py`)
- **Serial permissions**: User must be in `dialout` group, or use `sudo chmod 666 /dev/ttyACM*`
- **BLE**: BlueZ 5.50+ on Linux; system Bluetooth adapter required

### Quick Setup

```bash
pip install pyserial cryptography bleak
```

---

## Security Considerations

### Key Management

- **Never commit private keys** (`ota_signing_key.pem`, `ota_encryption_key.hex`) to version control
- Add `keys/` and `*.pem` to `.gitignore`
- Private keys are generated with `0o600` permissions (owner read/write only)
- The public key header (`ota_public_key.h`) is safe to commit and must be embedded in firmware
- The encryption key header (`ota_encryption_key.h`) must be embedded in firmware but should not be in public repositories

### Key Rotation

To rotate keys:
1. Generate new keypair: `python3 tools/generate_signing_key.py --output-dir keys/`
2. Copy new `ota_public_key.h` to `include/`
3. Rebuild and flash firmware with new public key via wired connection
4. All subsequent OTA updates must be signed with the new private key
5. Old signed firmware will be rejected after the key rotation flash

### Signing and Verification Chain

```
generate_signing_key.py  -->  ota_signing_key.pem (private, PC-side)
                         -->  ota_public_key.h (public, embedded in firmware)

package_firmware.py      -->  Signs SHA-256(payload) with private key
                         -->  Appends raw (r,s) signature to .ota file

serial_ota.py / ble_ota.py  -->  Sends signature to device before/during transfer

Device firmware          -->  Verifies signature using embedded public key (mbedtls)
```

### Encryption Chain

```
generate_signing_key.py --encryption  -->  ota_encryption_key.hex (PC-side)
                                      -->  ota_encryption_key.h (embedded in firmware)

package_firmware.py --encrypt         -->  AES-256-CTR(firmware) with random IV
                                      -->  Payload = IV || ciphertext
                                      -->  CRC32 and signature cover encrypted payload

Device firmware                       -->  Decrypts after CRC/signature verification passes
```

### Encrypt-Then-Sign

The system uses encrypt-then-sign ordering:
1. Firmware is encrypted (AES-256-CTR with random IV)
2. CRC32 is computed over the encrypted payload
3. ECDSA signature is computed over the encrypted payload
4. On the device: CRC32 is verified first, then signature, then decryption

This prevents oracle attacks where an adversary could use decryption failures to learn about the plaintext.

### Transport Security

- **Serial OTA**: No transport encryption (USB cable is assumed trusted). Firmware integrity is protected by CRC32 + optional ECDSA signature.
- **BLE OTA**: No BLE-level encryption by default (NimBLE pairing not required). Firmware integrity is protected by CRC32 + optional ECDSA signature. For sensitive deployments, enable `OTA_REQUIRE_SIGNATURE` build flag.
- **WiFi OTA**: HTTPS recommended for pull OTA (`OTA_URL`). Push OTA via HTTP upload page has no authentication by default.

### Build Flags

| Flag | Effect |
|------|--------|
| `-DOTA_REQUIRE_SIGNATURE` | Device rejects unsigned firmware on all pathways |
| `-DOTA_ANTI_ROLLBACK` | Device rejects firmware with lower semver version |

---

## Typical Workflow

### Initial Setup

```bash
# 1. Generate signing keys
python3 tools/generate_signing_key.py --output-dir keys/

# 2. Copy public key to firmware include path
cp keys/ota_public_key.h include/

# 3. Build and flash OTA-enabled firmware (wired, first time)
./scripts/flash.sh touch-lcd-349 ota
```

### Firmware Update Cycle

```bash
# 1. Build new firmware
make build BOARD=touch-lcd-349 APP=system

# 2. Package with signing
python3 tools/package_firmware.py \
    .pio/build/touch-lcd-349-system/firmware.bin \
    --sign keys/ota_signing_key.pem \
    --version 1.1.0 --board touch-lcd-349 \
    -o firmware.ota

# 3. Verify the package
python3 tools/package_firmware.py firmware.ota --verify --pubkey keys/ota_signing_key.pem

# 4. Push via serial
python3 tools/serial_ota.py /dev/ttyACM0 firmware.ota

# -- or push via BLE --
python3 tools/ble_ota.py firmware.ota
```

### Testing

```bash
# Run full test suite
python3 tools/test_ota.py \
    --port /dev/ttyACM0 \
    --firmware .pio/build/touch-lcd-349-ota/firmware.bin \
    --key keys/ota_signing_key.pem

# Serial-only tests (no BLE adapter needed)
python3 tools/test_ota.py \
    --port /dev/ttyACM0 \
    --firmware firmware.bin \
    --key keys/ota_signing_key.pem \
    --skip-ble
```
