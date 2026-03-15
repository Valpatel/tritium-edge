#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

static constexpr int ESPNOW_MAX_PEERS = 20;
static constexpr int ESPNOW_MAX_DATA = 240;  // 250 - header
static constexpr int ESPNOW_MESH_MAX_HOPS = 5;
static constexpr uint8_t ESPNOW_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

enum class EspNowRole : uint8_t {
    NODE,       // Regular mesh node (send + receive + relay)
    GATEWAY,    // Mesh gateway (connected to WiFi, bridges to MQTT/HTTP)
    LEAF,       // Leaf node (send + receive, no relay -- saves power)
};

// Message types for mesh protocol
// Message types for mesh protocol
enum class MeshMsgType : uint8_t {
    DATA = 0x01,        // Application data
    PING = 0x02,        // Mesh discovery ping
    PONG = 0x03,        // Mesh discovery response
    ROUTE = 0x04,       // Route advertisement
    ACK = 0x05,         // Delivery acknowledgment
};

// Application-level data subtypes (first byte of DATA payload)
enum class MeshDataType : uint8_t {
    RAW         = 0x00, // Untyped application data
    SIGHTING    = 0x10, // BLE/WiFi sighting relay
    CLASSIFY    = 0x11, // Device classification result
    CHAT        = 0x20, // Text chat message
    COMMAND     = 0x30, // Remote command
};

// Compact classification relay packet (fits in ESP-NOW payload)
// Broadcasts a device classification result so other nodes benefit
// without needing to re-classify the same device.
struct __attribute__((packed)) ClassifyRelay {
    MeshDataType subtype;       // = MeshDataType::CLASSIFY
    uint8_t  device_mac[6];     // MAC of classified device
    int8_t   rssi;              // RSSI when classified
    uint8_t  class_id;          // Device class enum (phone=1, watch=2, etc.)
    uint8_t  confidence;        // 0-100 confidence percentage
    uint8_t  name_len;          // Length of device name (0 if none)
    // Followed by name_len bytes of device name (null-terminated)
};

// Device class IDs for compact relay (matches BLE classifier categories)
enum class DeviceClassId : uint8_t {
    UNKNOWN         = 0,
    PHONE           = 1,
    WATCH           = 2,
    TABLET          = 3,
    LAPTOP          = 4,
    DESKTOP         = 5,
    EARBUDS         = 6,
    HEADPHONES      = 7,
    SPEAKER         = 8,
    TRACKER         = 9,
    CAMERA          = 10,
    SMART_HOME      = 11,
    VEHICLE         = 12,
    GAME_CONSOLE    = 13,
    PRINTER         = 14,
    MICROCONTROLLER = 15,
    MESH_RADIO      = 16,
    LIGHT           = 17,
    MEDIA_PLAYER    = 18,
    HOTSPOT         = 19,
    WEARABLE        = 20,
};

// Mesh packet header (prepended to all messages)
struct __attribute__((packed)) MeshHeader {
    uint8_t magic;          // 0xE5 (ESP-NOW mesh identifier)
    MeshMsgType type;
    uint8_t src[6];         // Original source MAC
    uint8_t dst[6];         // Final destination MAC (FF:FF:FF:FF:FF:FF = broadcast)
    uint16_t msg_id;        // Unique message ID (for dedup)
    uint8_t ttl;            // Time-to-live (hops remaining)
    uint8_t hop_count;      // Hops traversed so far
    uint8_t payload_len;
};

struct EspNowPeer {
    uint8_t mac[6];
    int8_t rssi;            // Last seen signal strength
    uint8_t hop_count;      // Hops to reach this peer (0 = direct)
    uint32_t last_seen_ms;  // millis() of last contact
    bool is_direct;         // Direct radio contact vs known via mesh
};

using EspNowRecvCb = std::function<void(const uint8_t* src_mac, const uint8_t* data, uint8_t len, int8_t rssi)>;
using EspNowMeshCb = std::function<void(const uint8_t* origin_mac, const uint8_t* data, uint8_t len, uint8_t hops)>;

class EspNowHAL {
public:
    // Lifecycle
    bool init(EspNowRole role = EspNowRole::NODE, uint8_t channel = 1);
    void deinit();
    bool isReady() const;

