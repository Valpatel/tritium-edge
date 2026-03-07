#pragma once
#include <cstdint>
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
enum class MeshMsgType : uint8_t {
    DATA = 0x01,        // Application data
    PING = 0x02,        // Mesh discovery ping
    PONG = 0x03,        // Mesh discovery response
    ROUTE = 0x04,       // Route advertisement
    ACK = 0x05,         // Delivery acknowledgment
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

    // Peer table
    int getPeerCount() const;
    int getPeers(EspNowPeer* peers, int maxPeers) const;
    bool isPeerKnown(const uint8_t mac[6]) const;

    // Callbacks
    void onReceive(EspNowRecvCb cb);      // Raw ESP-NOW receive
    void onMeshReceive(EspNowMeshCb cb);  // Mesh-routed data

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

    // Stats
    Stats _stats = {};

    // Dedup ring buffer
    static constexpr int DEDUP_SIZE = 64;
    uint16_t _dedupBuf[DEDUP_SIZE] = {};
    int _dedupIdx = 0;

    // Mesh message ID counter
    uint16_t _nextMsgId = 0;

    // Periodic discovery timer
    uint32_t _lastDiscoveryMs = 0;
    static constexpr uint32_t DISCOVERY_INTERVAL_MS = 10000;
    static constexpr uint32_t PEER_EXPIRY_MS = 30000;

    // Internal helpers
    bool isDuplicate(uint16_t msgId);
    void recordMsgId(uint16_t msgId);
    int findPeer(const uint8_t mac[6]) const;
    void updatePeer(const uint8_t mac[6], int8_t rssi, uint8_t hopCount, bool isDirect);
    void expirePeers();
    void buildMeshHeader(MeshHeader& hdr, MeshMsgType type, const uint8_t dst[6], uint8_t payloadLen);
    bool relayPacket(const uint8_t* rawPacket, uint8_t rawLen);
    void handleMeshPacket(const uint8_t* senderMac, const uint8_t* data, int len, int8_t rssi);

    // Static callback trampolines (ESP-NOW requires C function pointers)
    static EspNowHAL* _instance;
    static void onRecvStatic(const uint8_t* mac, const uint8_t* data, int len);
    static void onSendStatic(const uint8_t* mac, uint8_t status);
};
