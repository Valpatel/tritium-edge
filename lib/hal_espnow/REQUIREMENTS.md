# ESP-NOW Mesh Networking Requirements and Features

## Overview

The ESP-NOW HAL provides peer-to-peer wireless communication and multi-hop mesh networking between ESP32-S3 devices. Built on Espressif's ESP-NOW protocol (connectionless, 250-byte packets over WiFi PHY), it adds mesh routing with flooding-based forwarding, automatic peer discovery via PING/PONG, duplicate message suppression, and per-peer RSSI tracking.

## Architecture

```
                     +-------------------+
                     |   Application     |
                     |  (System, OTA)    |
                     +--------+----------+
                              |
               onMeshReceive  |  meshSend / meshBroadcast
                              |
                     +--------+----------+
                     |    EspNowHAL      |
                     |                   |
                     |  Peer Table       |   <- up to 20 peers, auto-expiry
                     |  Dedup Ring Buf   |   <- 64-entry msg_id dedup
                     |  Mesh Router      |   <- flood relay for NODE/GATEWAY
                     |  Discovery        |   <- periodic PING/PONG
                     |  Stats            |   <- tx/rx/relay/dup counters
                     +--------+----------+
                              |
                   ESP-NOW    |  broadcast / unicast
                              |
              +---------------+---------------+
              |               |               |
         +---------+    +---------+    +---------+
         | Device  |    | Device  |    | Device  |
         | (NODE)  |    |(GATEWAY)|    | (LEAF)  |
         +---------+    +---------+    +---------+
```

## Node Roles

| Role | Send | Receive | Relay | Description |
|------|------|---------|-------|-------------|
| `NODE` | Yes | Yes | Yes | Standard mesh participant, forwards packets for others |
| `GATEWAY` | Yes | Yes | Yes | WiFi-connected node, bridges mesh to MQTT/HTTP |
| `LEAF` | Yes | Yes | No | Power-efficient endpoint, never relays |

## Mesh Protocol

### Packet Format

All mesh packets begin with a 20-byte `MeshHeader`:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | magic | `0xE5` (mesh identifier) |
| 1 | 1 | type | `MeshMsgType` enum |
| 2 | 6 | src | Original source MAC address |
| 8 | 6 | dst | Final destination MAC (`FF:FF:FF:FF:FF:FF` = broadcast) |
| 14 | 2 | msg_id | Unique message ID (for deduplication) |
| 16 | 1 | ttl | Time-to-live (hops remaining) |
| 17 | 1 | hop_count | Hops traversed so far |
| 18 | 1 | payload_len | Application payload length |

Maximum payload per mesh packet: 240 - sizeof(MeshHeader) = 221 bytes.

Raw ESP-NOW packets (no mesh header) are also supported and passed directly to the `onReceive` callback.

### Message Types

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| `DATA` | 0x01 | Any -> Any | Application data, relayed by NODE/GATEWAY |
| `PING` | 0x02 | Broadcast | Mesh discovery ping (payload: role + peer count) |
| `PONG` | 0x03 | Unicast reply | Discovery response to PING sender |
| `ROUTE` | 0x04 | Broadcast | Route advertisement (reserved for future use) |
| `ACK` | 0x05 | Unicast | Delivery acknowledgment |

### Discovery Protocol

1. Every 10 seconds, `process()` triggers `meshDiscovery()`
2. Node broadcasts a `PING` with its role and current peer count as payload
3. Receiving nodes respond with a unicast `PONG` to the direct sender
4. `PING` packets are also relayed by NODE/GATEWAY roles for multi-hop discovery
5. Both direct senders and original sources are recorded in the peer table

### Message Routing

- **Direct peer known**: If the destination MAC is in the peer table and marked as direct, the packet is sent unicast
- **Unknown destination**: Packet is broadcast (flood) to all neighbors
- **Relay**: NODE and GATEWAY roles re-broadcast received packets with decremented TTL and incremented hop_count
- **LEAF nodes**: Never relay, reducing radio traffic for battery-powered devices

### Deduplication

- 64-entry ring buffer of `msg_id` values
- Every received mesh packet is checked against the dedup buffer
- Duplicates are silently dropped and counted in `dup_dropped` stat
- Own outgoing `msg_id` values are pre-recorded to prevent processing relayed copies of own messages

### Peer Management

- Peer table holds up to 20 entries (`ESPNOW_MAX_PEERS`)
- Each entry tracks: MAC, RSSI, hop count, direct/indirect flag, last-seen timestamp
- Peers expire after 30 seconds of inactivity (`PEER_EXPIRY_MS`)
- Hop count is updated when a shorter route is discovered
- New peers are automatically registered with the ESP-NOW driver for unicast

### TTL and Hop Limiting

- Initial TTL: 5 hops (`ESPNOW_MESH_MAX_HOPS`)
- TTL decremented on each relay; packets with TTL=0 are dropped
- `ttl_expired` stat tracks expired packets
- Hop count incremented on relay (informational, not used for routing decisions)

## API Reference

### Lifecycle

```cpp
EspNowHAL espnow;

// Initialize with role and WiFi channel
bool ok = espnow.init(EspNowRole::NODE, 1);

// Call every loop iteration for discovery + peer expiry
espnow.process();

// Shut down
espnow.deinit();
```

### Identity

```cpp
uint8_t mac[6];
espnow.getMAC(mac);
EspNowRole role = espnow.getRole();
```

### Direct Peer-to-Peer (No Mesh Header)

