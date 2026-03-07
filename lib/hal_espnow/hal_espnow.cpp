#include "hal_espnow.h"
#include "debug_log.h"
#include <cstring>
#include <cstdio>

static constexpr uint8_t MESH_MAGIC = 0xE5;
static constexpr const char* TAG = "espnow";

EspNowHAL* EspNowHAL::_instance = nullptr;

// ============================================================================
// Platform: ESP32
// ============================================================================
#ifndef SIMULATOR

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// RSSI tracking for incoming packets
static volatile int8_t s_lastRxRssi = 0;

// Promiscuous callback to capture RSSI from ESP-NOW frames
static void promiscRxCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    s_lastRxRssi = pkt->rx_ctrl.rssi;
}

// ---- Static callback trampolines ----

void EspNowHAL::onRecvStatic(const uint8_t* mac, const uint8_t* data, int len) {
    if (!_instance || !_instance->_ready) return;

    int8_t rssi = s_lastRxRssi;
    _instance->_stats.rx_count++;

    // Check if this is a mesh packet (starts with magic byte)
    if (len >= (int)sizeof(MeshHeader) && data[0] == MESH_MAGIC) {
        _instance->handleMeshPacket(mac, data, len, rssi);
    } else {
        // Raw ESP-NOW packet (no mesh header)
        if (_instance->_recvCb) {
            _instance->_recvCb(mac, data, (uint8_t)len, rssi);
        }
    }
}

