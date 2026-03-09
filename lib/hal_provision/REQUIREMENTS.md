# Device Provisioning System Requirements and Features

## Overview

The Provisioning HAL manages device identity, TLS certificates, and factory WiFi credentials for secure service connectivity (Cloudflare tunnels, MQTT brokers, AI gateways). It supports three provisioning sources: SD card batch provisioning, USB serial interactive provisioning, and programmatic API. All persistent data is stored on LittleFS in the `/prov/` directory.

## Architecture

```
                    +---------------------+
                    |   Fleet Server      |
                    | (commission device) |
                    +----------+----------+
                               |
               Generates certs | + device identity
                               |
              +----------------+----------------+
              |                                 |
         SD Card                           USB Serial
         (batch provision)                 (interactive)
              |                                 |
              v                                 v
         +----------------------------------------------+
         |              ProvisionHAL                     |
         |                                              |
         |  DeviceIdentity   <- device_id, name, URLs  |
         |  TLS Certificates <- CA, client cert+key     |
         |  Factory WiFi     <- temporary bootstrap     |
         |                                              |
         |  Storage: LittleFS /prov/                    |
         |    device.json    - identity + config        |
         |    ca.pem         - CA certificate           |
         |    client.crt     - client certificate       |
         |    client.key     - client private key       |
         |    factory_wifi.json - bootstrap WiFi        |
         +----------------------------------------------+
                    |
                    | getCACert(), getClientCert(), getClientKey()
                    |
              +-----+------+------+
              |            |      |
         WiFiClient   MQTT     HTTPS
         Secure       TLS      API
```

## Provisioning State Machine

```
  UNPROVISIONED --(provisionFromSD / USB / manual import)--> PROVISIONED
  PROVISIONED   --(factoryReset)-----------------------------> UNPROVISIONED
  any           --(LittleFS failure)-------------------------> ERROR
```

A device is considered `PROVISIONED` when **all three** conditions are met:
1. CA certificate exists at `/prov/ca.pem`
2. Client certificate exists at `/prov/client.crt`
3. Client private key exists at `/prov/client.key`
4. `DeviceIdentity.provisioned` is `true` in `/prov/device.json`

## Device Identity

The `DeviceIdentity` struct holds all service connectivity metadata:

| Field | Size | Description |
|-------|------|-------------|
| `device_id` | 64 chars | Unique device identifier (e.g., MAC-derived or fleet-assigned) |
| `device_name` | 64 chars | Human-friendly display name |
| `server_url` | 256 chars | Primary server endpoint URL |
| `mqtt_broker` | 128 chars | MQTT broker hostname or IP |
| `mqtt_port` | uint16 | MQTT port (default: 8883 for TLS) |
| `provisioned` | bool | Provisioning completion flag |

Identity is persisted as JSON in `/prov/device.json`:

```json
{
  "device_id": "esp32-sensor-042",
  "device_name": "Lab Temperature Sensor",
  "server_url": "https://fleet.example.com",
  "mqtt_broker": "mqtt.example.com",
  "mqtt_port": 8883,
  "provisioned": true
}
```

## Provisioning Sources

### 1. SD Card Provisioning

Batch provisioning for manufacturing or field deployment. The fleet server generates a provisioning bundle per device, which is placed on an SD card.

**SD card directory layout** (default path: `/provision/`):

```
/provision/
  device.json         -- Device identity and server config (required)
  ca.pem              -- CA certificate chain (required)
  client.crt          -- Client TLS certificate (required)
  client.key          -- Client private key (required)
  factory_wifi.json   -- Bootstrap WiFi credentials (optional)
```

**factory_wifi.json format:**

```json
{
  "ssid": "FactoryNetwork",
  "password": "bootstrap123"
}
```

**Provisioning flow:**

1. Application calls `provisionFromSD("/provision")`
2. HAL copies `ca.pem`, `client.crt`, `client.key` from SD to LittleFS `/prov/`
3. HAL parses `device.json` and saves identity to LittleFS
4. HAL parses `factory_wifi.json` (if present) and saves to LittleFS
5. HAL sets `provisioned = true` and verifies all files exist
6. Returns `true` if state transitions to `PROVISIONED`

File size limit per cert file: 16KB. SD card access requires `HAS_SDCARD` to be defined and `SD_MMC` initialized.

### 2. USB Serial Provisioning

Interactive provisioning over USB CDC serial, useful for development and single-device commissioning.

