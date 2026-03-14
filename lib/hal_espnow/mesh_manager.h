// Tritium-OS Mesh Manager — enhanced mesh networking layer.
// Wraps EspNowHAL with addressed messaging, shared state, topology
// tracking, gateway election, and JSON export for the web UI.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "mesh_protocol.h"
#include <cstddef>

class MeshManager {
public:
    static MeshManager& instance();

    // Lifecycle
    bool init(MeshRole role = MESH_ROLE_RELAY);
    void tick();
    bool isReady() const;

    // Identity
    void getMAC(uint8_t mac[6]) const;
    MeshRole getRole() const;
    void setRole(MeshRole role);
    void setDeviceName(const char* name);
    void setFirmwareVersion(const char* ver);
    void setBoardType(const char* board);
    void setBatteryPercent(uint8_t pct);

    // Messaging
    bool broadcast(const uint8_t* data, size_t len);
    bool sendTo(const uint8_t* dst_mac, const uint8_t* data, size_t len);

    // Sighting relay — forward BLE/WiFi sightings to gateway via mesh
    bool relaySighting(const MeshSightingPayload& sighting);
    bool relaySightingBatch(const MeshSightingPayload* sightings, int count);

    // Sighting receive callback (only fired on gateway nodes)
    typedef void (*SightingCallback)(const uint8_t* observer_mac,
                                      const MeshSightingPayload& sighting,
                                      void* ud);
    void onSighting(SightingCallback cb, void* user_data = nullptr);

    // Message callback
    typedef void (*MeshMessageCallback)(const MeshHeaderEx& hdr,
                                         const uint8_t* payload, void* ud);
    void onMessage(MeshMessageCallback cb, void* user_data = nullptr);

    // Peer management
    int  getPeers(MeshPeerInfo* out, int max_count) const;
    int  peerCount() const;
    bool pingPeer(const uint8_t* mac);
    const MeshPeerInfo* findPeer(const uint8_t* mac) const;

    // Shared state (distributed key-value store)
    bool        setState(const char* key, const char* value);
    const char* getState(const char* key) const;
    int         getAllState(MeshStateEntry* out, int max_count) const;
    int         stateCount() const;

    // Topology
    int  getTopology(MeshLink* out, int max_links) const;
    int  linkCount() const;
    bool isGateway() const;

    // Gateway election
    void electGateway();

    // Statistics
    struct MeshStats {
        uint32_t tx_count;
        uint32_t rx_count;
        uint32_t relay_count;
        uint32_t tx_fail;
        uint32_t dedup_drop;
        uint32_t state_syncs;
        int      peer_count;
        MeshRole role;
        bool     is_elected_gateway;
    };
    const MeshStats& getStats() const;

    // JSON export for web API
    int toJson(char* buf, size_t size) const;
    int peersToJson(char* buf, size_t size) const;
    int topologyToJson(char* buf, size_t size) const;
    int stateToJson(char* buf, size_t size) const;

    // Static callback — installed on EspNowHAL raw receive to intercept
    // enhanced frames (magic 0x54).  Must be public for service wiring.
    static void onRawReceive(const uint8_t* src_mac, const uint8_t* data,
                             uint8_t len, int8_t rssi);

private:
    MeshManager() = default;
    MeshManager(const MeshManager&) = delete;
    MeshManager& operator=(const MeshManager&) = delete;

    bool _ready = false;
    MeshRole _role = MESH_ROLE_RELAY;
    uint8_t  _mac[6] = {};

    // Device info (sent in PONG)
    char    _deviceName[MESH_DEVICE_NAME_LEN] = {};
    char    _firmwareVersion[MESH_FW_VERSION_LEN] = {};
    char    _boardType[MESH_BOARD_TYPE_LEN] = {};
    uint8_t _batteryPct = 0;

    // Peer table
    MeshPeerInfo _peers[MESH_MAX_PEERS] = {};
    int          _peerCount = 0;

    // Shared state table
    MeshStateEntry _state[MESH_MAX_STATE] = {};
    int            _stateCount = 0;

    // Topology links (assembled from TOPOLOGY announcements)
    MeshLink _links[MESH_MAX_LINKS] = {};
    int      _linkCount = 0;

    // Dedup ring buffer
    static constexpr int DEDUP_SIZE = 64;
    struct DedupEntry {
        uint8_t  src_mac[6];
        uint16_t seq;
    };
    DedupEntry _dedupBuf[DEDUP_SIZE] = {};
    int        _dedupIdx = 0;

    // Sequence counter
    uint16_t _nextSeq = 0;

    // Timers
    uint32_t _lastPingMs = 0;
    uint32_t _lastTopoMs = 0;
    uint32_t _lastStateSyncMs = 0;
    static constexpr uint32_t PING_INTERVAL_MS       = 30000;
    static constexpr uint32_t TOPO_INTERVAL_MS       = 45000;
    static constexpr uint32_t STATE_SYNC_INTERVAL_MS  = 60000;
    static constexpr uint32_t PEER_EXPIRY_MS          = 90000;

    // Message callback
    MeshMessageCallback _msgCb = nullptr;
    void*               _msgCbUserData = nullptr;

    // Sighting callback (gateway only)
    SightingCallback    _sightingCb = nullptr;
    void*               _sightingCbUserData = nullptr;

    // Stats
    MeshStats _stats = {};

    // Gateway election state
    bool _electedGateway = false;

    // Internal helpers
    bool isDuplicate(const uint8_t* src_mac, uint16_t seq);
    void recordDedup(const uint8_t* src_mac, uint16_t seq);
    void buildHeader(MeshHeaderEx& hdr, MeshMsgTypeEx type,
                     const uint8_t* dst, uint8_t payloadLen);
    void handlePacket(const uint8_t* senderMac, const uint8_t* data,
                      int len, int8_t rssi);
    bool relayPacket(const uint8_t* rawPacket, uint8_t rawLen);
    void handlePing(const MeshHeaderEx& hdr, const uint8_t* senderMac,
                    const uint8_t* payload, uint8_t payloadLen, int8_t rssi);
    void handlePong(const MeshHeaderEx& hdr, const uint8_t* payload,
                    uint8_t payloadLen, int8_t rssi);
    void handleStateSync(const MeshHeaderEx& hdr, const uint8_t* payload,
                         uint8_t payloadLen);
    void handleStateRequest(const MeshHeaderEx& hdr, const uint8_t* senderMac);
    void handleTopology(const MeshHeaderEx& hdr, const uint8_t* payload,
                        uint8_t payloadLen);
    int  findPeerIdx(const uint8_t* mac) const;
    void updatePeer(const uint8_t* mac, int8_t rssi, uint8_t hopCount);
    void updatePeerInfo(int idx, const MeshPongPayload& info);
    void expirePeers();
    int  findStateIdx(const char* key) const;
    void broadcastDiscovery();
    void broadcastTopology();
    void broadcastStateDump(const uint8_t* dst_mac);
    void addLink(const uint8_t* from, const uint8_t* to, int8_t rssi);
    uint8_t rssiToQuality(int8_t rssi) const;
};
