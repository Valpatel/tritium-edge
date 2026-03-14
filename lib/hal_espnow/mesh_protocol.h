// Tritium-OS Enhanced Mesh Protocol — wire format, peer info, shared state.
// Extends the existing ESP-NOW mesh with addressed messaging, distributed
// state, topology tracking, and encryption support.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <cstdint>

// ── Message types ──────────────────────────────────────────────────────────

enum MeshMsgTypeEx : uint8_t {
    MESH_EX_PING          = 0x01,  // Discovery ping
    MESH_EX_PONG          = 0x02,  // Discovery response (includes device info)
    MESH_EX_BROADCAST     = 0x10,  // Flood to all peers
    MESH_EX_ADDRESSED     = 0x11,  // Route to specific device
    MESH_EX_STATE_SYNC    = 0x20,  // Shared state update (single key)
    MESH_EX_STATE_REQUEST = 0x21,  // Request full state dump from peer
    MESH_EX_STATE_DUMP    = 0x22,  // Full state dump response
    MESH_EX_OTA_OFFER     = 0x30,  // Firmware available announcement
    MESH_EX_OTA_REQUEST   = 0x31,  // Request firmware chunk
    MESH_EX_OTA_CHUNK     = 0x32,  // Firmware data chunk
    MESH_EX_TOPOLOGY      = 0x40,  // Topology announcement (peer list)
    MESH_EX_SIGHTING      = 0x50,  // Sighting relay (BLE/WiFi device detection)
    MESH_EX_SIGHTING_ACK  = 0x51,  // Sighting delivery acknowledgment
};

// ── Mesh roles ──────────────────────────────────────────────────────────────

enum MeshRole : uint8_t {
    MESH_ROLE_GATEWAY = 0,  // Bridges mesh to WiFi/server
    MESH_ROLE_RELAY   = 1,  // Forwards messages
    MESH_ROLE_LEAF    = 2,  // Doesn't relay
    MESH_ROLE_SENSOR  = 3,  // Deep sleep, wake-transmit-sleep
};

// ── Enhanced mesh header ────────────────────────────────────────────────────
// Prepended to payload on the enhanced mesh channel.  Backwards-compatible:
// the existing EspNowHAL uses magic 0xE5; the enhanced protocol uses 0x54
// ('T' for Tritium), so old nodes safely ignore enhanced frames.

static constexpr uint8_t MESH_EX_MAGIC   = 0x54;  // 'T'
static constexpr uint8_t MESH_EX_VERSION = 1;

struct __attribute__((packed)) MeshHeaderEx {
    uint8_t      magic;         // MESH_EX_MAGIC (0x54)
    uint8_t      version;       // Protocol version (1)
    MeshMsgTypeEx type;
    uint8_t      ttl;           // Hop limit (max 5)
    uint8_t      src_mac[6];    // Original sender MAC
    uint8_t      dst_mac[6];    // Destination (FF:FF:FF:FF:FF:FF = broadcast)
    uint16_t     seq;           // Sequence number for dedup
    uint8_t      payload_len;   // Bytes following this header
    // payload follows immediately
};

// ── Peer info (maintained per discovered peer) ──────────────────────────────

static constexpr int MESH_MAX_PEERS       = 20;
static constexpr int MESH_MAX_STATE       = 32;
static constexpr int MESH_MAX_LINKS       = 40;
static constexpr int MESH_DEVICE_NAME_LEN = 16;
static constexpr int MESH_FW_VERSION_LEN  = 16;
static constexpr int MESH_BOARD_TYPE_LEN  = 24;

struct MeshPeerInfo {
    uint8_t  mac[6];
    MeshRole role;
    int8_t   rssi;
    uint8_t  hop_count;
    uint32_t last_seen_ms;
    char     device_name[MESH_DEVICE_NAME_LEN];
    char     firmware_version[MESH_FW_VERSION_LEN];
    uint8_t  battery_pct;
    char     board_type[MESH_BOARD_TYPE_LEN];
    bool     is_gateway;

    // Peer quality tracking — accumulated over time
    int32_t  rssi_sum;          // running sum for average
    uint16_t rssi_samples;      // number of RSSI samples taken
    int8_t   rssi_min;          // worst RSSI seen
    int8_t   rssi_max;          // best RSSI seen
    uint32_t tx_count;          // packets sent to this peer
    uint32_t rx_count;          // packets received from this peer
    uint32_t tx_fail;           // failed transmissions to this peer
    uint32_t first_seen_ms;     // uptime when first discovered
};

// ── PONG payload — sent as response to PING ─────────────────────────────────

struct __attribute__((packed)) MeshPongPayload {
    uint8_t  role;
    uint8_t  peer_count;
    uint8_t  battery_pct;
    char     device_name[MESH_DEVICE_NAME_LEN];
    char     firmware_version[MESH_FW_VERSION_LEN];
    char     board_type[MESH_BOARD_TYPE_LEN];
};

// ── Shared state entry ──────────────────────────────────────────────────────

static constexpr int MESH_STATE_KEY_LEN   = 16;
static constexpr int MESH_STATE_VALUE_LEN = 32;

struct MeshStateEntry {
    char     key[MESH_STATE_KEY_LEN];
    char     value[MESH_STATE_VALUE_LEN];
    uint8_t  src_mac[6];
    uint32_t timestamp_ms;
    uint16_t version;  // Increment on update; highest version wins (LWW-CRDT)
};

// ── State sync payload (single key update) ──────────────────────────────────

struct __attribute__((packed)) MeshStateSyncPayload {
    char     key[MESH_STATE_KEY_LEN];
    char     value[MESH_STATE_VALUE_LEN];
    uint16_t version;
};

// ── Topology link ───────────────────────────────────────────────────────────

struct MeshLink {
    uint8_t from_mac[6];
    uint8_t to_mac[6];
    int8_t  rssi;
    uint8_t quality;  // 0-100 (derived from RSSI)
};

// ── Topology announcement payload ───────────────────────────────────────────
// Each node periodically broadcasts its direct neighbor list.

static constexpr int MESH_TOPO_MAX_NEIGHBORS = 8;

struct __attribute__((packed)) MeshTopoNeighbor {
    uint8_t mac[6];
    int8_t  rssi;
};

struct __attribute__((packed)) MeshTopoPayload {
    uint8_t         neighbor_count;
    MeshTopoNeighbor neighbors[MESH_TOPO_MAX_NEIGHBORS];
};

// ── Sighting relay payload ──────────────────────────────────────────────────
// Compact sighting report for mesh relay to gateway.  Nodes without WiFi/MQTT
// use this to forward BLE/WiFi sightings through the mesh toward a gateway
// node, which then publishes them to MQTT.

enum MeshSightingType : uint8_t {
    MESH_SIGHTING_BLE  = 0x01,
    MESH_SIGHTING_WIFI = 0x02,
};

struct __attribute__((packed)) MeshSightingPayload {
    MeshSightingType type;          // BLE or WiFi
    uint8_t  detected_mac[6];       // MAC of detected device
    int8_t   rssi;                  // Signal strength
    uint8_t  device_class;          // BLE device class (0=unknown)
    uint16_t detection_count;       // Times seen by this node
    uint32_t first_seen_ms;         // Uptime ms when first detected
    uint32_t last_seen_ms;          // Uptime ms when last detected
    uint8_t  name_len;              // Length of optional name (0-31)
    // Followed by `name_len` bytes of device name (not null-terminated)
};

// Maximum sightings in a batch relay
static constexpr int MESH_SIGHTING_BATCH_MAX = 5;