**Serial protocol:**

| Command | Direction | Description |
|---------|-----------|-------------|
| `PROVISION_BEGIN` | Host -> Device | Enter provisioning mode |
| JSON payload + `\n` | Host -> Device | Provisioning data (single line) |
| `{"status":"ready",...}` | Device -> Host | Ready acknowledgment |
| `{"status":"ok",...}` | Device -> Host | Provisioning success |
| `{"status":"error",...}` | Device -> Host | Provisioning failure |
| `PROVISION_STATUS` | Host -> Device | Query current provisioning state |
| `PROVISION_RESET` | Host -> Device | Factory reset all provisioning data |

**JSON provisioning payload:**

```json
{
  "cmd": "provision",
  "device_id": "esp32-sensor-042",
  "device_name": "Lab Sensor",
  "server_url": "https://fleet.example.com",
  "mqtt_broker": "mqtt.example.com",
  "mqtt_port": 8883,
  "ca_pem": "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----",
  "client_crt": "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----",
  "client_key": "-----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----"
}
```

**Provisioning flow:**

1. Host sends `PROVISION_BEGIN`
2. Device responds with `{"status":"ready","msg":"Send provisioning JSON"}`
3. Host sends the JSON payload as a single line terminated by `\n`
4. Device parses JSON, imports certs to LittleFS, saves identity
5. Device responds with `{"status":"ok","msg":"Provisioned"}` or `{"status":"error",...}`
6. USB provisioning mode deactivates automatically

**Buffer constraint:** USB serial buffer is 4096 bytes. The entire JSON payload including PEM certificates must fit in a single line under this limit. For large certificate chains, use SD card provisioning instead.

### 3. Programmatic API

Direct certificate and identity management for custom provisioning workflows:

```cpp
ProvisionHAL prov;
prov.init();

// Import certificates individually
prov.importCACert(caPem, strlen(caPem));
prov.importClientCert(clientCrtPem, strlen(clientCrtPem));
prov.importClientKey(clientKeyPem, strlen(clientKeyPem));

// Set identity
DeviceIdentity id = {};
strncpy(id.device_id, "my-device", sizeof(id.device_id));
strncpy(id.server_url, "https://api.example.com", sizeof(id.server_url));
id.mqtt_port = 8883;
id.provisioned = true;
prov.setDeviceIdentity(id);
```

## Certificate Access

After provisioning, certificates are available for TLS clients:

```cpp
char ca[4096], cert[4096], key[4096];
size_t caLen, certLen, keyLen;

prov.getCACert(ca, sizeof(ca), &caLen);
prov.getClientCert(cert, sizeof(cert), &certLen);
prov.getClientKey(key, sizeof(key), &keyLen);

// Use with WiFiClientSecure
WiFiClientSecure client;
client.setCACert(ca);
client.setCertificate(cert);
client.setPrivateKey(key);

// Use with PubSubClient (MQTT)
PubSubClient mqtt(client);
mqtt.setServer(prov.getIdentity().mqtt_broker, prov.getIdentity().mqtt_port);
```

## Factory WiFi

Factory WiFi provides temporary network credentials for initial device setup. It is designed to be cleared after the device connects to its permanent network.

```cpp
if (prov.hasFactoryWiFi()) {
    char ssid[64], pass[64];
    prov.getFactoryWiFi(ssid, sizeof(ssid), pass, sizeof(pass));
    WiFi.begin(ssid, pass);

    // After successful connection and permanent WiFi configured:
    prov.clearFactoryWiFi();
}
```

Factory WiFi is stored on LittleFS (not NVS) at `/prov/factory_wifi.json` and is removed by both `clearFactoryWiFi()` and `factoryReset()`.

## Factory Reset

`factoryReset()` removes all provisioning data:
- Deletes `ca.pem`, `client.crt`, `client.key`
- Deletes `device.json`
- Deletes `factory_wifi.json`
- Clears in-memory `DeviceIdentity`
- Sets state to `UNPROVISIONED`

Factory reset does not affect NVS (user WiFi credentials stored by hal_wifi) or other LittleFS data outside `/prov/`.

## Fleet Server Commissioning Workflow

The intended end-to-end commissioning flow:

