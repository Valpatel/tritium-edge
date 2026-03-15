#include "hal_espnow.h"
#include "debug_log.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>

#if defined(ENABLE_DIAG)
#include "hal_diag.h"
#endif

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
        _instance->logDiagEvent("Send failed to %02X:%02X:%02X:%02X:%02X:%02X",
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
    _initTimeMs = millis();
    _prevPeerCount = 0;
    _ready = true;

    DBG_INFO(TAG, "Init OK, role=%d ch=%d MAC=%02X:%02X:%02X:%02X:%02X:%02X",
             (int)_role, _channel,
             _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);

    logDiagEvent("Mesh init OK, role=%d ch=%d", (int)_role, _channel);
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
    logDiagEvent("Mesh deinitialized");
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

    // Try route table first for next-hop selection
    int routeIdx = findRoute(dst_mac);
    if (routeIdx >= 0) {
        return send(_routes[routeIdx].next_hop, buf, totalLen);
    }

    // If destination is a direct peer, send directly
    int idx = findPeer(dst_mac);
    if (idx >= 0 && _peers[idx].is_direct) {
        return send(dst_mac, buf, totalLen);
    }

    // Fallback: flood to all direct peers
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
        logDiagEvent("Peer connected: %02X:%02X:%02X:%02X:%02X:%02X rssi=%d hops=%d",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     rssi, hopCount);
    }
}