    // Identity
    void getMAC(uint8_t mac[6]) const;
    EspNowRole getRole() const;

    // Direct peer-to-peer
    bool addPeer(const uint8_t mac[6]);
    bool removePeer(const uint8_t mac[6]);
    bool send(const uint8_t dst_mac[6], const uint8_t* data, uint8_t len);
    bool broadcast(const uint8_t* data, uint8_t len);

    // Mesh networking
    bool meshSend(const uint8_t dst_mac[6], const uint8_t* data, uint8_t len);
    bool meshBroadcast(const uint8_t* data, uint8_t len);
    void meshDiscovery();  // Send ping to discover neighbors

    // Reliable mesh send (with ACK + retry)
    bool meshSendReliable(const uint8_t dst_mac[6], const uint8_t* data, uint8_t len);

    // Route table access
    int getRouteCount() const;

    // Peer table
    int getPeerCount() const;
    int getPeers(EspNowPeer* peers, int maxPeers) const;
    bool isPeerKnown(const uint8_t mac[6]) const;

    // Callbacks
    void onReceive(EspNowRecvCb cb);      // Raw ESP-NOW receive
    void onMeshReceive(EspNowMeshCb cb);  // Mesh-routed data

    // Classification relay — broadcast device classification to mesh peers
    // so other nodes don't need to re-classify the same BLE device.
    bool broadcastClassification(const uint8_t device_mac[6], int8_t rssi,
                                  DeviceClassId class_id, uint8_t confidence,
                                  const char* device_name = nullptr);

    // Callback for received classification relays from other nodes
    using ClassifyRelayCallback = std::function<void(const ClassifyRelay& relay,
                                                      const char* name)>;
    void onClassifyRelay(ClassifyRelayCallback cb);

    // Mesh maintenance -- call in loop()
    void process();

    // Statistics
    struct Stats {
        uint32_t tx_count;
        uint32_t rx_count;
        uint32_t tx_fail;
        uint32_t relay_count;       // Messages relayed for others
        uint32_t dup_dropped;       // Duplicate messages dropped
        uint32_t ttl_expired;       // Messages that ran out of TTL
        uint32_t discovery_count;   // Discovery rounds completed
        uint32_t ack_received;      // ACKs received for our messages
        uint32_t ack_timeout;       // Messages that timed out without ACK
        uint32_t route_count;       // Current route table entries
    };
    Stats getStats() const;
    void resetStats();

    // Test harness
    struct TestResult {
        bool init_ok;
        bool mac_ok;
        bool broadcast_ok;
        bool peer_add_ok;
        bool send_ok;
        bool mesh_discovery_ok;
        int peers_found;
        int8_t best_rssi;
        int8_t worst_rssi;
        uint32_t avg_rtt_us;      // Round-trip time for ping/pong
        uint32_t test_duration_ms;
        Stats stats;
    };
    // Run discovery + ping test. duration_s = how long to listen for peers.
    TestResult runTest(int duration_s = 10);

    // ── Diagnostics integration ──────────────────────────────────────────

    /// Mesh health metrics for diagnostic snapshots.
    struct MeshHealth {
        int      peer_count;          // Current known peers
        int      direct_peers;        // Peers reachable in 1 hop
        uint32_t msgs_sent;           // Total tx_count
        uint32_t msgs_received;       // Total rx_count
        uint32_t msgs_relayed;        // Total relay_count
        uint32_t send_failures;       // Total tx_fail
        float    packet_loss_rate;    // tx_fail / (tx_count + tx_fail), 0.0-1.0
        float    avg_hop_count;       // Average hop count across known peers
        int8_t   best_rssi;           // Best RSSI across peers
        int8_t   worst_rssi;          // Worst RSSI across peers
        uint32_t discovery_rounds;    // Total discovery cycles
        uint32_t uptime_ms;           // Time since init()
    };

    /// Get current mesh health metrics.
    MeshHealth getMeshHealth() const;

    /// Serialize mesh topology to JSON for /api/mesh endpoint.
    /// Returns bytes written, or -1 on error.
    /// JSON format: {"self":{mac,role,channel},"peers":[{mac,rssi,hops,direct,age_ms}],"stats":{...}}
    int meshToJson(char* buf, size_t size) const;