```cpp
// Send raw ESP-NOW packet (no mesh routing)
espnow.send(peerMac, data, len);
espnow.broadcast(data, len);

// Receive raw packets
espnow.onReceive([](const uint8_t* src_mac, const uint8_t* data, uint8_t len, int8_t rssi) {
    // Handle raw packet
});
```

### Mesh Networking

```cpp
// Send with mesh header (routed, deduped, relayed)
espnow.meshSend(destMac, data, len);
espnow.meshBroadcast(data, len);

// Receive mesh-routed data
espnow.onMeshReceive([](const uint8_t* origin_mac, const uint8_t* data, uint8_t len, uint8_t hops) {
    // Handle mesh data (origin_mac is the original sender, not the relay)
});

// Trigger immediate discovery
espnow.meshDiscovery();
```

### Peer Table

```cpp
int count = espnow.getPeerCount();

EspNowPeer peers[20];
int n = espnow.getPeers(peers, 20);
for (int i = 0; i < n; i++) {
    printf("Peer %02X:%02X:%02X:%02X:%02X:%02X rssi=%d hops=%d direct=%d\n",
           peers[i].mac[0], peers[i].mac[1], peers[i].mac[2],
           peers[i].mac[3], peers[i].mac[4], peers[i].mac[5],
           peers[i].rssi, peers[i].hop_count, peers[i].is_direct);
}

bool known = espnow.isPeerKnown(someMac);
```

### Statistics

```cpp
EspNowHAL::Stats stats = espnow.getStats();
// stats.tx_count, stats.rx_count, stats.tx_fail
// stats.relay_count, stats.dup_dropped, stats.ttl_expired
// stats.discovery_count
espnow.resetStats();
```

### Test Harness

```cpp
// Run automated test: init, broadcast, discovery, peer ping
EspNowHAL::TestResult r = espnow.runTest(10);  // 10-second listen window
// r.init_ok, r.mac_ok, r.broadcast_ok, r.peer_add_ok, r.send_ok
// r.mesh_discovery_ok, r.peers_found, r.best_rssi, r.worst_rssi
// r.avg_rtt_us, r.test_duration_ms, r.stats
```

## Integration with OTA Mesh

The `OtaMesh` class (in `lib/hal_ota/ota_mesh.h`) builds on `EspNowHAL` for peer-to-peer firmware distribution:

1. `OtaMesh::init(&espnow)` registers a mesh receive callback via `espnow.onMeshReceive()`
2. Sender broadcasts `OTA_ANNOUNCE` via `espnow.meshBroadcast()` with firmware metadata
3. Receiver responds with `OTA_REQUEST` via `espnow.meshSend()` to the sender MAC
4. Firmware is transferred in 200-byte chunks with a 4-chunk sliding window
5. Each chunk/ACK/status message uses `meshSend()` for point-to-point delivery
6. Multi-hop relay is handled transparently by EspNowHAL's mesh router

The OtaMesh protocol adds its own packet types (0x40-0x45) inside the mesh DATA payload, keeping the mesh transport and OTA application cleanly separated.

## RSSI Tracking

- WiFi promiscuous mode is enabled to capture per-frame RSSI
- A promiscuous callback stores the RSSI of each management frame
- The stored RSSI is associated with the next ESP-NOW receive callback
- RSSI is tracked per-peer in the peer table and updated on every contact

## Configuration Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `ESPNOW_MAX_PEERS` | 20 | Maximum tracked peers |
| `ESPNOW_MAX_DATA` | 240 | Maximum payload per ESP-NOW packet |
| `ESPNOW_MESH_MAX_HOPS` | 5 | Initial TTL for mesh packets |
| `DEDUP_SIZE` | 64 | Deduplication ring buffer entries |
| `DISCOVERY_INTERVAL_MS` | 10,000 | Auto-discovery period |
| `PEER_EXPIRY_MS` | 30,000 | Peer inactivity timeout |

## Limitations

- **No encryption**: ESP-NOW encryption is disabled (`encrypt = false`). Application-layer encryption should be used for sensitive data.
- **Flooding router**: Mesh uses simple flooding, not routing tables. Every relayed packet is broadcast to all neighbors. This is simple and robust for small meshes (<20 nodes) but does not scale to large networks.
- **Single instance**: Static `_instance` pointer limits to one `EspNowHAL` per process. The ESP-NOW driver is a global singleton anyway.
- **Channel fixed at init**: All nodes must use the same WiFi channel. No channel hopping.
- **No guaranteed delivery**: `meshSend()` uses best-effort flooding. The ACK message type is defined but not used for automatic retransmission.
- **Dedup collisions**: 16-bit `msg_id` wraps at 65535. With high message rates from many nodes, dedup false positives are possible. The 64-entry ring buffer limits the window.
- **WiFi coexistence**: `WiFi.mode(WIFI_STA)` is set on init. ESP-NOW and WiFi STA can coexist but share the radio. Active WiFi connections may delay ESP-NOW delivery.

## Platform Support

- **ESP32**: Full implementation using `esp_now.h`, `esp_wifi.h`, WiFi promiscuous mode
- **Simulator**: Stub implementation with no actual radio. `init()` succeeds, all send/receive operations return false. Useful for compile-testing application logic.

## File Structure

```
lib/hal_espnow/
  hal_espnow.h       -- Class declaration, MeshHeader, EspNowPeer, enums
  hal_espnow.cpp     -- ESP32 implementation + simulator stubs
  REQUIREMENTS.md    -- This file
```