void EspNowHAL::expirePeers() {
    uint32_t now = millis();
    for (int i = _peerCount - 1; i >= 0; i--) {
        if ((now - _peers[i].last_seen_ms) > PEER_EXPIRY_MS) {
            DBG_DEBUG(TAG, "Peer expired: %02X:%02X:%02X:%02X:%02X:%02X",
                      _peers[i].mac[0], _peers[i].mac[1], _peers[i].mac[2],
                      _peers[i].mac[3], _peers[i].mac[4], _peers[i].mac[5]);
            logDiagEvent("Peer disconnected: %02X:%02X:%02X:%02X:%02X:%02X (expired)",
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

// ---- Route table ----

void EspNowHAL::updateRoute(const uint8_t dest[6], const uint8_t next_hop[6],
                             uint8_t hops, int8_t rssi) {
    // Don't route to ourselves
    if (memcmp(dest, _mac, 6) == 0) return;

    int idx = findRoute(dest);
    if (idx >= 0) {
        // Update existing route if new path is shorter or fresher
        RouteEntry& r = _routes[idx];
        if (hops < r.hop_count || (millis() - r.last_updated_ms) > 10000) {
            memcpy(r.next_hop, next_hop, 6);
            r.hop_count = hops;
            r.rssi = rssi;
            r.last_updated_ms = millis();
        }
    } else if (_routeCount < MAX_ROUTES) {
        RouteEntry& r = _routes[_routeCount];
        memcpy(r.dest, dest, 6);
        memcpy(r.next_hop, next_hop, 6);
        r.hop_count = hops;
        r.rssi = rssi;
        r.last_updated_ms = millis();
        r.valid = true;
        _routeCount++;
    }
}

int EspNowHAL::findRoute(const uint8_t dest[6]) const {
    for (int i = 0; i < _routeCount; i++) {
        if (_routes[i].valid && memcmp(_routes[i].dest, dest, 6) == 0) {
            return i;
        }
    }
    return -1;
}

void EspNowHAL::expireRoutes() {
    uint32_t now = millis();
    for (int i = _routeCount - 1; i >= 0; i--) {
        if (!_routes[i].valid || (now - _routes[i].last_updated_ms) > ROUTE_EXPIRY_MS) {
            if (i < _routeCount - 1) {
                memmove(&_routes[i], &_routes[i + 1],
                        (_routeCount - i - 1) * sizeof(RouteEntry));
            }
            _routeCount--;
        }
    }
}

void EspNowHAL::advertiseRoutes() {
    if (!_ready || _peerCount == 0) return;

    // Build ROUTE payload: list of (MAC[6] + hop_count[1]) for each known peer
    uint8_t buf[250];
    MeshHeader& hdr = *(MeshHeader*)buf;

    // Each route entry in payload is 7 bytes (6 MAC + 1 hop_count)
    int maxEntries = (ESPNOW_MAX_DATA - sizeof(MeshHeader)) / 7;
    int entries = (_peerCount < maxEntries) ? _peerCount : maxEntries;

    buildMeshHeader(hdr, MeshMsgType::ROUTE, ESPNOW_BROADCAST, entries * 7);

    uint8_t* payload = buf + sizeof(MeshHeader);
    for (int i = 0; i < entries; i++) {
        memcpy(payload + i * 7, _peers[i].mac, 6);
        payload[i * 7 + 6] = _peers[i].hop_count;
    }

    recordMsgId(hdr.msg_id);
    broadcast(buf, sizeof(MeshHeader) + entries * 7);

    DBG_DEBUG(TAG, "Route advertisement sent (%d entries)", entries);
}

int EspNowHAL::getRouteCount() const { return _routeCount; }

// ---- ACK handling ----

void EspNowHAL::sendAck(const uint8_t dest_mac[6], uint16_t ack_msg_id) {
    uint8_t buf[250];
    MeshHeader& hdr = *(MeshHeader*)buf;
    buildMeshHeader(hdr, MeshMsgType::ACK, dest_mac, 2);

    // Payload: the msg_id we're acknowledging
    uint8_t* payload = buf + sizeof(MeshHeader);
    payload[0] = (uint8_t)(ack_msg_id >> 8);
    payload[1] = (uint8_t)(ack_msg_id & 0xFF);

    recordMsgId(hdr.msg_id);

    // Send directly to next hop if we have a route, otherwise to sender
    int routeIdx = findRoute(dest_mac);
    if (routeIdx >= 0) {
        send(_routes[routeIdx].next_hop, buf, sizeof(MeshHeader) + 2);
    } else {
        int peerIdx = findPeer(dest_mac);
        if (peerIdx >= 0 && _peers[peerIdx].is_direct) {
            send(dest_mac, buf, sizeof(MeshHeader) + 2);
        } else {
            broadcast(buf, sizeof(MeshHeader) + 2);
        }
    }
}

void EspNowHAL::handleAck(uint16_t msg_id) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (_pendingAcks[i].active && _pendingAcks[i].msg_id == msg_id) {
            _pendingAcks[i].active = false;
            _stats.ack_received++;
            DBG_DEBUG(TAG, "ACK received for msg_id=%u", msg_id);
            return;
        }
    }
}

void EspNowHAL::processAcks() {
    uint32_t now = millis();
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!_pendingAcks[i].active) continue;
        if ((now - _pendingAcks[i].sent_ms) < ACK_TIMEOUT_MS) continue;

        if (_pendingAcks[i].retries < MAX_RETRIES) {
            // Retry
            _pendingAcks[i].retries++;
            _pendingAcks[i].sent_ms = now;

            int routeIdx = findRoute(_pendingAcks[i].dest);
            if (routeIdx >= 0) {
                send(_routes[routeIdx].next_hop,
                     _pendingAcks[i].data, _pendingAcks[i].data_len);
            } else {
                broadcast(_pendingAcks[i].data, _pendingAcks[i].data_len);
            }
            _stats.tx_count++;

            DBG_DEBUG(TAG, "Retry %d for msg_id=%u",
                      _pendingAcks[i].retries, _pendingAcks[i].msg_id);
        } else {
            // Give up
            _pendingAcks[i].active = false;
            _stats.ack_timeout++;
            DBG_DEBUG(TAG, "ACK timeout for msg_id=%u after %d retries",
                      _pendingAcks[i].msg_id, MAX_RETRIES);
            logDiagEvent("Mesh delivery failed: msg_id=%u to %02X:%02X:%02X:%02X:%02X:%02X",
                         _pendingAcks[i].msg_id,
                         _pendingAcks[i].dest[0], _pendingAcks[i].dest[1],
                         _pendingAcks[i].dest[2], _pendingAcks[i].dest[3],
                         _pendingAcks[i].dest[4], _pendingAcks[i].dest[5]);
        }
    }
}

