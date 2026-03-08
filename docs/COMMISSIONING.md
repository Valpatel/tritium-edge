# Device Commissioning Guide

## Overview

Commissioning connects a new Tritium node to the fleet. Every node ships
unprovisioned — it doesn't know which server to talk to or which WiFi to join.
Commissioning provides this identity.

There are **five commissioning paths**, from easiest to most automated:

| Method | Requires | Best For |
|--------|----------|----------|
| **Web Portal** | Phone + WiFi | One-off setup, non-technical users |
| **BLE** | Phone app | Headless nodes, no WiFi initially |
| **USB Serial** | Laptop + USB cable | Lab/bench provisioning |
| **SD Card** | SD card + PC | Bulk pre-provisioning |
| **Peer Seeding** | Another Tritium node | Field deployment, zero-touch |

## 1. Web Captive Portal

When a node can't find a known WiFi network, it creates an access point:

```
SSID: Tritium-XXYY       (where XXYY = last 4 hex of MAC)
Password: (none)
URL: http://192.168.4.1/
```

**Steps:**
1. Power on the node
2. Connect your phone to the `Tritium-XXYY` WiFi
3. The captive portal opens automatically (or browse to `http://192.168.4.1/`)
4. Enter:
   - WiFi SSID and password
   - Server URL (e.g., `http://192.168.1.100:8080`)
   - Device name (optional)
5. Tap "Commission"
6. Node saves credentials, reboots, connects to your WiFi, starts heartbeating

**Display feedback:** Nodes with screens show the AP name and URL on the display
during commissioning mode (starfield overlay).

## 2. BLE Provisioning

For nodes without WiFi access or in environments where you can't create an AP.

**Requirements:**
- Phone with Tritium companion app (or any BLE GATT client like nRF Connect)
- Node built with `ENABLE_BLE` flag

**Steps:**
1. Power on the node
2. Open Tritium app / nRF Connect
3. Scan for BLE devices — look for `Tritium-XXYY`
4. Connect and write to the provisioning characteristic:
   ```
   Service UUID: 00001234-0000-1000-8000-00805f9b34fb
   Characteristic: 00001235-0000-1000-8000-00805f9b34fb
   ```
5. Write JSON payload in chunks:
   ```json
   {
     "cmd": "provision",
     "ssid": "MyNetwork",
     "password": "secret",
     "server_url": "http://192.168.1.100:8080",
     "device_name": "Kitchen Sensor"
   }
   ```
6. Node saves credentials, disconnects BLE, reboots

## 3. USB Serial Provisioning

Best for lab/bench work when the node is connected to a computer.

**Steps:**
1. Connect the node via USB
2. Open a serial terminal at 115200 baud
3. Send the provisioning command as JSON:
   ```json
   {"cmd":"provision","ssid":"MyNetwork","password":"secret","server_url":"http://192.168.1.100:8080","device_name":"Lab Node 1"}
   ```
4. Node responds with `{"status":"ok","device_id":"esp32-aabbccddeeff"}`

**With certificates:**
```json
{
  "cmd": "provision",
  "device_id": "sensor-001",
  "server_url": "https://fleet.example.com",
  "ca_pem": "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----",
  "client_crt": "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----",
  "client_key": "-----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----"
}
```

## 4. SD Card Provisioning

Pre-provision nodes in bulk by preparing SD cards.

**SD card layout:**
```
/provision/
  device.json          # Identity and server config
  ca.pem               # CA certificate (optional)
  client.crt           # Client certificate (optional)
  client.key           # Client private key (optional)
  factory_wifi.json    # WiFi credentials (removed after first connect)
```

**device.json:**
```json
{
  "server_url": "http://192.168.1.100:8080",
  "device_name": "Node-A",
  "mqtt_broker": "192.168.1.100",
  "mqtt_port": 1883
}
```

**factory_wifi.json:**
```json
{
  "ssid": "MyNetwork",
  "password": "secret"
}
```

**Steps:**
1. Prepare the SD card on your PC
2. Insert into the node
3. Boot the node — it reads `/provision/`, saves to internal flash, and clears the SD
4. Factory WiFi is used for initial connection, then removed from SD

**Bulk preparation:** Use the `scripts/prepare_sd_provision.sh` helper:
```bash
./scripts/prepare_sd_provision.sh \
  --server http://192.168.1.100:8080 \
  --ssid MyNetwork --password secret \
  --output /path/to/sdcard
```

## 5. Peer Seeding (Zero-Touch)

A provisioned node can commission nearby unprovisioned nodes automatically.

**How it works:**
1. Unprovisioned node broadcasts on ESP-NOW: `"I'm new, who's here?"`
2. A provisioned node responds with the fleet server URL and WiFi credentials
3. New node provisions itself, connects to the fleet
4. Fleet server auto-registers the node

**Requirements:**
- Both nodes built with `ENABLE_ESPNOW`
- The seed node must have `seed_provisioning: true` in its config

This is the "mesh grows itself" principle in action.

## Server-Side Commissioning

The fleet server also supports commissioning workflows:

### Admin Dashboard
- **Provision > Discovered** tab shows unregistered nodes
- Click "Commission" to assign name, role, location, WiFi, and server URL
- Supports "Bulk Commission" for multiple nodes at once

### REST API
- `GET /api/provision/discovered` — List discovered nodes
- `POST /api/provision/commission/{device_id}` — Commission a node
- `POST /api/provision/bulk` — Commission multiple nodes
- `POST /api/provision/decommission/{device_id}` — Remove a node
- `GET /api/commission/discover` — Active network scan

## Commissioning Data Model

All commissioning methods ultimately write the same data:

| Field | Storage | Description |
|-------|---------|-------------|
| `device_id` | NVS + LittleFS | Unique device identifier (derived from MAC if not set) |
| `device_name` | LittleFS | Human-friendly name |
| `server_url` | LittleFS | Fleet server endpoint |
| `mqtt_broker` | LittleFS | MQTT broker address |
| `ssid` + `password` | NVS (encrypted) | WiFi credentials |
| `ca.pem` | LittleFS | CA certificate for TLS |
| `client.crt` + `client.key` | LittleFS | Client certificates for mTLS |

After commissioning, the node reboots and begins its heartbeat cycle.

## Decommissioning

To remove a node from the fleet:

1. **Server:** `POST /api/provision/decommission/{device_id}` or click "Decommission" in the admin dashboard
2. **On-device:** Send `FACTORY_RESET` via serial, or call `provision.factoryReset()` in code
3. This clears all provisioning data (certs, identity, WiFi) from the node's flash

The node reverts to unprovisioned state and will start the captive portal on next boot.