1. **Server**: Generate device identity + TLS client certificate signed by fleet CA
2. **Server**: Create provisioning bundle (device.json + certs + optional factory WiFi)
3. **Provisioning**: Copy bundle to SD card, or send over USB serial
4. **Device boot**: `prov.init()` loads identity and checks provisioning state
5. **Device**: If provisioned, use certificates for mutual-TLS connections to fleet server
6. **Device**: Report identity in heartbeat/IDENTIFY response
7. **Device**: Connect to MQTT broker using provisioned credentials
8. **Server**: Accept device based on client certificate, track by device_id

## JSON Parsing

The HAL includes a minimal hand-written JSON parser (no ArduinoJson dependency):
- `jsonGetString()`: Extract string values with escape handling (`\n`, `\t`, `\\`, `\"`)
- `jsonGetInt()`: Extract integer values (supports both quoted and unquoted numbers)
- Sufficient for the simple flat JSON structures used in provisioning
- No nested object or array support

## API Reference

### Lifecycle

```cpp
ProvisionHAL prov;
bool ok = prov.init();           // Mount LittleFS, load identity, check state
ProvisionState s = prov.getState();  // UNPROVISIONED, PROVISIONED, or ERROR
bool ready = prov.isProvisioned();
```

### Provisioning

```cpp
// SD card
bool ok = prov.provisionFromSD("/provision");

// USB serial
prov.startUSBProvision();
while (prov.isUSBProvisionActive()) {
    prov.processUSBProvision();  // Call in loop()
}

// Manual
prov.importCACert(pem, len);
prov.importClientCert(pem, len);
prov.importClientKey(pem, len);
prov.setDeviceIdentity(id);
```

### Certificate Access

```cpp
char buf[4096];
size_t len;
prov.getCACert(buf, sizeof(buf), &len);
prov.getClientCert(buf, sizeof(buf), &len);
prov.getClientKey(buf, sizeof(buf), &len);
```

### Factory WiFi

```cpp
bool has = prov.hasFactoryWiFi();
char ssid[64], pass[64];
prov.getFactoryWiFi(ssid, sizeof(ssid), pass, sizeof(pass));
prov.clearFactoryWiFi();
```

### Factory Reset

```cpp
prov.factoryReset();  // Wipe all provisioning data
```

### Test Harness

```cpp
ProvisionHAL::TestResult r = prov.runTest();
// r.init_ok, r.fs_ok, r.cert_write_ok, r.cert_read_ok
// r.cert_verify_ok (data integrity), r.identity_ok
// r.factory_wifi_ok, r.factory_reset_ok
// r.state, r.test_duration_ms
```

The test harness performs a complete round-trip: write test cert, read it back, verify data integrity, save/load identity, save/load/clear factory WiFi, and factory reset. All test data is cleaned up via factory reset at the end.

## Storage Layout

| Path | Format | Description |
|------|--------|-------------|
| `/prov/device.json` | JSON | Device identity and server configuration |
| `/prov/ca.pem` | PEM | CA certificate (or certificate chain) |
| `/prov/client.crt` | PEM | Client TLS certificate |
| `/prov/client.key` | PEM | Client private key |
| `/prov/factory_wifi.json` | JSON | Temporary bootstrap WiFi credentials |

## Security Considerations

- **Private key storage**: Client private key is stored in plaintext on LittleFS. Enable ESP32 flash encryption for at-rest protection.
- **SD card exposure**: Provisioning SD card contains private keys. Remove the card after provisioning, or use a secure provisioning station.
- **USB buffer size**: The 4KB USB serial buffer limits certificate size. Large certificate chains should use SD card provisioning.
- **No certificate validation**: The HAL stores and retrieves certificates but does not validate them (no chain verification, expiry checking, or revocation). Validation is the responsibility of the TLS client (mbedtls/WiFiClientSecure).
- **Factory reset protection**: `factoryReset()` is callable without authentication. Applications should gate this behind a confirmation mechanism (physical button, serial command, etc.) to prevent accidental data loss.
- **JSON parser limitations**: The minimal JSON parser does not handle nested objects, arrays, or Unicode escapes. Malformed JSON may produce partial results rather than errors.

## Platform Support

- **ESP32**: Full implementation using LittleFS, SD_MMC, and Arduino Serial
- **Simulator**: Full implementation using host filesystem (`./littlefs/` and `./sdcard/` directories). All operations work identically except USB serial input (stub).

## File Structure

```
lib/hal_provision/
  hal_provision.h       -- Class declaration, DeviceIdentity struct, enums
  hal_provision.cpp     -- ESP32 + simulator implementations, JSON parser
  REQUIREMENTS.md       -- This file
```