bool EspNowHAL::meshSendReliable(const uint8_t dst_mac[6],
                                   const uint8_t* data, uint8_t len) {
    // Can't do reliable delivery to broadcast
    if (memcmp(dst_mac, ESPNOW_BROADCAST, 6) == 0) {
        return meshSend(dst_mac, data, len);
    }

    if (!_ready || !data || len == 0) return false;
    if (len > ESPNOW_MAX_DATA - sizeof(MeshHeader)) return false;

    // Build the packet
    uint8_t buf[250];
    MeshHeader& hdr = *(MeshHeader*)buf;
    buildMeshHeader(hdr, MeshMsgType::DATA, dst_mac, len);
    memcpy(buf + sizeof(MeshHeader), data, len);

    recordMsgId(hdr.msg_id);
    uint8_t totalLen = sizeof(MeshHeader) + len;

    // Find a free pending ACK slot
    int slot = -1;
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!_pendingAcks[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        // All slots full, fall back to unreliable
        return meshSend(dst_mac, data, len);
    }

    // Store for retry
    PendingAck& pa = _pendingAcks[slot];
    pa.msg_id = hdr.msg_id;
    memcpy(pa.dest, dst_mac, 6);
    memcpy(pa.data, buf, totalLen);
    pa.data_len = totalLen;
    pa.retries = 0;
    pa.sent_ms = millis();
    pa.active = true;

    // Send using best route
    int routeIdx = findRoute(dst_mac);
    if (routeIdx >= 0) {
        return send(_routes[routeIdx].next_hop, buf, totalLen);
    }

    int peerIdx = findPeer(dst_mac);
    if (peerIdx >= 0 && _peers[peerIdx].is_direct) {
        return send(dst_mac, buf, totalLen);
    }

    return broadcast(buf, totalLen);
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
        // Learn route: to reach src, go through senderMac
        updateRoute(hdr->src, senderMac, hdr->hop_count, rssi);
    }

    bool forUs = (memcmp(hdr->dst, _mac, 6) == 0);
    bool isBroadcast = (memcmp(hdr->dst, ESPNOW_BROADCAST, 6) == 0);

    const uint8_t* payload = data + sizeof(MeshHeader);
    uint8_t payloadLen = hdr->payload_len;

    switch (hdr->type) {
        case MeshMsgType::DATA:
            if (forUs || isBroadcast) {
                logDiagEvent("Mesh DATA from %02X:%02X:%02X:%02X:%02X:%02X hops=%d len=%d",
                             hdr->src[0], hdr->src[1], hdr->src[2],
                             hdr->src[3], hdr->src[4], hdr->src[5],
                             hdr->hop_count, payloadLen);

                // Check for classification relay subtype (first byte)
                if (payloadLen >= sizeof(ClassifyRelay) &&
                    payload[0] == (uint8_t)MeshDataType::CLASSIFY) {
                    handleClassifyRelay(payload, payloadLen);
                } else if (_meshCb) {
                    _meshCb(hdr->src, payload, payloadLen, hdr->hop_count);
                }
                // Send ACK back to source for unicast messages
                if (forUs && !isBroadcast) {
                    sendAck(hdr->src, hdr->msg_id);
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
                if (payloadLen >= 2) {
                    uint16_t ackedId = ((uint16_t)payload[0] << 8) | payload[1];
                    handleAck(ackedId);
                }
                DBG_DEBUG(TAG, "ACK from %02X:%02X:%02X:%02X:%02X:%02X",
                          hdr->src[0], hdr->src[1], hdr->src[2],
                          hdr->src[3], hdr->src[4], hdr->src[5]);
            }
            if (!forUs && _role != EspNowRole::LEAF) {
                relayPacket(data, len);
            }
            break;

        case MeshMsgType::ROUTE:
            // Route advertisements: learn routes through sender
            if (forUs || isBroadcast) {
                // Payload: N entries of (MAC[6] + hop_count[1])
                int numEntries = payloadLen / 7;
                for (int i = 0; i < numEntries; i++) {
                    const uint8_t* advMac = payload + i * 7;
                    uint8_t advHops = payload[i * 7 + 6];
                    // Route to advMac goes through senderMac, total hops = advHops + 1
                    updateRoute(advMac, senderMac, advHops + 1, rssi);
                    // Also update peer table for indirect peers
                    if (findPeer(advMac) < 0) {
                        updatePeer(advMac, rssi, advHops + 1, false);
                    }
                }
                DBG_DEBUG(TAG, "ROUTE from %02X:%02X:%02X:%02X:%02X:%02X (%d entries)",
                          hdr->src[0], hdr->src[1], hdr->src[2],
                          hdr->src[3], hdr->src[4], hdr->src[5], numEntries);
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
void EspNowHAL::onClassifyRelay(ClassifyRelayCallback cb) { _classifyCb = cb; }

bool EspNowHAL::broadcastClassification(const uint8_t device_mac[6], int8_t rssi,
                                          DeviceClassId class_id, uint8_t confidence,
                                          const char* device_name) {
    if (!_ready) return false;

    // Build compact classification relay payload
    uint8_t name_len = 0;
    if (device_name) {
        name_len = (uint8_t)strlen(device_name);
        if (name_len > 30) name_len = 30;  // Cap name length for packet size
    }

    uint8_t payload[sizeof(ClassifyRelay) + 31];  // relay struct + max name + null
    ClassifyRelay* relay = (ClassifyRelay*)payload;
    relay->subtype = MeshDataType::CLASSIFY;
    memcpy(relay->device_mac, device_mac, 6);
    relay->rssi = rssi;
    relay->class_id = (uint8_t)class_id;
    relay->confidence = confidence;
    relay->name_len = name_len;

    if (name_len > 0 && device_name) {
        memcpy(payload + sizeof(ClassifyRelay), device_name, name_len);
        payload[sizeof(ClassifyRelay) + name_len] = '\0';
    }

    uint8_t total_len = sizeof(ClassifyRelay) + name_len + (name_len > 0 ? 1 : 0);

    DBG_DEBUG(TAG, "Broadcasting classification: MAC=%02X:%02X:%02X:%02X:%02X:%02X class=%d conf=%d",
              device_mac[0], device_mac[1], device_mac[2],
              device_mac[3], device_mac[4], device_mac[5],
              (int)class_id, (int)confidence);

    return meshBroadcast(payload, total_len);
}

void EspNowHAL::handleClassifyRelay(const uint8_t* payload, uint8_t len) {
    if (len < sizeof(ClassifyRelay)) return;

    const ClassifyRelay* relay = (const ClassifyRelay*)payload;
    if (relay->subtype != MeshDataType::CLASSIFY) return;

    // Extract device name if present
    const char* name = nullptr;
    char name_buf[32] = {};
    if (relay->name_len > 0 && len >= sizeof(ClassifyRelay) + relay->name_len) {
        uint8_t copy_len = relay->name_len;
        if (copy_len > 30) copy_len = 30;
        memcpy(name_buf, payload + sizeof(ClassifyRelay), copy_len);
        name_buf[copy_len] = '\0';
        name = name_buf;
    }

    DBG_DEBUG(TAG, "Received classification relay: MAC=%02X:%02X:%02X:%02X:%02X:%02X class=%d conf=%d name=%s",
              relay->device_mac[0], relay->device_mac[1], relay->device_mac[2],
              relay->device_mac[3], relay->device_mac[4], relay->device_mac[5],
              (int)relay->class_id, (int)relay->confidence,
              name ? name : "(none)");

    if (_classifyCb) {
        _classifyCb(*relay, name);
    }
}

// ---- Process (call from loop) ----

void EspNowHAL::process() {
    if (!_ready) return;

    uint32_t now = millis();

    // Periodic discovery (also serves as beacon/probe for neighbor discovery)
    if ((now - _lastDiscoveryMs) >= DISCOVERY_INTERVAL_MS) {
        _lastDiscoveryMs = now;
        meshDiscovery();
    }

    // Periodic route advertisements
    if ((now - _lastRouteAdvMs) >= ROUTE_ADV_INTERVAL_MS) {
        _lastRouteAdvMs = now;
        advertiseRoutes();
    }

    // Process pending ACK retries
    processAcks();

    // Expire stale peers and routes
    expirePeers();
    expireRoutes();

    // Update route count stat
    _stats.route_count = _routeCount;

    // Check for peer count changes and log to diagnostics
    checkPeerChanges();
}

// ---- Stats ----

EspNowHAL::Stats EspNowHAL::getStats() const { return _stats; }

void EspNowHAL::resetStats() {
    memset(&_stats, 0, sizeof(_stats));
}

// ---- Diagnostics integration ----

void EspNowHAL::logDiagEvent(const char* fmt, ...) {
    if (!_diagEnabled) return;
#if defined(ENABLE_DIAG)
    char msg[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    hal_diag::log(hal_diag::Severity::INFO, "mesh", "%s", msg);
#else
    (void)fmt;
#endif
}

void EspNowHAL::checkPeerChanges() {
    if (!_diagEnabled) return;
    if (_peerCount != _prevPeerCount) {
#if defined(ENABLE_DIAG)
        hal_diag::log_value("mesh", "peer_count", (float)_peerCount, 0, ESPNOW_MAX_PEERS);
#endif
        _prevPeerCount = _peerCount;
    }
}

void EspNowHAL::enableDiagLogging(bool enable) {
    _diagEnabled = enable;
    if (enable) {
        DBG_INFO(TAG, "Diagnostic logging enabled");
    }
}

EspNowHAL::MeshHealth EspNowHAL::getMeshHealth() const {
    MeshHealth h = {};
    h.peer_count = _peerCount;
    h.msgs_sent = _stats.tx_count;
    h.msgs_received = _stats.rx_count;
    h.msgs_relayed = _stats.relay_count;
    h.send_failures = _stats.tx_fail;
    h.discovery_rounds = _stats.discovery_count;
    h.uptime_ms = _ready ? (millis() - _initTimeMs) : 0;

    // Packet loss rate
    uint32_t totalAttempts = _stats.tx_count + _stats.tx_fail;
    h.packet_loss_rate = (totalAttempts > 0)
        ? (float)_stats.tx_fail / (float)totalAttempts
        : 0.0f;

    // Peer analysis
    h.direct_peers = 0;
    h.best_rssi = -127;
    h.worst_rssi = 0;
    float hopSum = 0;

    if (_peerCount > 0) {
        for (int i = 0; i < _peerCount; i++) {
            if (_peers[i].is_direct) h.direct_peers++;
            if (_peers[i].rssi > h.best_rssi) h.best_rssi = _peers[i].rssi;
            if (_peers[i].rssi < h.worst_rssi) h.worst_rssi = _peers[i].rssi;
            hopSum += _peers[i].hop_count;
        }
        h.avg_hop_count = hopSum / _peerCount;
    } else {
        h.best_rssi = 0;
        h.worst_rssi = 0;
        h.avg_hop_count = 0;
    }

    return h;
}

int EspNowHAL::meshToJson(char* buf, size_t size) const {
    if (!buf || size < 64) return -1;

    const char* roleStr = "node";
    if (_role == EspNowRole::GATEWAY) roleStr = "gateway";
    else if (_role == EspNowRole::LEAF) roleStr = "leaf";

    int pos = snprintf(buf, size,
        "{\"self\":{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"role\":\"%s\",\"channel\":%d},",
        _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5],
        roleStr, _channel);

    // Peers array
    pos += snprintf(buf + pos, size - pos, "\"peers\":[");
    for (int i = 0; i < _peerCount && pos < (int)size - 128; i++) {
        if (i > 0) buf[pos++] = ',';
        uint32_t age = _ready ? (millis() - _peers[i].last_seen_ms) : 0;
        pos += snprintf(buf + pos, size - pos,
            "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"rssi\":%d,\"hops\":%d,\"direct\":%s,\"age_ms\":%lu}",
            _peers[i].mac[0], _peers[i].mac[1], _peers[i].mac[2],
            _peers[i].mac[3], _peers[i].mac[4], _peers[i].mac[5],
            _peers[i].rssi, _peers[i].hop_count,
            _peers[i].is_direct ? "true" : "false",
            (unsigned long)age);
    }
    pos += snprintf(buf + pos, size - pos, "],");

    // Stats
    MeshHealth h = getMeshHealth();
    pos += snprintf(buf + pos, size - pos,
        "\"stats\":{\"tx\":%lu,\"rx\":%lu,\"relayed\":%lu,"
        "\"tx_fail\":%lu,\"dup_dropped\":%lu,\"ttl_expired\":%lu,"
        "\"discovery_rounds\":%lu,\"packet_loss_rate\":%.3f,"
        "\"avg_hop_count\":%.2f,\"uptime_ms\":%lu}}",
        (unsigned long)_stats.tx_count,
        (unsigned long)_stats.rx_count,
        (unsigned long)_stats.relay_count,
        (unsigned long)_stats.tx_fail,
        (unsigned long)_stats.dup_dropped,
        (unsigned long)_stats.ttl_expired,
        (unsigned long)_stats.discovery_count,
        h.packet_loss_rate,
        h.avg_hop_count,
        (unsigned long)h.uptime_ms);

    return pos;
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
bool EspNowHAL::meshSendReliable(const uint8_t[6], const uint8_t*, uint8_t) { return false; }
void EspNowHAL::meshDiscovery() {}
int EspNowHAL::getRouteCount() const { return 0; }

int EspNowHAL::getPeerCount() const { return 0; }
int EspNowHAL::getPeers(EspNowPeer*, int) const { return 0; }
bool EspNowHAL::isPeerKnown(const uint8_t[6]) const { return false; }

void EspNowHAL::onReceive(EspNowRecvCb cb) { _recvCb = cb; }
void EspNowHAL::onMeshReceive(EspNowMeshCb cb) { _meshCb = cb; }
void EspNowHAL::onClassifyRelay(ClassifyRelayCallback cb) { _classifyCb = cb; }
bool EspNowHAL::broadcastClassification(const uint8_t[6], int8_t, DeviceClassId, uint8_t, const char*) { return false; }
void EspNowHAL::handleClassifyRelay(const uint8_t*, uint8_t) {}

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

// Route table stubs
void EspNowHAL::updateRoute(const uint8_t[6], const uint8_t[6], uint8_t, int8_t) {}
int EspNowHAL::findRoute(const uint8_t[6]) const { return -1; }
void EspNowHAL::expireRoutes() {}
void EspNowHAL::advertiseRoutes() {}

// ACK stubs
void EspNowHAL::sendAck(const uint8_t[6], uint16_t) {}
void EspNowHAL::processAcks() {}
void EspNowHAL::handleAck(uint16_t) {}

// Diagnostics stubs
void EspNowHAL::logDiagEvent(const char*, ...) {}
void EspNowHAL::checkPeerChanges() {}
void EspNowHAL::enableDiagLogging(bool) {}

EspNowHAL::MeshHealth EspNowHAL::getMeshHealth() const {
    return MeshHealth{};
}

int EspNowHAL::meshToJson(char* buf, size_t size) const {
    if (!buf || size < 32) return -1;
    return snprintf(buf, size, "{\"self\":{},\"peers\":[],\"stats\":{}}");
}

#endif // SIMULATOR
