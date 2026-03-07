# hal_provision - Secure Device Provisioning

Handles provisioning ESP32 devices with TLS certificates, device identity, and factory WiFi credentials for connecting to secure services (Cloudflare tunnels, AI gateways, MQTT brokers, etc.).

## Architecture

Provisioning data is stored across three layers, each chosen for its persistence and security characteristics:

| Storage | Contents | Survives OTA | Encrypted |
|---------|----------|-------------|-----------|
| **LittleFS** (internal flash) | Certs, device identity, server URLs | Yes (separate partition) | If flash encryption enabled |
| **NVS** | User WiFi credentials | Yes | If flash encryption enabled |
| **SD card** | Provisioning source files | N/A (removable) | No |

LittleFS stores certs in `/prov/` on a dedicated partition that is not overwritten during OTA updates. This means a device retains its identity and certificates across firmware upgrades.

## SD Card Provisioning

### Directory Layout

Place these files on the SD card under `/provision/`:

```
/provision/
  device.json          # Device identity and server config (required)
  ca.pem               # CA certificate (required)
  client.crt           # Client certificate (required)
  client.key           # Client private key (required)
  factory_wifi.json    # Temporary WiFi for initial setup (optional)
```

### device.json

```json
{
  "device_id": "esp32-kitchen-001",
  "device_name": "Kitchen Sensor",
  "server_url": "https://api.example.com",
  "mqtt_broker": "mqtt.example.com",
  "mqtt_port": 8883
}
```

### factory_wifi.json

```json
{
  "ssid": "FactorySetup",
  "password": "temp-password-123"
}
```

### Usage

```cpp
#include "hal_provision.h"
#include "hal_sdcard.h"

ProvisionHAL prov;
SDCardHAL sd;

void setup() {
    sd.init();
    prov.init();

    if (!prov.isProvisioned()) {
        prov.provisionFromSD("/provision");
    }

    // Use certs for secure connections
    char ca[4096], crt[4096], key[4096];
    prov.getCACert(ca, sizeof(ca));
    prov.getClientCert(crt, sizeof(crt));
    prov.getClientKey(key, sizeof(key));
}
```

## USB Serial Provisioning

For headless devices without SD card slots, provisioning can be done over USB serial.

### Protocol

The host sends a single newline-terminated JSON line:

```json
{"cmd":"provision","device_id":"esp32-001","device_name":"My Device","server_url":"https://api.example.com","mqtt_broker":"mqtt.example.com","mqtt_port":8883,"ca_pem":"-----BEGIN CERTIFICATE-----\n...","client_crt":"-----BEGIN CERTIFICATE-----\n...","client_key":"-----BEGIN RSA PRIVATE KEY-----\n..."}
```

Responses from the device:

- Ready: `{"status":"ready","msg":"Send provisioning JSON"}`
- Success: `{"status":"ok","msg":"Provisioned"}`
- Failure: `{"status":"error","msg":"Provisioning failed"}`

### Usage

```cpp
ProvisionHAL prov;

void setup() {
    Serial.begin(115200);
    prov.init();
    if (!prov.isProvisioned()) {
        prov.startUSBProvision();
    }
}

void loop() {
    if (prov.isUSBProvisionActive()) {
        prov.processUSBProvision();
    }
}
```

## Factory WiFi Flow

Factory WiFi is a temporary credential used only during initial provisioning to allow the device to reach the network for the first time.

1. Provisioning writes `factory_wifi.json` to LittleFS
2. Device boots, detects factory WiFi, connects using those credentials
3. Device reaches the server, confirms activation
4. Application calls `prov.clearFactoryWiFi()` to remove the temporary credentials
5. User configures permanent WiFi through the normal flow (BLE, web portal, etc.)

```cpp
if (prov.hasFactoryWiFi()) {
    char ssid[64], pass[64];
    prov.getFactoryWiFi(ssid, sizeof(ssid), pass, sizeof(pass));
    WiFi.begin(ssid, pass);
    // ... connect and verify ...
    prov.clearFactoryWiFi();  // One-time use
}
```

## OTA Survival

Certificates and device identity persist across OTA updates because:

- They are stored on the **LittleFS partition**, which is separate from the application partition
- The ESP32 OTA process only overwrites the app partition (OTA_0 / OTA_1)
- The LittleFS partition (`spiffs` or `littlefs` in partition table) is left untouched

Ensure your `partitions.csv` includes a dedicated storage partition:

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
app0,     app,  ota_0,   0x10000, 0x1E0000
app1,     app,  ota_1,   0x1F0000,0x1E0000
storage,  data, spiffs,  0x3D0000,0x30000
```

## Provisioning Flow

```mermaid
graph TD
    A[Device Boot] --> B{Provisioned?}
    B -->|Yes| C[Load certs from LittleFS]
    C --> D[Connect to server]
    B -->|No| E{SD card present?}
    E -->|Yes| F[Read /provision/ from SD]
    F --> G[Copy certs to LittleFS]
    G --> H[Parse device.json]
    H --> I[Save identity]
    I --> J{factory_wifi.json?}
    J -->|Yes| K[Save factory WiFi]
    K --> L[Connect with factory WiFi]
    L --> M[Clear factory WiFi]
    M --> D
    J -->|No| D
    E -->|No| N{USB serial?}
    N -->|Yes| O[Receive JSON over Serial]
    O --> P[Import certs + identity]
    P --> D
    N -->|No| Q[Wait for provisioning]
```

## API Summary

| Method | Description |
|--------|-------------|
| `init()` | Mount LittleFS, check provisioning state |
| `isProvisioned()` | Returns true if certs and identity are present |
| `provisionFromSD(path)` | Import certs and config from SD card |
| `startUSBProvision()` | Begin USB serial provisioning mode |
| `processUSBProvision()` | Process incoming serial data (call in loop) |
| `getCACert/getClientCert/getClientKey()` | Read certificates into buffers |
| `hasFactoryWiFi/getFactoryWiFi/clearFactoryWiFi()` | Factory WiFi lifecycle |
| `importCACert/importClientCert/importClientKey()` | Manual cert import |
| `setDeviceIdentity()` | Set device identity programmatically |
| `factoryReset()` | Wipe all provisioning data |
| `runTest()` | Self-test: write/read/verify certs, identity, WiFi, reset |

## Test Harness

```cpp
ProvisionHAL prov;
auto result = prov.runTest();
// result.init_ok, .fs_ok, .cert_write_ok, .cert_read_ok,
// .cert_verify_ok, .identity_ok, .factory_wifi_ok, .factory_reset_ok
```

The test writes a dummy certificate, reads it back, verifies integrity, tests identity save/load, exercises the factory WiFi save/load/clear cycle, and performs a factory reset. All test artifacts are cleaned up.