    /// Enable diagnostic event logging to hal_diag (if ENABLE_DIAG is defined).
    /// Call after both hal_espnow and hal_diag are initialized.
    void enableDiagLogging(bool enable = true);

private:
    bool _ready = false;
    EspNowRole _role = EspNowRole::NODE;
    uint8_t _channel = 1;
    uint8_t _mac[6] = {};

    // Peer table
    EspNowPeer _peers[ESPNOW_MAX_PEERS] = {};
    int _peerCount = 0;

    // Callbacks
    EspNowRecvCb _recvCb = nullptr;
    EspNowMeshCb _meshCb = nullptr;
    ClassifyRelayCallback _classifyCb = nullptr;

    // Stats
    Stats _stats = {};

    // Dedup ring buffer
    static constexpr int DEDUP_SIZE = 64;
    uint16_t _dedupBuf[DEDUP_SIZE] = {};
    int _dedupIdx = 0;

    // Mesh message ID counter
    uint16_t _nextMsgId = 0;

    // Route table: destination MAC -> next-hop MAC + metrics
    struct RouteEntry {
        uint8_t dest[6];        // Destination MAC
        uint8_t next_hop[6];    // Best next-hop MAC (direct neighbor)
        uint8_t hop_count;      // Total hops to destination
        int8_t  rssi;           // RSSI of next-hop link
        uint32_t last_updated_ms;
        bool    valid;
    };
    static constexpr int MAX_ROUTES = 32;
    RouteEntry _routes[MAX_ROUTES] = {};
    int _routeCount = 0;

    void updateRoute(const uint8_t dest[6], const uint8_t next_hop[6],
                     uint8_t hops, int8_t rssi);
    int findRoute(const uint8_t dest[6]) const;
    void expireRoutes();
    void advertiseRoutes();  // Broadcast ROUTE messages with our known peers

    // Reliable delivery: pending ACK tracking
    struct PendingAck {
        uint16_t msg_id;
        uint8_t  dest[6];
        uint8_t  data[250];
        uint8_t  data_len;
        uint8_t  retries;
        uint32_t sent_ms;
        bool     active;
    };
    static constexpr int MAX_PENDING_ACKS = 4;
    static constexpr int MAX_RETRIES = 3;
    static constexpr uint32_t ACK_TIMEOUT_MS = 500;
    PendingAck _pendingAcks[MAX_PENDING_ACKS] = {};
    void sendAck(const uint8_t dest_mac[6], uint16_t msg_id);
    void processAcks();
    void handleAck(uint16_t msg_id);

    // Periodic timers
    uint32_t _lastDiscoveryMs = 0;
    uint32_t _lastRouteAdvMs = 0;
    static constexpr uint32_t DISCOVERY_INTERVAL_MS = 10000;
    static constexpr uint32_t ROUTE_ADV_INTERVAL_MS = 15000;
    static constexpr uint32_t PEER_EXPIRY_MS = 30000;
    static constexpr uint32_t ROUTE_EXPIRY_MS = 45000;

    // Diagnostics integration
    bool _diagEnabled = false;
    uint32_t _initTimeMs = 0;
    int _prevPeerCount = 0;     // For detecting connect/disconnect events
    void logDiagEvent(const char* fmt, ...);
    void checkPeerChanges();

    // Internal helpers
    bool isDuplicate(uint16_t msgId);
    void recordMsgId(uint16_t msgId);
    int findPeer(const uint8_t mac[6]) const;
    void updatePeer(const uint8_t mac[6], int8_t rssi, uint8_t hopCount, bool isDirect);
    void expirePeers();
    void buildMeshHeader(MeshHeader& hdr, MeshMsgType type, const uint8_t dst[6], uint8_t payloadLen);
    bool relayPacket(const uint8_t* rawPacket, uint8_t rawLen);
    void handleMeshPacket(const uint8_t* senderMac, const uint8_t* data, int len, int8_t rssi);
    void handleClassifyRelay(const uint8_t* payload, uint8_t len);

    // Static callback trampolines (ESP-NOW requires C function pointers)
    static EspNowHAL* _instance;
    static void onRecvStatic(const uint8_t* mac, const uint8_t* data, int len);
    static void onSendStatic(const uint8_t* mac, uint8_t status);
};
