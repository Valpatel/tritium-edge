# Meshtastic Integration Research

Created by Matthew Valancy
Copyright 2026 Valpatel Software LLC
SPDX-License-Identifier: AGPL-3.0

Research notes for integrating LoRa/Meshtastic devices into the Tritium ecosystem.

---

## Table of Contents

1. [Meshtastic Python API (for tritium-sc)](#meshtastic-python-api)
2. [Serial/USB Protocol (ESP32-to-Meshtastic)](#serialusb-protocol)
3. [Key LoRa Hardware](#key-lora-hardware)
4. [Mesh Topology and Routing](#mesh-topology-and-routing)
5. [ESP32 LoRa Libraries](#esp32-lora-libraries)
6. [Commissioning and Provisioning](#commissioning-and-provisioning)
7. [Tritium Integration Strategy](#tritium-integration-strategy)

---

## Meshtastic Python API

The `meshtastic` Python package (latest: v2.7.7, Jan 2026) provides full programmatic
control over Meshtastic devices from Python. This is the primary integration point for
tritium-sc.

### Installation

```bash
pip3 install meshtastic
```

### Interface Classes

| Class | Transport | Use Case |
|---|---|---|
| `SerialInterface` | USB serial | Direct USB connection to device |
| `TCPInterface` | TCP/IP | Network connection (ESP32 WiFi) |
| `BLEInterface` | Bluetooth LE | Wireless connection |

All interfaces share a common API. `SerialInterface` auto-discovers Meshtastic devices
on serial ports if no path is specified.

### Key API

```python
import meshtastic
import meshtastic.serial_interface
from pubsub import pub

# Connect (auto-discover or specify port)
interface = meshtastic.serial_interface.SerialInterface()
# interface = meshtastic.serial_interface.SerialInterface("/dev/ttyUSB0")

# Send text broadcast
interface.sendText("hello mesh")

# Send to specific node
interface.sendText("hello node", destinationId="!abcd1234")

# Send binary data
interface.sendData(bytes([0x01, 0x02]), portNum=256)

# Access node database
for node in interface.nodes.values():
    print(node)

# Access local device config
local = interface.localNode
local.localConfig       # Radio settings
local.moduleConfig      # Module settings
local.channels          # Channel list
```

### Event System (Pub/Sub)

```python
def onReceive(packet, interface):
    """Called for every received mesh packet."""
    print(f"From: {packet['fromId']}")
    print(f"Data: {packet['decoded']['data']['payload']}")

def onConnection(interface, topic=pub.AUTO_TOPIC):
    """Called when connected to radio."""
    interface.sendText("tritium-sc online")

pub.subscribe(onReceive, "meshtastic.receive")
pub.subscribe(onReceive, "meshtastic.receive.text")
pub.subscribe(onReceive, "meshtastic.receive.position")
pub.subscribe(onConnection, "meshtastic.connection.established")
pub.subscribe(lambda *a: print("lost"), "meshtastic.connection.lost")
pub.subscribe(lambda node, *a: print(node), "meshtastic.node.updated")
```

### CLI Tool

The pip package also installs a `meshtastic` CLI:

```bash
meshtastic --info                           # Device info
meshtastic --nodes                          # List mesh nodes
meshtastic --sendtext "hello"               # Send broadcast
meshtastic --set lora.region US             # Set region
meshtastic --set serial.enabled true        # Enable serial module
meshtastic --set serial.mode TEXTMSG        # Set serial mode
meshtastic --export-config > config.yaml    # Export config
```

### Relevance to tritium-sc

tritium-sc can use `SerialInterface` or `TCPInterface` to:
- Bridge mesh messages to MQTT (and thus to the AI pipeline)
- Monitor node positions and telemetry
- Push commands/alerts to mesh nodes
- Manage device configuration programmatically

---

## Serial/USB Protocol

This section covers two things: (1) how an external MCU (like our ESP32 display boards)
can talk to a Meshtastic radio over UART, and (2) the underlying wire protocol.

### Client API Wire Protocol

All Meshtastic client communication uses Protocol Buffers over a framed stream.

**Serial framing (4-byte header):**

| Byte | Value | Purpose |
|---|---|---|
| 0 | `0x94` | START1 magic |
| 1 | `0xc3` | START2 magic |
| 2 | MSB | Protobuf message length (high byte) |
| 3 | LSB | Protobuf message length (low byte) |

Followed by the protobuf-encoded `ToRadio` or `FromRadio` message. Max packet size
is 512 bytes; anything larger indicates corruption.

**Connection sequence:**
1. Send `startConfig` ToRadio message
2. Read FromRadio repeatedly to receive: RadioConfig, User, MyNodeInfo, NodeInfo
   entries, endConfig marker, pending MeshPackets
3. Subscribe to notifications for new data

**BLE uses the same protobuf messages** but over GATT characteristics:
- Service UUID: `6ba1b218-15a8-461f-9fa8-5dcae273eafd`
- `FromRadio` characteristic (read): protobuf packets from device
- `ToRadio` characteristic (write): protobuf packets to device
- `FromNum` characteristic (notify): signals new data available
- Recommended MTU: 512 bytes

### Serial Module (UART Bridge)

The Meshtastic Serial Module turns the device into a UART bridge, allowing an external
MCU to send/receive mesh messages without implementing the full protobuf protocol.

**Operating Modes:**

| Mode | Description | Complexity |
|---|---|---|
| **SIMPLE** | Dumb UART tunnel, requires channel named "serial" | Lowest |
| **TEXTMSG** | Sends strings as text messages to default channel; incoming prefixed with `<ShortName>: ` | Low |
| **PROTO** | Full Protobuf Client API on UART (same as USB protocol) | High |
| **NMEA** | Outputs NMEA 0183 GPS sentences | Special |
| **CALTOPO** | NMEA waypoints every 10s for SARTopo | Special |

**Configuration:**

| Setting | Default | Notes |
|---|---|---|
| `serial.enabled` | false | Must enable |
| `serial.mode` | SIMPLE | See modes above |
| `serial.baud` | 38400 | 110-921600 supported |
| `serial.rxd` | 0 (unset) | GPIO pin for RX |
| `serial.txd` | 0 (unset) | GPIO pin for TX |
| `serial.timeout` | 250ms | Message timeout |
| `serial.echo` | false | Echo received data |
| `serial.override_console` | false | Replace USB console with serial module |

### ESP32-to-Meshtastic UART Wiring

For our Waveshare ESP32-S3 boards talking to a Meshtastic radio over UART:

```
ESP32-S3 (Tritium)          Meshtastic Device
─────────────────          ──────────────────
TX GPIO ──────────────────> RX GPIO (serial.rxd)
RX GPIO <────────────────── TX GPIO (serial.txd)
GND ──────────────────────> GND
(optional) 3V3 ───────────> 3V3
```

**Recommended approach for Tritium:**
- Use **TEXTMSG** mode for simple alert/status messages
- Use **PROTO** mode for full control (position, telemetry, node management)
- For PROTO mode, use the Meshtastic Arduino client library or implement the
  protobuf framing directly

---

## Key LoRa Hardware

### Lilygo T-LoRa Pager

The T-LoRa Pager is a complete handheld Meshtastic device with extensive I/O.

| Feature | Spec |
|---|---|
| MCU | ESP32-S3 dual-core 240MHz |
| Memory | 8MB PSRAM, 16MB Flash |
| Display | 2.3" IPS LCD, 480x222 |
| LoRa | SX1262 (also LR1121/CC1101 variants) |
| GPS | u-blox MIA-M10Q |
| NFC | ST25R3916 |
| IMU | BHI260AP (AI-enabled) |
| Audio | ES8311 codec (mic + speaker + headphone) |
| RTC | Yes |
| Input | QWERTY keyboard + rotary encoder |
| Battery | 500mAh Li-ion, USB-C charging |
| Expansion | 2.54mm 2x8 header (8-ch GPIO, NRF24L01-compatible) |
| Storage | microSD slot |
| Connectivity | WiFi + Bluetooth 5 + LoRa |

**Relevance:** Full-featured handheld node. Same ESP32-S3 + ES8311 audio codec as our
3.5B-C board. Could serve as a portable Meshtastic field terminal. The expansion header
allows connecting external sensors.

### Meshnology N32 (ESP32 LoRa V3 / Heltec WiFi LoRa 32 V3)

A minimal, low-cost Meshtastic node based on the Heltec WiFi LoRa 32 V3 design.

| Feature | Spec |
|---|---|
| MCU | ESP32-S3FN8 dual-core 240MHz |
| Memory | 8MB Flash (V3); V4: 2MB PSRAM + 16MB Flash |
| Display | 0.96" OLED |
| LoRa | SX1262, 21dBm TX, -134dBm sensitivity, RF shielding |
| Battery | 3000mAh Li-ion (kit), USB-C |
| Connectivity | WiFi + Bluetooth 5 (LE) + LoRa |

**Relevance:** Cheapest path to a Meshtastic node. Good for outdoor relay/repeater
deployments. The 0.96" OLED is tiny but sufficient for node status. Available as
ready-to-go kits with antenna, battery, and 3D-printed case on Amazon.

### SenseCAP Solar Node P1-Pro

A weatherproof outdoor solar-powered Meshtastic node from Seeed Studio.

| Feature | Spec |
|---|---|
| MCU | XIAO nRF52840 Plus (NOT ESP32) |
| LoRa | Wio-SX1262 |
| GPS | XIAO L76K |
| Power | 5W solar panel + 4x 18650 (3350mAh each = 13.4Ah) |
| Antenna | Rod rubber antenna, 868-915MHz, 2dBi |
| Range | 8-9km in open areas |
| Temp Range | -40 to 60C (discharge), 0 to 50C (charging) |
| Expansion | Grove interface (I2C/GPIO) |
| Connectivity | Bluetooth 5.0 + LoRa |
| Firmware | Pre-installed Meshtastic |

**Relevance:** Purpose-built outdoor infrastructure node. The massive battery + solar
means indefinite operation. Ideal as a mesh relay/repeater on rooftops, towers, or
remote locations. Grove interface allows environmental sensors. Note: nRF52840 MCU,
not ESP32, so no WiFi -- Bluetooth only for configuration.

### Hardware Comparison

| | T-LoRa Pager | Meshnology N32 | SenseCAP P1-Pro |
|---|---|---|---|
| MCU | ESP32-S3 | ESP32-S3 | nRF52840 |
| Price | ~$60 | ~$25 (kit) | ~$90 |
| Display | 2.3" IPS | 0.96" OLED | None |
| Battery | 500mAh | 3000mAh | 13,400mAh + solar |
| GPS | Yes | No | Yes |
| WiFi | Yes | Yes | No |
| Use Case | Handheld terminal | Indoor relay/dev | Outdoor infrastructure |

---

## Mesh Topology and Routing

### Protocol Layers

Meshtastic uses a four-layer protocol stack:

**Layer 0 -- LoRa Radio:**
- Preamble length: 16 symbols
- Sync word: `0x2B`
- Modulation: LoRa spread spectrum

**Layer 1 -- Unreliable Zero Hop:**
- 16-byte header: destination, sender, packet ID, flags, channel hash, next-hop,
  relay node
- Payload: up to 237 bytes
- CSMA/CA with Channel Activity Detection before TX
- Adaptive contention windows based on channel utilization

**Layer 2 -- Reliable Zero Hop:**
- `WantAck` flag enables acknowledgments between neighbors
- Retry up to 3 times with airtime-aware expiration timers
- Broadcast ACKs are implicit (rebroadcast = ACK)

**Layer 3 -- Multi-Hop:**
- Two routing modes coexist:
  - **Managed Flood** for broadcasts
  - **Next-Hop Routing** for direct messages (since firmware 2.6)

### Managed Flood Routing (Broadcasts)

The primary routing mechanism. Every node that hears a packet with HopLimit > 0
decrements the limit and rebroadcasts, with these optimizations:

- **SNR-based contention:** Nodes with lower SNR (further away) get smaller contention
  windows and rebroadcast first. Closer nodes hear the rebroadcast and suppress their
  own, naturally extending range outward.
- **Duplicate suppression:** Nodes track recently-seen packet IDs to avoid loops.
- **Role priority:** ROUTER and REPEATER roles override SNR priority.

### Next-Hop Routing (Direct Messages, v2.6+)

For unicast messages, the system learns optimal routes:

1. First DM to a node uses managed flooding to discover path
2. Successful response identifies relay nodes
3. Future messages route through known next-hop relays
4. Falls back to managed flooding on final retry or topology changes

### Range and Performance

| Parameter | Typical Value |
|---|---|
| Line-of-sight range | 5-15km (ground level), 50+ km (elevated) |
| Max hop count | 7 (default 3) |
| Packet payload | Up to 237 bytes |
| Duty cycle | Region-dependent (1% in EU, 100% in US ISM) |
| Encryption | AES-256 on payload; headers unencrypted for routing |
| Node IDs | Bottom 4 bytes of MAC address |

### Traffic Scaling

Default broadcast intervals:
- Device telemetry: every 30 minutes
- Position updates: every 15 minutes (smart broadcast)
- NodeInfo: every 3 hours

For meshes with >40 nodes, intervals auto-scale:
```
ScaledInterval = Interval * (1.0 + ((NumOnlineNodes - 40) * 0.075))
```

### Can We Use Meshtastic as a Transport Layer?

**Yes, with caveats:**

Advantages:
- Zero infrastructure needed -- pure peer-to-peer
- Self-healing mesh with automatic route discovery
- AES-256 encryption built in
- Works where WiFi/cellular don't exist
- 237-byte payload per packet is enough for short commands/telemetry

Limitations:
- **Low bandwidth:** LoRa is ~1-10 kbps depending on spreading factor
- **High latency:** Each hop adds seconds of airtime + contention delay
- **Small packets:** 237 bytes max payload, no streaming
- **Duty cycle:** EU regions limit to 1% duty cycle
- **Not real-time:** Unsuitable for audio/video or rapid interactive control

**Best fit for Tritium:**
- Device status/telemetry reporting
- Alert propagation (motion detected, sensor threshold, etc.)
- Remote command dispatch (reboot, mode change, config push)
- Position tracking
- Bridging to MQTT/AI pipeline via tritium-sc gateway

---

## ESP32 LoRa Libraries

If we want to build custom LoRa firmware (not Meshtastic) on our ESP32-S3 boards.

### RadioLib

The leading universal radio library for embedded platforms.

| Feature | Details |
|---|---|
| Repository | [github.com/jgromes/RadioLib](https://github.com/jgromes/RadioLib) |
| PlatformIO | Officially registered: `jgromes/RadioLib` |
| LoRa Chips | SX1261, SX1262, SX1268, SX1272, SX1276-79, SX1280-82, LR1110/1120/1121 |
| FSK Chips | CC1101, RF69, RFM2x/9x, Si443x, SX123x |
| Other | nRF24L01, STM32WL |
| Protocols | LoRaWAN (Class A/C), AX.25, RTTY, Morse, SSTV, APRS, POCSAG |
| ESP32 | Full support via Arduino core |
| License | MIT |

**PlatformIO usage:**
```ini
[env:my-lora-board]
platform = espressif32
framework = arduino
lib_deps = jgromes/RadioLib@^7.2.1
```

**Basic LoRa send/receive with RadioLib:**
```cpp
#include <RadioLib.h>

// SX1262 on SPI with CS=8, DIO1=14, RESET=12, BUSY=13
SX1262 radio = new Module(8, 14, 12, 13);

void setup() {
    int state = radio.begin(915.0, 125.0, 9, 7, 0x12, 10);
    // freq=915MHz, bw=125kHz, sf=9, cr=4/7, syncWord=0x12, power=10dBm
}

void sendPacket() {
    radio.transmit("Hello LoRa");
}

void receivePacket() {
    String str;
    int state = radio.receive(str);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println(str);
    }
}
```

### LoRaMesher

A mesh networking library built on top of RadioLib, specifically for ESP32.

| Feature | Details |
|---|---|
| Repository | [github.com/LoRaMesher/LoRaMesher](https://github.com/LoRaMesher/LoRaMesher) |
| Built on | RadioLib |
| MCU | ESP32 family |
| Purpose | LoRa mesh networking without Meshtastic |

### SX12XX-LoRa

Standalone library for SX126x/SX127x/SX128x with no external dependencies.

| Feature | Details |
|---|---|
| Repository | [github.com/StuartsProjects/SX12XX-LoRa](https://github.com/StuartsProjects/SX12XX-LoRa) |
| Chips | SX1261-68, SX1272-79, SX1280-82 |
| Approach | Minimal, direct register access |

### Recommendation for Tritium

**Use Meshtastic firmware on dedicated LoRa nodes** rather than building custom LoRa
firmware. Reasons:
- Meshtastic handles mesh routing, encryption, channel management
- Proven at scale with large community
- Our ESP32 display boards don't have LoRa radios
- Connect to Meshtastic via UART (Serial Module) or USB (Python API)
- RadioLib is only needed if we build a custom LoRa board from scratch

---

## Commissioning and Provisioning

How Meshtastic devices are initially configured and joined to a mesh.

### First-Time Setup Flow

1. **Power on device** -- boots with default config, no region set
2. **Connect client** -- via Bluetooth (phone app), USB (CLI/web), or WiFi (ESP32 web)
3. **Set region** -- mandatory first step, selects frequency plan and regulatory limits
   - Android: Settings > LoRa > Region
   - CLI: `meshtastic --set lora.region US`
   - Web: client.meshtastic.org > LoRa > Regional Settings
4. **Set device name** -- optional but recommended for mesh identification
5. **Configure channels** -- default channel uses a known key; private meshes need
   custom channel with PSK

### Bluetooth Pairing

- Device advertises as "Meshtastic_XXXX" over BLE
- Default pairing PIN: `123456` (headless devices)
- Devices with screens display a random PIN
- PIN can be changed: `meshtastic --set bluetooth.fixedPin 123456`
- Pairing modes: Random PIN, Fixed PIN, No PIN

### QR Code Channel Sharing

Meshtastic generates QR codes that encode channel + LoRa settings:

- **Generate:** App > Settings > Share QR Code (select channels to share)
- **Scan:** Android QR scanner or iOS camera opens URL in Meshtastic app
- **URL format:** `https://meshtastic.org/e/#...` with base64-encoded channel config
- Scanning applies all included channel settings and LoRa configuration

### Programmatic Provisioning (for Tritium)

```python
import meshtastic
import meshtastic.serial_interface

interface = meshtastic.serial_interface.SerialInterface("/dev/ttyUSB0")

# Set region
interface.localNode.setConfig("lora", {"region": 1})  # 1 = US

# Set device name
interface.localNode.setOwner("tritium-relay-01")

# Configure channel with PSK
ch = interface.localNode.channels[0]
ch.settings.name = "tritium"
ch.settings.psk = bytes.fromhex("your_256bit_key_here")
interface.localNode.writeChannel(0)

# Export config for cloning to other devices
# meshtastic --export-config > tritium-node.yaml
# meshtastic --configure tritium-node.yaml  (apply to new device)
```

### Bulk Provisioning Strategy

For deploying multiple Tritium mesh nodes:
1. Create a master config YAML: `meshtastic --export-config > tritium-mesh.yaml`
2. Flash Meshtastic firmware to each device
3. Apply config: `meshtastic --configure tritium-mesh.yaml --port /dev/ttyUSBx`
4. Or use QR code for phone-based field provisioning
5. tritium-sc can automate this via the Python API on first device connection

---

## Tritium Integration Strategy

### Architecture Overview

```
                    LoRa Mesh
                   ┌─────────┐
  Outdoor Nodes ──>│Meshtastic│<── Handheld Pager
  (SenseCAP P1)   │  Mesh    │    (T-LoRa Pager)
                   └────┬────┘
                        │ LoRa
                   ┌────┴────┐
                   │ Gateway  │  Meshnology N32 / T-LoRa Pager
                   │  Node    │  (Meshtastic + UART or USB)
                   └────┬────┘
                        │ UART (TEXTMSG/PROTO) or USB
                   ┌────┴────┐
                   │tritium-  │  Waveshare ESP32-S3 display board
                   │  edge    │  (display + touch + sensors)
                   └────┬────┘
                        │ WiFi / MQTT
                   ┌────┴────┐
                   │tritium-  │  Python service
                   │   sc     │  (MQTT + AI pipeline)
                   └─────────┘
```

### Integration Points

**Option A: tritium-sc as Meshtastic Gateway (Recommended)**
- Connect Meshtastic radio to tritium-sc host via USB
- Use `meshtastic` Python library with `SerialInterface`
- Bridge mesh messages to/from MQTT topics
- Full access to node database, telemetry, position
- Can manage device config programmatically

**Option B: ESP32 Display Board as Meshtastic Gateway**
- Connect Meshtastic radio to ESP32-S3 via UART
- Use Serial Module in TEXTMSG or PROTO mode
- ESP32 bridges mesh messages to MQTT via WiFi
- Adds a display for local mesh status visualization

**Option C: Both (Recommended Production Setup)**
- tritium-sc handles all programmatic mesh access (monitoring, AI bridging)
- ESP32 display boards show local mesh status and enable touch interaction
- Multiple gateway nodes for redundancy

### Message Flow Examples

**Mesh alert to AI pipeline:**
```
SenseCAP sensor node -> LoRa mesh -> Gateway node -> UART -> tritium-edge
  -> WiFi/MQTT -> tritium-sc -> AI pipeline -> response
  -> MQTT -> tritium-edge -> UART -> Gateway -> LoRa mesh -> nodes
```

**Remote command to mesh node:**
```
tritium-sc -> SerialInterface -> Meshtastic radio -> LoRa mesh -> target node
```

### MQTT Topic Mapping

```
tritium/mesh/rx/{nodeId}/text      # Incoming text messages
tritium/mesh/rx/{nodeId}/position  # Position updates
tritium/mesh/rx/{nodeId}/telemetry # Device telemetry
tritium/mesh/tx/broadcast          # Outgoing broadcast
tritium/mesh/tx/{nodeId}           # Outgoing to specific node
tritium/mesh/nodes                 # Node database updates
```

### Next Steps

1. **Order hardware:** Meshnology N32 kit (cheapest dev entry point, ~$25)
2. **Flash Meshtastic** on N32, verify basic mesh operation with phone app
3. **Python integration:** Write tritium-sc Meshtastic bridge service using
   `meshtastic` Python library
4. **UART bridge:** Connect N32 to 3.5B-C via UART, test TEXTMSG mode
5. **Display app:** Build mesh status app for Waveshare boards (node list, map, chat)
6. **Outdoor node:** Deploy SenseCAP P1-Pro as outdoor relay
7. **Scale:** Add T-LoRa Pagers as portable field terminals

---

## Sources

- [Meshtastic Python API Documentation](https://python.meshtastic.org/)
- [Meshtastic Serial Module Configuration](https://meshtastic.org/docs/configuration/module/serial/)
- [Meshtastic Client API (Serial/TCP/BLE)](https://meshtastic.org/docs/development/device/client-api/)
- [Meshtastic Mesh Broadcast Algorithm](https://meshtastic.org/docs/overview/mesh-algo/)
- [Why Meshtastic Uses Managed Flood Routing](https://meshtastic.org/blog/why-meshtastic-uses-managed-flood-routing/)
- [Meshtastic 2.6: Next-Hop Routing](https://meshtastic.org/blog/meshtastic-2-6-preview/)
- [Meshtastic Initial Configuration](https://meshtastic.org/docs/getting-started/initial-config/)
- [Meshtastic Bluetooth Settings](https://meshtastic.org/docs/configuration/radio/bluetooth/)
- [RadioLib GitHub](https://github.com/jgromes/RadioLib)
- [Lilygo T-LoRa Pager Wiki](https://wiki.lilygo.cc/get_started/en/LoRa_GPS/T-LoraPager/T-LoraPager.html)
- [Lilygo T-LoRa Pager Review (AndiBond)](https://www.andibond.com/lilygo-t-lora-pager-review/)
- [Meshnology N32 Product Page](https://meshnology.com/products/esp32-lora-v3-dev-board-kit-sx1262-915mhz-antennas-3000mah-battery-case-n32)
- [Heltec WiFi LoRa 32 V3](https://heltec.org/project/wifi-lora-32-v3/)
- [SenseCAP Solar Node P1-Pro (Seeed Studio)](https://www.seeedstudio.com/SenseCAP-Solar-Node-P1-Pro-for-Meshtastic-LoRa-p-6412.html)
- [SenseCAP Solar Node Meshtastic Docs](https://meshtastic.org/docs/hardware/devices/seeed-studio/sensecap/solar-node/)
- [LoRaMesher (Hackster.io)](https://www.hackster.io/news/joan-miquel-sole-s-loramesher-builds-lora-mesh-networks-on-espressif-esp32-microcontrollers-b546e249bcd8)
- [Meshtastic Wikipedia](https://en.wikipedia.org/wiki/Meshtastic)