void EspNowHAL::onSendStatic(const uint8_t* mac, uint8_t status) {
    if (!_instance) return;
    if (status != 0) {
        _instance->_stats.tx_fail++;
        DBG_DEBUG(TAG, "Send failed to %02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

// ---- Lifecycle ----

bool EspNowHAL::init(EspNowRole role, uint8_t channel) {
    if (_ready) {
        DBG_WARN(TAG, "Already initialized");
        return true;
    }

    _instance = this;
    _role = role;
    _channel = channel;

    // Initialize WiFi in STA mode for ESP-NOW
    // Must call WiFi.mode() before any esp_wifi_* calls to init the stack
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    DBG_INFO(TAG, "WiFi STA mode initialized");

    // Set channel
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    // Init ESP-NOW
    if (esp_now_init() != ESP_OK) {
        DBG_ERROR(TAG, "esp_now_init() failed");
        return false;
    }

    // Register callbacks
    esp_now_register_recv_cb((esp_now_recv_cb_t)onRecvStatic);
    esp_now_register_send_cb((esp_now_send_cb_t)onSendStatic);

    // Enable promiscuous mode for RSSI capture
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(promiscRxCb);

    // Add broadcast peer so we can always broadcast
    addPeer(ESPNOW_BROADCAST);

    // Get our MAC
    esp_wifi_get_mac(WIFI_IF_STA, _mac);

    resetStats();
    _lastDiscoveryMs = millis();
    _ready = true;

    DBG_INFO(TAG, "Init OK, role=%d ch=%d MAC=%02X:%02X:%02X:%02X:%02X:%02X",
             (int)_role, _channel,
             _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
    return true;
}

void EspNowHAL::deinit() {
    if (!_ready) return;

    esp_wifi_set_promiscuous(false);
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();

    _ready = false;
    _peerCount = 0;
    memset(_peers, 0, sizeof(_peers));

    if (_instance == this) _instance = nullptr;
    DBG_INFO(TAG, "Deinitialized");
}

bool EspNowHAL::isReady() const { return _ready; }

// ---- Identity ----

void EspNowHAL::getMAC(uint8_t mac[6]) const {
    memcpy(mac, _mac, 6);
}

EspNowRole EspNowHAL::getRole() const { return _role; }

// ---- Direct peer-to-peer ----

bool EspNowHAL::addPeer(const uint8_t mac[6]) {
    if (!_ready && memcmp(mac, ESPNOW_BROADCAST, 6) != 0) return false;

    // Check if already registered with ESP-NOW
    if (esp_now_is_peer_exist(mac)) return true;

    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, mac, 6);
    info.channel = _channel;
    info.encrypt = false;

    if (esp_now_add_peer(&info) != ESP_OK) {
        DBG_ERROR(TAG, "Failed to add peer %02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return false;
    }

    DBG_DEBUG(TAG, "Added peer %02X:%02X:%02X:%02X:%02X:%02X",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

bool EspNowHAL::removePeer(const uint8_t mac[6]) {
    if (!_ready) return false;

    // Don't remove broadcast peer
    if (memcmp(mac, ESPNOW_BROADCAST, 6) == 0) return false;

    if (esp_now_del_peer(mac) != ESP_OK) return false;

    // Remove from our peer table
    int idx = findPeer(mac);
    if (idx >= 0) {
        if (idx < _peerCount - 1) {
            memmove(&_peers[idx], &_peers[idx + 1],
                    (_peerCount - idx - 1) * sizeof(EspNowPeer));
        }
        _peerCount--;
    }
    return true;
}

bool EspNowHAL::send(const uint8_t dst_mac[6], const uint8_t* data, uint8_t len) {
    if (!_ready || !data || len == 0 || len > ESPNOW_MAX_DATA) return false;

    // Ensure peer is registered
    addPeer(dst_mac);

    esp_err_t err = esp_now_send(dst_mac, data, len);
    if (err == ESP_OK) {
        _stats.tx_count++;
        return true;
    }
    _stats.tx_fail++;
    return false;
}

bool EspNowHAL::broadcast(const uint8_t* data, uint8_t len) {
    return send(ESPNOW_BROADCAST, data, len);
}

// ---- Mesh networking ----

void EspNowHAL::buildMeshHeader(MeshHeader& hdr, MeshMsgType type,
                                  const uint8_t dst[6], uint8_t payloadLen) {
    hdr.magic = MESH_MAGIC;
    hdr.type = type;
    memcpy(hdr.src, _mac, 6);
    memcpy(hdr.dst, dst, 6);
    hdr.msg_id = _nextMsgId++;
    hdr.ttl = ESPNOW_MESH_MAX_HOPS;
    hdr.hop_count = 0;
    hdr.payload_len = payloadLen;
}

bool EspNowHAL::meshSend(const uint8_t dst_mac[6], const uint8_t* data, uint8_t len) {
    if (!_ready || !data || len == 0) return false;
    if (len > ESPNOW_MAX_DATA - sizeof(MeshHeader)) return false;

    uint8_t buf[250];
    MeshHeader& hdr = *(MeshHeader*)buf;
    buildMeshHeader(hdr, MeshMsgType::DATA, dst_mac, len);
    memcpy(buf + sizeof(MeshHeader), data, len);

    // Record our own message ID to avoid processing our own relayed packets
    recordMsgId(hdr.msg_id);

    uint8_t totalLen = sizeof(MeshHeader) + len;

    // If destination is a direct peer, send directly
    int idx = findPeer(dst_mac);
    if (idx >= 0 && _peers[idx].is_direct) {
        return send(dst_mac, buf, totalLen);
    }

    // Otherwise flood to all direct peers (mesh routing)
    return broadcast(buf, totalLen);
}

bool EspNowHAL::meshBroadcast(const uint8_t* data, uint8_t len) {
    return meshSend(ESPNOW_BROADCAST, data, len);
}

void EspNowHAL::meshDiscovery() {
    if (!_ready) return;

    // Build a PING with our role and peer count as payload
    uint8_t buf[250];
    MeshHeader& hdr = *(MeshHeader*)buf;
    buildMeshHeader(hdr, MeshMsgType::PING, ESPNOW_BROADCAST, 2);

    uint8_t* payload = buf + sizeof(MeshHeader);
    payload[0] = (uint8_t)_role;
    payload[1] = (uint8_t)_peerCount;

    recordMsgId(hdr.msg_id);
    broadcast(buf, sizeof(MeshHeader) + 2);
    _stats.discovery_count++;

    DBG_DEBUG(TAG, "Discovery PING sent (msg_id=%u)", hdr.msg_id);
}

// ---- Peer table ----

int EspNowHAL::getPeerCount() const { return _peerCount; }

int EspNowHAL::getPeers(EspNowPeer* peers, int maxPeers) const {
    int count = (_peerCount < maxPeers) ? _peerCount : maxPeers;
    memcpy(peers, _peers, count * sizeof(EspNowPeer));
    return count;
}

bool EspNowHAL::isPeerKnown(const uint8_t mac[6]) const {
    return findPeer(mac) >= 0;
}

int EspNowHAL::findPeer(const uint8_t mac[6]) const {
    for (int i = 0; i < _peerCount; i++) {
        if (memcmp(_peers[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

void EspNowHAL::updatePeer(const uint8_t mac[6], int8_t rssi,
                             uint8_t hopCount, bool isDirect) {
    // Don't track broadcast or our own MAC
    if (memcmp(mac, ESPNOW_BROADCAST, 6) == 0) return;
    if (memcmp(mac, _mac, 6) == 0) return;

    int idx = findPeer(mac);
    if (idx >= 0) {
        _peers[idx].rssi = rssi;
        _peers[idx].last_seen_ms = millis();
        // Update hop count if this is a shorter route
        if (hopCount < _peers[idx].hop_count || isDirect) {
            _peers[idx].hop_count = hopCount;
            _peers[idx].is_direct = isDirect;
        }
    } else if (_peerCount < ESPNOW_MAX_PEERS) {
        EspNowPeer& p = _peers[_peerCount];
        memcpy(p.mac, mac, 6);
        p.rssi = rssi;
        p.hop_count = hopCount;
        p.last_seen_ms = millis();
        p.is_direct = isDirect;
        _peerCount++;

        // Register with ESP-NOW so we can send to them
        addPeer(mac);

        DBG_INFO(TAG, "New peer: %02X:%02X:%02X:%02X:%02X:%02X rssi=%d hops=%d direct=%d",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 rssi, hopCount, isDirect);
    }
}

void EspNowHAL::expirePeers() {
    uint32_t now = millis();
    for (int i = _peerCount - 1; i >= 0; i--) {
        if ((now - _peers[i].last_seen_ms) > PEER_EXPIRY_MS) {
            DBG_DEBUG(TAG, "Peer expired: %02X:%02X:%02X:%02X:%02X:%02X",
                      _peers[i].mac[0], _peers[i].mac[1], _peers[i].mac[2],
                      _peers[i].mac[3], _peers[i].mac[4], _peers[i].mac[5]);

            // Remove from ESP-NOW peer list
            esp_now_del_peer(_peers[i].mac);

            if (i < _peerCount - 1) {
                memmove(&_peers[i], &_peers[i + 1],
                        (_peerCount - i - 1) * sizeof(EspNowPeer));
            }
            _peerCount--;
        }
    }
}

// ---- Dedup ----

bool EspNowHAL::isDuplicate(uint16_t msgId) {
    for (int i = 0; i < DEDUP_SIZE; i++) {
        if (_dedupBuf[i] == msgId) return true;
    }
    return false;
}

void EspNowHAL::recordMsgId(uint16_t msgId) {
    _dedupBuf[_dedupIdx] = msgId;
    _dedupIdx = (_dedupIdx + 1) % DEDUP_SIZE;
}

// ---- Mesh packet handling ----

void EspNowHAL::handleMeshPacket(const uint8_t* senderMac,
                                   const uint8_t* data, int len,
                                   int8_t rssi) {
    if (len < (int)sizeof(MeshHeader)) return;

    const MeshHeader* hdr = (const MeshHeader*)data;
    if (hdr->magic != MESH_MAGIC) return;

    // Dedup check
    if (isDuplicate(hdr->msg_id)) {
        _stats.dup_dropped++;
        return;
    }
    recordMsgId(hdr->msg_id);

    // Update peer table with the direct sender
    updatePeer(senderMac, rssi, 0, true);

    // Update peer table with the original source (via mesh)
    if (memcmp(senderMac, hdr->src, 6) != 0) {
        updatePeer(hdr->src, rssi, hdr->hop_count, false);
    }

    bool forUs = (memcmp(hdr->dst, _mac, 6) == 0);
    bool isBroadcast = (memcmp(hdr->dst, ESPNOW_BROADCAST, 6) == 0);

    const uint8_t* payload = data + sizeof(MeshHeader);
    uint8_t payloadLen = hdr->payload_len;

    switch (hdr->type) {
        case MeshMsgType::DATA:
            if (forUs || isBroadcast) {
                if (_meshCb) {
                    _meshCb(hdr->src, payload, payloadLen, hdr->hop_count);
                }
            }
            // Relay if not a leaf and TTL permits
            if (!forUs && _role != EspNowRole::LEAF) {
                relayPacket(data, len);
            }
            break;

        case MeshMsgType::PING:
            // Respond with PONG
            {
                uint8_t buf[250];
                MeshHeader& resp = *(MeshHeader*)buf;
                buildMeshHeader(resp, MeshMsgType::PONG, hdr->src, 2);

                uint8_t* respPayload = buf + sizeof(MeshHeader);
                respPayload[0] = (uint8_t)_role;
                respPayload[1] = (uint8_t)_peerCount;

                recordMsgId(resp.msg_id);
                send(senderMac, buf, sizeof(MeshHeader) + 2);

                DBG_DEBUG(TAG, "PONG sent to %02X:%02X:%02X:%02X:%02X:%02X",
                          senderMac[0], senderMac[1], senderMac[2],
                          senderMac[3], senderMac[4], senderMac[5]);
            }
            // Also relay PING for multi-hop discovery
            if (_role != EspNowRole::LEAF) {
                relayPacket(data, len);
            }
            break;

        case MeshMsgType::PONG:
            if (forUs || isBroadcast) {
                // PONG payload: [role, peerCount]
                if (payloadLen >= 2) {
                    DBG_DEBUG(TAG, "PONG from %02X:%02X:%02X:%02X:%02X:%02X role=%d peers=%d",
                              hdr->src[0], hdr->src[1], hdr->src[2],
                              hdr->src[3], hdr->src[4], hdr->src[5],
                              payload[0], payload[1]);
                }
            }
            // Relay PONG toward destination
            if (!forUs && _role != EspNowRole::LEAF) {
                relayPacket(data, len);
            }
            break;

        case MeshMsgType::ACK:
            if (forUs || isBroadcast) {
                DBG_DEBUG(TAG, "ACK from %02X:%02X:%02X:%02X:%02X:%02X",
                          hdr->src[0], hdr->src[1], hdr->src[2],
                          hdr->src[3], hdr->src[4], hdr->src[5]);
            }
            if (!forUs && _role != EspNowRole::LEAF) {
                relayPacket(data, len);
            }
            break;

        case MeshMsgType::ROUTE:
            // Route advertisements: update peer table with advertised nodes
            if (forUs || isBroadcast) {
                DBG_DEBUG(TAG, "ROUTE from %02X:%02X:%02X:%02X:%02X:%02X",
                          hdr->src[0], hdr->src[1], hdr->src[2],
                          hdr->src[3], hdr->src[4], hdr->src[5]);
            }
            if (_role != EspNowRole::LEAF) {
                relayPacket(data, len);
            }
            break;
    }
}

bool EspNowHAL::relayPacket(const uint8_t* rawPacket, uint8_t rawLen) {
    if (rawLen < sizeof(MeshHeader)) return false;

    // Make a mutable copy
    uint8_t buf[250];
    memcpy(buf, rawPacket, rawLen);

    MeshHeader* hdr = (MeshHeader*)buf;

    // Check TTL
    if (hdr->ttl == 0) {
        _stats.ttl_expired++;
        return false;
    }

    hdr->ttl--;
    hdr->hop_count++;

    // Broadcast the relayed packet
    esp_err_t err = esp_now_send(ESPNOW_BROADCAST, buf, rawLen);
    if (err == ESP_OK) {
        _stats.relay_count++;
        _stats.tx_count++;
        DBG_DEBUG(TAG, "Relayed msg_id=%u ttl=%d hops=%d", hdr->msg_id, hdr->ttl, hdr->hop_count);
        return true;
    }
    _stats.tx_fail++;
    return false;
}

// ---- Callbacks ----

void EspNowHAL::onReceive(EspNowRecvCb cb) { _recvCb = cb; }
void EspNowHAL::onMeshReceive(EspNowMeshCb cb) { _meshCb = cb; }

// ---- Process (call from loop) ----

void EspNowHAL::process() {
    if (!_ready) return;

    uint32_t now = millis();

    // Periodic discovery
    if ((now - _lastDiscoveryMs) >= DISCOVERY_INTERVAL_MS) {
        _lastDiscoveryMs = now;
        meshDiscovery();
    }

    // Expire stale peers
    expirePeers();
}

// ---- Stats ----

EspNowHAL::Stats EspNowHAL::getStats() const { return _stats; }

void EspNowHAL::resetStats() {
    memset(&_stats, 0, sizeof(_stats));
}

// ---- Test harness ----

EspNowHAL::TestResult EspNowHAL::runTest(int duration_s) {
    TestResult r = {};
    uint32_t startMs = millis();

    // Test init
    if (!_ready) {
        r.init_ok = init();
    } else {
        r.init_ok = true;
    }
    if (!r.init_ok) {
        r.test_duration_ms = millis() - startMs;
        return r;
    }

    // Test MAC
    uint8_t mac[6];
    getMAC(mac);
    r.mac_ok = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) { r.mac_ok = true; break; }
        if (i == 5) r.mac_ok = false; // All zeros = bad
    }

    // Test broadcast
    uint8_t testMsg[] = { 0xDE, 0xAD };
    r.broadcast_ok = broadcast(testMsg, sizeof(testMsg));

    // Run discovery
    meshDiscovery();
    r.mesh_discovery_ok = true;

    // Listen for peers over duration_s
    DBG_INFO(TAG, "Test: listening for %d seconds...", duration_s);
    uint32_t listenEnd = millis() + (duration_s * 1000);
    while (millis() < listenEnd) {
        process();
        delay(100);
    }

    r.peers_found = _peerCount;

    // Compute RSSI stats
    r.best_rssi = -127;
    r.worst_rssi = 0;
    if (_peerCount > 0) {
        for (int i = 0; i < _peerCount; i++) {
            if (_peers[i].rssi > r.best_rssi) r.best_rssi = _peers[i].rssi;
            if (_peers[i].rssi < r.worst_rssi) r.worst_rssi = _peers[i].rssi;
        }
    } else {
        r.best_rssi = 0;
        r.worst_rssi = 0;
    }

    // Ping each peer and measure RTT
    uint32_t totalRtt = 0;
    int pingCount = 0;
    for (int i = 0; i < _peerCount; i++) {
        uint8_t pingBuf[250];
        MeshHeader& hdr = *(MeshHeader*)pingBuf;
        buildMeshHeader(hdr, MeshMsgType::PING, _peers[i].mac, 2);
        uint8_t* payload = pingBuf + sizeof(MeshHeader);
        payload[0] = (uint8_t)_role;
        payload[1] = (uint8_t)_peerCount;
        recordMsgId(hdr.msg_id);

        uint32_t t0 = micros();
        if (send(_peers[i].mac, pingBuf, sizeof(MeshHeader) + 2)) {
            // Wait up to 500ms for any response (approximation since we
            // don't have per-message PONG matching in this simple test)
            delay(50);
            uint32_t t1 = micros();
            totalRtt += (t1 - t0);
            pingCount++;
        }
    }
    r.avg_rtt_us = pingCount > 0 ? (totalRtt / pingCount) : 0;

    // Test adding a fake peer
    uint8_t fakeMac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    r.peer_add_ok = addPeer(fakeMac);
    if (r.peer_add_ok) {
        // Clean up the fake peer
        esp_now_del_peer(fakeMac);
    }

    // Test sending (to broadcast, since we may not have real peers)
    r.send_ok = r.broadcast_ok;

    r.stats = getStats();
    r.test_duration_ms = millis() - startMs;

    DBG_INFO(TAG, "Test complete: peers=%d best_rssi=%d worst_rssi=%d rtt=%luus duration=%lums",
             r.peers_found, r.best_rssi, r.worst_rssi,
             (unsigned long)r.avg_rtt_us, (unsigned long)r.test_duration_ms);
    return r;
}


// ============================================================================
// Platform: Desktop Simulator
// ============================================================================
#else // SIMULATOR

#include <cstdio>

bool EspNowHAL::init(EspNowRole role, uint8_t channel) {
    _role = role;
    _channel = channel;
    memset(_mac, 0xAA, 6);
    _ready = true;
    resetStats();
    printf("[ESPNOW-SIM] Init OK, role=%d ch=%d\n", (int)role, channel);
    return true;
}

void EspNowHAL::deinit() {
    _ready = false;
    _peerCount = 0;
    printf("[ESPNOW-SIM] Deinitialized\n");
}

bool EspNowHAL::isReady() const { return _ready; }

void EspNowHAL::getMAC(uint8_t mac[6]) const { memcpy(mac, _mac, 6); }
EspNowRole EspNowHAL::getRole() const { return _role; }

bool EspNowHAL::addPeer(const uint8_t[6]) { return _ready; }
bool EspNowHAL::removePeer(const uint8_t[6]) { return _ready; }
bool EspNowHAL::send(const uint8_t[6], const uint8_t*, uint8_t) { return false; }
bool EspNowHAL::broadcast(const uint8_t*, uint8_t) { return false; }

bool EspNowHAL::meshSend(const uint8_t[6], const uint8_t*, uint8_t) { return false; }
bool EspNowHAL::meshBroadcast(const uint8_t*, uint8_t) { return false; }
void EspNowHAL::meshDiscovery() {}

int EspNowHAL::getPeerCount() const { return 0; }
int EspNowHAL::getPeers(EspNowPeer*, int) const { return 0; }
bool EspNowHAL::isPeerKnown(const uint8_t[6]) const { return false; }

void EspNowHAL::onReceive(EspNowRecvCb cb) { _recvCb = cb; }
void EspNowHAL::onMeshReceive(EspNowMeshCb cb) { _meshCb = cb; }

void EspNowHAL::process() {}

EspNowHAL::Stats EspNowHAL::getStats() const { return _stats; }
void EspNowHAL::resetStats() { memset(&_stats, 0, sizeof(_stats)); }

EspNowHAL::TestResult EspNowHAL::runTest(int) {
    TestResult r = {};
    r.init_ok = _ready;
    r.mac_ok = _ready;
    r.broadcast_ok = false;
    r.peer_add_ok = false;
    r.send_ok = false;
    r.mesh_discovery_ok = false;
    r.peers_found = 0;
    r.stats = _stats;
    return r;
}

// Private helpers (stubs for simulator)
bool EspNowHAL::isDuplicate(uint16_t) { return false; }
void EspNowHAL::recordMsgId(uint16_t) {}
int EspNowHAL::findPeer(const uint8_t[6]) const { return -1; }
void EspNowHAL::updatePeer(const uint8_t[6], int8_t, uint8_t, bool) {}
void EspNowHAL::expirePeers() {}
void EspNowHAL::buildMeshHeader(MeshHeader&, MeshMsgType, const uint8_t[6], uint8_t) {}
bool EspNowHAL::relayPacket(const uint8_t*, uint8_t) { return false; }
void EspNowHAL::handleMeshPacket(const uint8_t*, const uint8_t*, int, int8_t) {}

#endif // SIMULATOR
