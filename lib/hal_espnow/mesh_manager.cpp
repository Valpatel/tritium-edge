// Tritium-OS Mesh Manager implementation.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "mesh_manager.h"
#include "hal_espnow.h"
#include "debug_log.h"
#include <cstring>
#include <cstdio>

static constexpr const char* TAG = "mesh_mgr";

// ============================================================================
// Platform: ESP32
// ============================================================================
#ifndef SIMULATOR

#include "tritium_compat.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <esp_wifi.h>

// espnow_hal_ptr() defined inline in espnow_service.h
#include "espnow_service.h"

// ── Singleton ───────────────────────────────────────────────────────────────

MeshManager& MeshManager::instance() {
    static MeshManager inst;
    return inst;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

bool MeshManager::init(MeshRole role) {
    if (_ready) return true;

    _role = role;
    memset(&_stats, 0, sizeof(_stats));
    _stats.role = role;
    _peerCount = 0;
    _stateCount = 0;
    _linkCount = 0;
    _nextSeq = (uint16_t)(esp_random() & 0xFFFF);  // Random start for dedup

    // Get our MAC from the underlying HAL
    // We need the EspNowHAL to be initialized already by EspNowService
    // Grab MAC from WiFi directly
    esp_wifi_get_mac(WIFI_IF_STA, _mac);

    // Set default device name from MAC suffix
    if (_deviceName[0] == '\0') {
        snprintf(_deviceName, sizeof(_deviceName), "T-%02X%02X",
                 _mac[4], _mac[5]);
    }

    // Register our raw callback on the HAL — we intercept enhanced frames
    // The EspNowHAL already handles legacy 0xE5 frames; we listen for raw
    // frames that start with our 0x54 magic via the raw receive callback.
    // Note: we get ALL raw frames from the onReceive callback, then check
    // for our magic byte.

    _lastPingMs = millis();
    _lastTopoMs = millis();
    _lastStateSyncMs = millis();
    _ready = true;

    DBG_INFO(TAG, "Init OK, role=%d MAC=%02X:%02X:%02X:%02X:%02X:%02X",
             (int)_role, _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);

    // Immediate discovery
    broadcastDiscovery();

    return true;
}

bool MeshManager::isReady() const { return _ready; }

// ── Identity ────────────────────────────────────────────────────────────────

void MeshManager::getMAC(uint8_t mac[6]) const {
    memcpy(mac, _mac, 6);
}

MeshRole MeshManager::getRole() const { return _role; }

void MeshManager::setRole(MeshRole role) {
    _role = role;
    _stats.role = role;
}

void MeshManager::setDeviceName(const char* name) {
    strncpy(_deviceName, name, sizeof(_deviceName) - 1);
    _deviceName[sizeof(_deviceName) - 1] = '\0';
}

void MeshManager::setFirmwareVersion(const char* ver) {
    strncpy(_firmwareVersion, ver, sizeof(_firmwareVersion) - 1);
    _firmwareVersion[sizeof(_firmwareVersion) - 1] = '\0';
}

void MeshManager::setBoardType(const char* board) {
    strncpy(_boardType, board, sizeof(_boardType) - 1);
    _boardType[sizeof(_boardType) - 1] = '\0';
}

void MeshManager::setBatteryPercent(uint8_t pct) {
    _batteryPct = pct;
}

// ── Tick ────────────────────────────────────────────────────────────────────

void MeshManager::tick() {
    if (!_ready) return;

    uint32_t now = millis();

    // Periodic discovery ping
    if ((now - _lastPingMs) >= PING_INTERVAL_MS) {
        _lastPingMs = now;
        broadcastDiscovery();
    }

    // Periodic topology announcement
    if ((now - _lastTopoMs) >= TOPO_INTERVAL_MS) {
        _lastTopoMs = now;
        broadcastTopology();
    }

    // Expire stale peers
    expirePeers();

    // Update stats
    _stats.peer_count = _peerCount;
    _stats.is_elected_gateway = _electedGateway;
}

// ── Messaging ───────────────────────────────────────────────────────────────

bool MeshManager::broadcast(const uint8_t* data, size_t len) {
    if (!_ready || !data || len == 0) return false;
    if (len > ESPNOW_MAX_DATA - sizeof(MeshHeaderEx)) return false;

    uint8_t buf[250];
    MeshHeaderEx& hdr = *(MeshHeaderEx*)buf;
    buildHeader(hdr, MESH_EX_BROADCAST, ESPNOW_BROADCAST, (uint8_t)len);
    memcpy(buf + sizeof(MeshHeaderEx), data, len);

    recordDedup(_mac, hdr.seq);

    // Use the EspNowHAL singleton (accessed via the static _instance)
    EspNowHAL* hal = espnow_hal_ptr();
    if (!hal) return false;

    bool ok = hal->broadcast(buf, sizeof(MeshHeaderEx) + (uint8_t)len);
    if (ok) _stats.tx_count++;
    else    _stats.tx_fail++;
    return ok;
}

bool MeshManager::sendTo(const uint8_t* dst_mac, const uint8_t* data, size_t len) {
    if (!_ready || !data || !dst_mac || len == 0) return false;
    if (len > ESPNOW_MAX_DATA - sizeof(MeshHeaderEx)) return false;

    uint8_t buf[250];
    MeshHeaderEx& hdr = *(MeshHeaderEx*)buf;
    buildHeader(hdr, MESH_EX_ADDRESSED, dst_mac, (uint8_t)len);
    memcpy(buf + sizeof(MeshHeaderEx), data, len);

    recordDedup(_mac, hdr.seq);

    EspNowHAL* hal = espnow_hal_ptr();
    if (!hal) return false;

    // Check if direct peer — send directly, otherwise broadcast for relay
    bool ok;
    if (hal->isPeerKnown(dst_mac)) {
        ok = hal->send(dst_mac, buf, sizeof(MeshHeaderEx) + (uint8_t)len);
    } else {
        ok = hal->broadcast(buf, sizeof(MeshHeaderEx) + (uint8_t)len);
    }

    if (ok) _stats.tx_count++;
    else    _stats.tx_fail++;
    return ok;
}

void MeshManager::onMessage(MeshMessageCallback cb, void* user_data) {
    _msgCb = cb;
    _msgCbUserData = user_data;
}

// ── Peer management ─────────────────────────────────────────────────────────

int MeshManager::getPeers(MeshPeerInfo* out, int max_count) const {
    int count = (_peerCount < max_count) ? _peerCount : max_count;
    if (count > 0) memcpy(out, _peers, count * sizeof(MeshPeerInfo));
    return count;
}

int MeshManager::peerCount() const { return _peerCount; }

bool MeshManager::pingPeer(const uint8_t* mac) {
    if (!_ready || !mac) return false;

    uint8_t buf[250];
    MeshHeaderEx& hdr = *(MeshHeaderEx*)buf;
    MeshPongPayload payload = {};
    payload.role = (uint8_t)_role;
    payload.peer_count = (uint8_t)_peerCount;
    payload.battery_pct = _batteryPct;
    strncpy(payload.device_name, _deviceName, sizeof(payload.device_name) - 1);
    strncpy(payload.firmware_version, _firmwareVersion, sizeof(payload.firmware_version) - 1);
    strncpy(payload.board_type, _boardType, sizeof(payload.board_type) - 1);

    buildHeader(hdr, MESH_EX_PING, mac, sizeof(MeshPongPayload));
    memcpy(buf + sizeof(MeshHeaderEx), &payload, sizeof(MeshPongPayload));

    recordDedup(_mac, hdr.seq);

    EspNowHAL* hal = espnow_hal_ptr();
    if (!hal) return false;

    bool ok = hal->send(mac, buf, sizeof(MeshHeaderEx) + sizeof(MeshPongPayload));
    if (ok) _stats.tx_count++;
    else    _stats.tx_fail++;
    return ok;
}

const MeshPeerInfo* MeshManager::findPeer(const uint8_t* mac) const {
    int idx = findPeerIdx(mac);
    return (idx >= 0) ? &_peers[idx] : nullptr;
}

// ── Shared state ────────────────────────────────────────────────────────────

bool MeshManager::setState(const char* key, const char* value) {
    if (!_ready || !key || !value) return false;
    if (strlen(key) >= MESH_STATE_KEY_LEN) return false;
    if (strlen(value) >= MESH_STATE_VALUE_LEN) return false;

    int idx = findStateIdx(key);
    if (idx < 0) {
        // New entry
        if (_stateCount >= MESH_MAX_STATE) return false;
        idx = _stateCount++;
        strncpy(_state[idx].key, key, MESH_STATE_KEY_LEN - 1);
        _state[idx].key[MESH_STATE_KEY_LEN - 1] = '\0';
        _state[idx].version = 0;
    }

    strncpy(_state[idx].value, value, MESH_STATE_VALUE_LEN - 1);
    _state[idx].value[MESH_STATE_VALUE_LEN - 1] = '\0';
    memcpy(_state[idx].src_mac, _mac, 6);
    _state[idx].timestamp_ms = millis();
    _state[idx].version++;
    _stats.state_syncs++;

    // Broadcast the state update to peers
    uint8_t buf[250];
    MeshHeaderEx& hdr = *(MeshHeaderEx*)buf;
    MeshStateSyncPayload syncPayload = {};
    strncpy(syncPayload.key, key, MESH_STATE_KEY_LEN - 1);
    strncpy(syncPayload.value, value, MESH_STATE_VALUE_LEN - 1);
    syncPayload.version = _state[idx].version;

    buildHeader(hdr, MESH_EX_STATE_SYNC, ESPNOW_BROADCAST, sizeof(MeshStateSyncPayload));
    memcpy(buf + sizeof(MeshHeaderEx), &syncPayload, sizeof(MeshStateSyncPayload));

    recordDedup(_mac, hdr.seq);

    EspNowHAL* hal = espnow_hal_ptr();
    if (hal) {
        hal->broadcast(buf, sizeof(MeshHeaderEx) + sizeof(MeshStateSyncPayload));
    }

    return true;
}

const char* MeshManager::getState(const char* key) const {
    int idx = findStateIdx(key);
    return (idx >= 0) ? _state[idx].value : nullptr;
}

int MeshManager::getAllState(MeshStateEntry* out, int max_count) const {
    int count = (_stateCount < max_count) ? _stateCount : max_count;
    if (count > 0) memcpy(out, _state, count * sizeof(MeshStateEntry));
    return count;
}

int MeshManager::stateCount() const { return _stateCount; }

// ── Topology ────────────────────────────────────────────────────────────────

int MeshManager::getTopology(MeshLink* out, int max_links) const {
    int count = (_linkCount < max_links) ? _linkCount : max_links;
    if (count > 0) memcpy(out, _links, count * sizeof(MeshLink));
    return count;
}

int MeshManager::linkCount() const { return _linkCount; }

bool MeshManager::isGateway() const {
    return _role == MESH_ROLE_GATEWAY || _electedGateway;
}

// ── Gateway election ────────────────────────────────────────────────────────

void MeshManager::electGateway() {
    // Simple election: if we have WiFi and peers don't, we're gateway.
    // Check if WiFi is connected
    wifi_ap_record_t _ap_info;
    bool hasWifi = (esp_wifi_sta_get_ap_info(&_ap_info) == ESP_OK);

    if (hasWifi) {
        // Check if any peer is already gateway with better RSSI
        bool otherGateway = false;
        for (int i = 0; i < _peerCount; i++) {
            if (_peers[i].is_gateway) {
                otherGateway = true;
                break;
            }
        }
        if (!otherGateway) {
            _electedGateway = true;
            _role = MESH_ROLE_GATEWAY;
            _stats.role = MESH_ROLE_GATEWAY;
            DBG_INFO(TAG, "Elected as mesh gateway");
        }
    } else {
        _electedGateway = false;
        if (_role == MESH_ROLE_GATEWAY) {
            _role = MESH_ROLE_RELAY;
            _stats.role = MESH_ROLE_RELAY;
        }
    }
}

// ── Stats ───────────────────────────────────────────────────────────────────

const MeshManager::MeshStats& MeshManager::getStats() const { return _stats; }

// ── Deduplication ───────────────────────────────────────────────────────────

bool MeshManager::isDuplicate(const uint8_t* src_mac, uint16_t seq) {
    for (int i = 0; i < DEDUP_SIZE; i++) {
        if (_dedupBuf[i].seq == seq &&
            memcmp(_dedupBuf[i].src_mac, src_mac, 6) == 0) {
            return true;
        }
    }
    return false;
}

void MeshManager::recordDedup(const uint8_t* src_mac, uint16_t seq) {
    memcpy(_dedupBuf[_dedupIdx].src_mac, src_mac, 6);
    _dedupBuf[_dedupIdx].seq = seq;
    _dedupIdx = (_dedupIdx + 1) % DEDUP_SIZE;
}

// ── Header construction ─────────────────────────────────────────────────────

void MeshManager::buildHeader(MeshHeaderEx& hdr, MeshMsgTypeEx type,
                               const uint8_t* dst, uint8_t payloadLen) {
    hdr.magic = MESH_EX_MAGIC;
    hdr.version = MESH_EX_VERSION;
    hdr.type = type;
    hdr.ttl = ESPNOW_MESH_MAX_HOPS;
    memcpy(hdr.src_mac, _mac, 6);
    memcpy(hdr.dst_mac, dst, 6);
    hdr.seq = _nextSeq++;
    hdr.payload_len = payloadLen;
}

// ── Raw receive callback ────────────────────────────────────────────────────

void MeshManager::onRawReceive(const uint8_t* src_mac, const uint8_t* data,
                                uint8_t len, int8_t rssi) {
    // Check for our enhanced magic byte
    if (len < sizeof(MeshHeaderEx)) return;
    if (data[0] != MESH_EX_MAGIC) return;

    MeshManager::instance().handlePacket(src_mac, data, len, rssi);
}

// ── Packet handling ─────────────────────────────────────────────────────────

void MeshManager::handlePacket(const uint8_t* senderMac, const uint8_t* data,
                                int len, int8_t rssi) {
    if (len < (int)sizeof(MeshHeaderEx)) return;

    const MeshHeaderEx* hdr = (const MeshHeaderEx*)data;
    if (hdr->magic != MESH_EX_MAGIC) return;
    if (hdr->version != MESH_EX_VERSION) return;

    // Don't process our own packets
    if (memcmp(hdr->src_mac, _mac, 6) == 0) return;

    // Dedup check
    if (isDuplicate(hdr->src_mac, hdr->seq)) {
        _stats.dedup_drop++;
        return;
    }
    recordDedup(hdr->src_mac, hdr->seq);

    _stats.rx_count++;

    // Track the direct sender as a peer
    updatePeer(senderMac, rssi, 0);

    // Track the original source if different
    if (memcmp(senderMac, hdr->src_mac, 6) != 0) {
        updatePeer(hdr->src_mac, rssi, hdr->ttl > 0 ?
                   (ESPNOW_MESH_MAX_HOPS - hdr->ttl + 1) : ESPNOW_MESH_MAX_HOPS);
    }

    const uint8_t* payload = data + sizeof(MeshHeaderEx);
    uint8_t payloadLen = hdr->payload_len;

    bool forUs = (memcmp(hdr->dst_mac, _mac, 6) == 0);
    bool isBroadcast = (memcmp(hdr->dst_mac, ESPNOW_BROADCAST, 6) == 0);

    switch (hdr->type) {
        case MESH_EX_PING:
            handlePing(*hdr, senderMac, payload, payloadLen, rssi);
            // Relay pings for multi-hop discovery
            if (!forUs && _role != MESH_ROLE_LEAF && _role != MESH_ROLE_SENSOR) {
                relayPacket(data, (uint8_t)len);
            }
            break;

        case MESH_EX_PONG:
            if (forUs || isBroadcast) {
                handlePong(*hdr, payload, payloadLen, rssi);
            }
            // Relay pong toward destination
            if (!forUs && _role != MESH_ROLE_LEAF && _role != MESH_ROLE_SENSOR) {
                relayPacket(data, (uint8_t)len);
            }
            break;

        case MESH_EX_BROADCAST:
            if (_msgCb) {
                _msgCb(*hdr, payload, _msgCbUserData);
            }
            // Relay broadcasts
            if (_role != MESH_ROLE_LEAF && _role != MESH_ROLE_SENSOR) {
                relayPacket(data, (uint8_t)len);
            }
            break;

        case MESH_EX_ADDRESSED:
            if (forUs) {
                if (_msgCb) {
                    _msgCb(*hdr, payload, _msgCbUserData);
                }
            } else if (_role != MESH_ROLE_LEAF && _role != MESH_ROLE_SENSOR) {
                relayPacket(data, (uint8_t)len);
            }
            break;

        case MESH_EX_STATE_SYNC:
            handleStateSync(*hdr, payload, payloadLen);
            // Relay state syncs
            if (_role != MESH_ROLE_LEAF && _role != MESH_ROLE_SENSOR) {
                relayPacket(data, (uint8_t)len);
            }
            break;

        case MESH_EX_STATE_REQUEST:
            if (forUs || isBroadcast) {
                handleStateRequest(*hdr, senderMac);
            }
            break;

        case MESH_EX_STATE_DUMP:
            // State dump is multiple state sync payloads concatenated
            if (forUs || isBroadcast) {
                int offset = 0;
                while (offset + (int)sizeof(MeshStateSyncPayload) <= payloadLen) {
                    handleStateSync(*hdr, payload + offset,
                                    sizeof(MeshStateSyncPayload));
                    offset += sizeof(MeshStateSyncPayload);
                }
            }
            break;

        case MESH_EX_TOPOLOGY:
            handleTopology(*hdr, payload, payloadLen);
            // Relay topology announcements
            if (_role != MESH_ROLE_LEAF && _role != MESH_ROLE_SENSOR) {
                relayPacket(data, (uint8_t)len);
            }
            break;

        case MESH_EX_OTA_OFFER:
        case MESH_EX_OTA_REQUEST:
        case MESH_EX_OTA_CHUNK:
            // OTA messages — forward to callback, relay if needed
            if (forUs || isBroadcast) {
                if (_msgCb) _msgCb(*hdr, payload, _msgCbUserData);
            }
            if (!forUs && _role != MESH_ROLE_LEAF && _role != MESH_ROLE_SENSOR) {
                relayPacket(data, (uint8_t)len);
            }
            break;
    }
}

bool MeshManager::relayPacket(const uint8_t* rawPacket, uint8_t rawLen) {
    if (rawLen < sizeof(MeshHeaderEx)) return false;

    uint8_t buf[250];
    memcpy(buf, rawPacket, rawLen);

    MeshHeaderEx* hdr = (MeshHeaderEx*)buf;

    if (hdr->ttl == 0) return false;
    hdr->ttl--;

    EspNowHAL* hal = espnow_hal_ptr();
    if (!hal) return false;

    bool ok = hal->broadcast(buf, rawLen);
    if (ok) {
        _stats.relay_count++;
        _stats.tx_count++;
    } else {
        _stats.tx_fail++;
    }
    return ok;
}

// ── PING/PONG handling ──────────────────────────────────────────────────────

void MeshManager::handlePing(const MeshHeaderEx& hdr, const uint8_t* senderMac,
                              const uint8_t* payload, uint8_t payloadLen,
                              int8_t rssi) {
    (void)payload; (void)payloadLen; (void)rssi;

    // Respond with PONG containing our device info
    uint8_t buf[250];
    MeshHeaderEx& resp = *(MeshHeaderEx*)buf;
    MeshPongPayload pong = {};
    pong.role = (uint8_t)_role;
    pong.peer_count = (uint8_t)_peerCount;
    pong.battery_pct = _batteryPct;
    strncpy(pong.device_name, _deviceName, sizeof(pong.device_name) - 1);
    strncpy(pong.firmware_version, _firmwareVersion, sizeof(pong.firmware_version) - 1);
    strncpy(pong.board_type, _boardType, sizeof(pong.board_type) - 1);

    buildHeader(resp, MESH_EX_PONG, hdr.src_mac, sizeof(MeshPongPayload));
    memcpy(buf + sizeof(MeshHeaderEx), &pong, sizeof(MeshPongPayload));

    recordDedup(_mac, resp.seq);

    EspNowHAL* hal = espnow_hal_ptr();
    if (hal) {
        hal->send(senderMac, buf, sizeof(MeshHeaderEx) + sizeof(MeshPongPayload));
        _stats.tx_count++;
    }

    // If this is a new peer, send them our full state
    int idx = findPeerIdx(hdr.src_mac);
    if (idx >= 0 && _peers[idx].last_seen_ms == millis()) {
        // Freshly added — dump state
        broadcastStateDump(hdr.src_mac);
    }

    DBG_DEBUG(TAG, "PONG sent to %02X:%02X:%02X:%02X:%02X:%02X",
              senderMac[0], senderMac[1], senderMac[2],
              senderMac[3], senderMac[4], senderMac[5]);
}

void MeshManager::handlePong(const MeshHeaderEx& hdr, const uint8_t* payload,
                              uint8_t payloadLen, int8_t rssi) {
    (void)rssi;

    if (payloadLen < sizeof(MeshPongPayload)) return;

    const MeshPongPayload* pong = (const MeshPongPayload*)payload;

    int idx = findPeerIdx(hdr.src_mac);
    if (idx >= 0) {
        updatePeerInfo(idx, *pong);
    }

    DBG_DEBUG(TAG, "PONG from %02X:%02X:%02X:%02X:%02X:%02X name=%s board=%s",
              hdr.src_mac[0], hdr.src_mac[1], hdr.src_mac[2],
              hdr.src_mac[3], hdr.src_mac[4], hdr.src_mac[5],
              pong->device_name, pong->board_type);
}

// ── State sync handling ─────────────────────────────────────────────────────

void MeshManager::handleStateSync(const MeshHeaderEx& hdr, const uint8_t* payload,
                                   uint8_t payloadLen) {
    if (payloadLen < sizeof(MeshStateSyncPayload)) return;

    const MeshStateSyncPayload* sync = (const MeshStateSyncPayload*)payload;

    int idx = findStateIdx(sync->key);
    if (idx >= 0) {
        // LWW-CRDT: highest version wins
        if (sync->version <= _state[idx].version) return;

        strncpy(_state[idx].value, sync->value, MESH_STATE_VALUE_LEN - 1);
        _state[idx].value[MESH_STATE_VALUE_LEN - 1] = '\0';
        memcpy(_state[idx].src_mac, hdr.src_mac, 6);
        _state[idx].timestamp_ms = millis();
        _state[idx].version = sync->version;
    } else {
        // New entry
        if (_stateCount >= MESH_MAX_STATE) return;
        idx = _stateCount++;
        strncpy(_state[idx].key, sync->key, MESH_STATE_KEY_LEN - 1);
        _state[idx].key[MESH_STATE_KEY_LEN - 1] = '\0';
        strncpy(_state[idx].value, sync->value, MESH_STATE_VALUE_LEN - 1);
        _state[idx].value[MESH_STATE_VALUE_LEN - 1] = '\0';
        memcpy(_state[idx].src_mac, hdr.src_mac, 6);
        _state[idx].timestamp_ms = millis();
        _state[idx].version = sync->version;
    }

    _stats.state_syncs++;
}

void MeshManager::handleStateRequest(const MeshHeaderEx& hdr,
                                      const uint8_t* senderMac) {
    (void)senderMac;
    broadcastStateDump(hdr.src_mac);
}

void MeshManager::handleTopology(const MeshHeaderEx& hdr, const uint8_t* payload,
                                  uint8_t payloadLen) {
    if (payloadLen < 1) return;

    const MeshTopoPayload* topo = (const MeshTopoPayload*)payload;
    if (payloadLen < 1 + topo->neighbor_count * (int)sizeof(MeshTopoNeighbor)) return;

    // Add topology links from this node
    for (int i = 0; i < topo->neighbor_count && i < MESH_TOPO_MAX_NEIGHBORS; i++) {
        addLink(hdr.src_mac, topo->neighbors[i].mac, topo->neighbors[i].rssi);
    }
}

// ── Peer table ──────────────────────────────────────────────────────────────

int MeshManager::findPeerIdx(const uint8_t* mac) const {
    for (int i = 0; i < _peerCount; i++) {
        if (memcmp(_peers[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

void MeshManager::updatePeer(const uint8_t* mac, int8_t rssi, uint8_t hopCount) {
    // Don't track broadcast or self
    if (memcmp(mac, ESPNOW_BROADCAST, 6) == 0) return;
    if (memcmp(mac, _mac, 6) == 0) return;

    int idx = findPeerIdx(mac);
    if (idx >= 0) {
        _peers[idx].rssi = rssi;
        _peers[idx].last_seen_ms = millis();
        if (hopCount < _peers[idx].hop_count) {
            _peers[idx].hop_count = hopCount;
        }
    } else if (_peerCount < MESH_MAX_PEERS) {
        MeshPeerInfo& p = _peers[_peerCount];
        memset(&p, 0, sizeof(p));
        memcpy(p.mac, mac, 6);
        p.rssi = rssi;
        p.hop_count = hopCount;
        p.last_seen_ms = millis();
        p.role = MESH_ROLE_RELAY;  // Default until PONG received
        _peerCount++;

        DBG_INFO(TAG, "New peer: %02X:%02X:%02X:%02X:%02X:%02X rssi=%d hops=%d",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 rssi, hopCount);
    } else {
        // Evict oldest peer
        uint32_t oldestMs = UINT32_MAX;
        int oldestIdx = 0;
        for (int i = 0; i < _peerCount; i++) {
            uint32_t age = millis() - _peers[i].last_seen_ms;
            if (age > (millis() - oldestMs)) {
                oldestMs = _peers[i].last_seen_ms;
                oldestIdx = i;
            }
        }
        MeshPeerInfo& p = _peers[oldestIdx];
        memset(&p, 0, sizeof(p));
        memcpy(p.mac, mac, 6);
        p.rssi = rssi;
        p.hop_count = hopCount;
        p.last_seen_ms = millis();
        p.role = MESH_ROLE_RELAY;

        DBG_DEBUG(TAG, "Evicted oldest peer, replaced with %02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

void MeshManager::updatePeerInfo(int idx, const MeshPongPayload& info) {
    if (idx < 0 || idx >= _peerCount) return;

    _peers[idx].role = (MeshRole)info.role;
    _peers[idx].battery_pct = info.battery_pct;
    _peers[idx].is_gateway = (info.role == MESH_ROLE_GATEWAY);
    strncpy(_peers[idx].device_name, info.device_name,
            sizeof(_peers[idx].device_name) - 1);
    _peers[idx].device_name[sizeof(_peers[idx].device_name) - 1] = '\0';
    strncpy(_peers[idx].firmware_version, info.firmware_version,
            sizeof(_peers[idx].firmware_version) - 1);
    _peers[idx].firmware_version[sizeof(_peers[idx].firmware_version) - 1] = '\0';
    strncpy(_peers[idx].board_type, info.board_type,
            sizeof(_peers[idx].board_type) - 1);
    _peers[idx].board_type[sizeof(_peers[idx].board_type) - 1] = '\0';
}

void MeshManager::expirePeers() {
    uint32_t now = millis();
    for (int i = _peerCount - 1; i >= 0; i--) {
        if ((now - _peers[i].last_seen_ms) > PEER_EXPIRY_MS) {
            DBG_DEBUG(TAG, "Peer expired: %02X:%02X:%02X:%02X:%02X:%02X",
                      _peers[i].mac[0], _peers[i].mac[1], _peers[i].mac[2],
                      _peers[i].mac[3], _peers[i].mac[4], _peers[i].mac[5]);

            if (i < _peerCount - 1) {
                memmove(&_peers[i], &_peers[i + 1],
                        (_peerCount - i - 1) * sizeof(MeshPeerInfo));
            }
            _peerCount--;
        }
    }
}

// ── State helpers ───────────────────────────────────────────────────────────

int MeshManager::findStateIdx(const char* key) const {
    for (int i = 0; i < _stateCount; i++) {
        if (strncmp(_state[i].key, key, MESH_STATE_KEY_LEN) == 0) return i;
    }
    return -1;
}

// ── Broadcast helpers ───────────────────────────────────────────────────────

void MeshManager::broadcastDiscovery() {
    uint8_t buf[250];
    MeshHeaderEx& hdr = *(MeshHeaderEx*)buf;
    MeshPongPayload payload = {};
    payload.role = (uint8_t)_role;
    payload.peer_count = (uint8_t)_peerCount;
    payload.battery_pct = _batteryPct;
    strncpy(payload.device_name, _deviceName, sizeof(payload.device_name) - 1);
    strncpy(payload.firmware_version, _firmwareVersion, sizeof(payload.firmware_version) - 1);
    strncpy(payload.board_type, _boardType, sizeof(payload.board_type) - 1);

    buildHeader(hdr, MESH_EX_PING, ESPNOW_BROADCAST, sizeof(MeshPongPayload));
    memcpy(buf + sizeof(MeshHeaderEx), &payload, sizeof(MeshPongPayload));

    recordDedup(_mac, hdr.seq);

    EspNowHAL* hal = espnow_hal_ptr();
    if (hal) {
        hal->broadcast(buf, sizeof(MeshHeaderEx) + sizeof(MeshPongPayload));
        _stats.tx_count++;
    }

    // Also run gateway election periodically
    electGateway();

    DBG_DEBUG(TAG, "Discovery PING sent");
}

void MeshManager::broadcastTopology() {
    if (_peerCount == 0) return;

    uint8_t buf[250];
    MeshHeaderEx& hdr = *(MeshHeaderEx*)buf;
    MeshTopoPayload topo = {};

    int count = (_peerCount < MESH_TOPO_MAX_NEIGHBORS) ? _peerCount : MESH_TOPO_MAX_NEIGHBORS;
    topo.neighbor_count = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        memcpy(topo.neighbors[i].mac, _peers[i].mac, 6);
        topo.neighbors[i].rssi = _peers[i].rssi;
    }

    uint8_t payloadLen = 1 + count * sizeof(MeshTopoNeighbor);
    buildHeader(hdr, MESH_EX_TOPOLOGY, ESPNOW_BROADCAST, payloadLen);
    memcpy(buf + sizeof(MeshHeaderEx), &topo, payloadLen);

    recordDedup(_mac, hdr.seq);

    EspNowHAL* hal = espnow_hal_ptr();
    if (hal) {
        hal->broadcast(buf, sizeof(MeshHeaderEx) + payloadLen);
        _stats.tx_count++;
    }

    // Also add our own links to the topology table
    for (int i = 0; i < _peerCount; i++) {
        addLink(_mac, _peers[i].mac, _peers[i].rssi);
    }

    DBG_DEBUG(TAG, "Topology broadcast: %d neighbors", count);
}

void MeshManager::broadcastStateDump(const uint8_t* dst_mac) {
    if (_stateCount == 0) return;

    // Send state entries in batches that fit in one ESP-NOW frame
    int maxPerFrame = (ESPNOW_MAX_DATA - sizeof(MeshHeaderEx)) /
                       sizeof(MeshStateSyncPayload);
    if (maxPerFrame < 1) maxPerFrame = 1;

    for (int offset = 0; offset < _stateCount; offset += maxPerFrame) {
        int count = _stateCount - offset;
        if (count > maxPerFrame) count = maxPerFrame;

        uint8_t buf[250];
        MeshHeaderEx& hdr = *(MeshHeaderEx*)buf;
        uint8_t payloadLen = (uint8_t)(count * sizeof(MeshStateSyncPayload));

        buildHeader(hdr, MESH_EX_STATE_DUMP, dst_mac, payloadLen);

        uint8_t* p = buf + sizeof(MeshHeaderEx);
        for (int i = 0; i < count; i++) {
            MeshStateSyncPayload sync = {};
            strncpy(sync.key, _state[offset + i].key, MESH_STATE_KEY_LEN - 1);
            strncpy(sync.value, _state[offset + i].value, MESH_STATE_VALUE_LEN - 1);
            sync.version = _state[offset + i].version;
            memcpy(p, &sync, sizeof(MeshStateSyncPayload));
            p += sizeof(MeshStateSyncPayload);
        }

        recordDedup(_mac, hdr.seq);

        extern EspNowHAL* espnow_hal_ptr();
        EspNowHAL* hal = espnow_hal_ptr();
        if (hal) {
            hal->broadcast(buf, sizeof(MeshHeaderEx) + payloadLen);
            _stats.tx_count++;
        }
    }
}

// ── Topology link management ────────────────────────────────────────────────

void MeshManager::addLink(const uint8_t* from, const uint8_t* to, int8_t rssi) {
    // Check if link already exists (either direction)
    for (int i = 0; i < _linkCount; i++) {
        if ((memcmp(_links[i].from_mac, from, 6) == 0 &&
             memcmp(_links[i].to_mac, to, 6) == 0) ||
            (memcmp(_links[i].from_mac, to, 6) == 0 &&
             memcmp(_links[i].to_mac, from, 6) == 0)) {
            // Update existing link
            _links[i].rssi = rssi;
            _links[i].quality = rssiToQuality(rssi);
            return;
        }
    }

    // Add new link
    if (_linkCount < MESH_MAX_LINKS) {
        memcpy(_links[_linkCount].from_mac, from, 6);
        memcpy(_links[_linkCount].to_mac, to, 6);
        _links[_linkCount].rssi = rssi;
        _links[_linkCount].quality = rssiToQuality(rssi);
        _linkCount++;
    }
}

uint8_t MeshManager::rssiToQuality(int8_t rssi) const {
    // Map RSSI to 0-100 quality
    // -30 dBm = excellent (100), -90 dBm = unusable (0)
    if (rssi >= -30) return 100;
    if (rssi <= -90) return 0;
    return (uint8_t)(((rssi + 90) * 100) / 60);
}

// ── JSON export ─────────────────────────────────────────────────────────────

static void macToStr(const uint8_t* mac, char* out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char* roleToStr(MeshRole role) {
    switch (role) {
        case MESH_ROLE_GATEWAY: return "gateway";
        case MESH_ROLE_RELAY:   return "relay";
        case MESH_ROLE_LEAF:    return "leaf";
        case MESH_ROLE_SENSOR:  return "sensor";
        default: return "unknown";
    }
}

int MeshManager::toJson(char* buf, size_t size) const {
    char macStr[18];
    macToStr(_mac, macStr);

    int pos = snprintf(buf, size,
        "{\"enabled\":true,\"mac\":\"%s\",\"role\":\"%s\","
        "\"is_gateway\":%s,\"device_name\":\"%s\","
        "\"firmware\":\"%s\",\"board\":\"%s\",\"battery\":%u,"
        "\"stats\":{\"tx\":%lu,\"rx\":%lu,\"relay\":%lu,"
        "\"tx_fail\":%lu,\"dedup_drop\":%lu,\"state_syncs\":%lu},"
        "\"peer_count\":%d,\"state_count\":%d,\"link_count\":%d,",
        macStr, roleToStr(_role),
        isGateway() ? "true" : "false",
        _deviceName, _firmwareVersion, _boardType, _batteryPct,
        (unsigned long)_stats.tx_count, (unsigned long)_stats.rx_count,
        (unsigned long)_stats.relay_count, (unsigned long)_stats.tx_fail,
        (unsigned long)_stats.dedup_drop, (unsigned long)_stats.state_syncs,
        _peerCount, _stateCount, _linkCount);

    // Inline peers
    pos += snprintf(buf + pos, size - pos, "\"peers\":");
    pos += const_cast<MeshManager*>(this)->peersToJson(buf + pos, size - pos);
    pos += snprintf(buf + pos, size - pos, ",\"state\":");
    pos += const_cast<MeshManager*>(this)->stateToJson(buf + pos, size - pos);
    pos += snprintf(buf + pos, size - pos, ",\"topology\":");
    pos += const_cast<MeshManager*>(this)->topologyToJson(buf + pos, size - pos);
    pos += snprintf(buf + pos, size - pos, "}");

    return pos;
}

int MeshManager::peersToJson(char* buf, size_t size) const {
    int pos = snprintf(buf, size, "[");
    for (int i = 0; i < _peerCount && pos < (int)size - 256; i++) {
        char macStr[18];
        macToStr(_peers[i].mac, macStr);
        uint32_t ageSec = (millis() - _peers[i].last_seen_ms) / 1000;
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, size - pos,
            "{\"mac\":\"%s\",\"role\":\"%s\",\"rssi\":%d,\"hops\":%u,"
            "\"seen_ago\":%lu,\"name\":\"%s\",\"firmware\":\"%s\","
            "\"board\":\"%s\",\"battery\":%u,\"is_gateway\":%s}",
            macStr, roleToStr(_peers[i].role), _peers[i].rssi,
            _peers[i].hop_count, (unsigned long)ageSec,
            _peers[i].device_name, _peers[i].firmware_version,
            _peers[i].board_type, _peers[i].battery_pct,
            _peers[i].is_gateway ? "true" : "false");
    }
    pos += snprintf(buf + pos, size - pos, "]");
    return pos;
}

int MeshManager::topologyToJson(char* buf, size_t size) const {
    char macStr1[18], macStr2[18];
    int pos = snprintf(buf, size, "[");
    for (int i = 0; i < _linkCount && pos < (int)size - 128; i++) {
        macToStr(_links[i].from_mac, macStr1);
        macToStr(_links[i].to_mac, macStr2);
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, size - pos,
            "{\"from\":\"%s\",\"to\":\"%s\",\"rssi\":%d,\"quality\":%u}",
            macStr1, macStr2, _links[i].rssi, _links[i].quality);
    }
    pos += snprintf(buf + pos, size - pos, "]");
    return pos;
}

int MeshManager::stateToJson(char* buf, size_t size) const {
    int pos = snprintf(buf, size, "[");
    for (int i = 0; i < _stateCount && pos < (int)size - 128; i++) {
        char macStr[18];
        macToStr(_state[i].src_mac, macStr);
        uint32_t ageSec = (millis() - _state[i].timestamp_ms) / 1000;
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, size - pos,
            "{\"key\":\"%s\",\"value\":\"%s\",\"source\":\"%s\","
            "\"age\":%lu,\"version\":%u}",
            _state[i].key, _state[i].value, macStr,
            (unsigned long)ageSec, _state[i].version);
    }
    pos += snprintf(buf + pos, size - pos, "]");
    return pos;
}

// ============================================================================
// Platform: Desktop Simulator (stubs)
// ============================================================================
#else // SIMULATOR

#include <cstdio>
#include <cstdlib>

MeshManager& MeshManager::instance() {
    static MeshManager inst;
    return inst;
}

bool MeshManager::init(MeshRole role) {
    _role = role;
    memset(_mac, 0xAA, 6);
    _ready = true;
    memset(&_stats, 0, sizeof(_stats));
    _stats.role = role;
    printf("[MESH-SIM] Init OK, role=%d\n", (int)role);
    return true;
}

bool MeshManager::isReady() const { return _ready; }
void MeshManager::tick() {}
void MeshManager::getMAC(uint8_t mac[6]) const { memcpy(mac, _mac, 6); }
MeshRole MeshManager::getRole() const { return _role; }
void MeshManager::setRole(MeshRole role) { _role = role; _stats.role = role; }
void MeshManager::setDeviceName(const char* name) {
    strncpy(_deviceName, name, sizeof(_deviceName) - 1);
}
void MeshManager::setFirmwareVersion(const char* ver) {
    strncpy(_firmwareVersion, ver, sizeof(_firmwareVersion) - 1);
}
void MeshManager::setBoardType(const char* board) {
    strncpy(_boardType, board, sizeof(_boardType) - 1);
}
void MeshManager::setBatteryPercent(uint8_t pct) { _batteryPct = pct; }
bool MeshManager::broadcast(const uint8_t*, size_t) { return false; }
bool MeshManager::sendTo(const uint8_t*, const uint8_t*, size_t) { return false; }
void MeshManager::onMessage(MeshMessageCallback cb, void* ud) {
    _msgCb = cb; _msgCbUserData = ud;
}
int  MeshManager::getPeers(MeshPeerInfo*, int) const { return 0; }
int  MeshManager::peerCount() const { return 0; }
bool MeshManager::pingPeer(const uint8_t*) { return false; }
const MeshPeerInfo* MeshManager::findPeer(const uint8_t*) const { return nullptr; }
bool MeshManager::setState(const char*, const char*) { return false; }
const char* MeshManager::getState(const char*) const { return nullptr; }
int  MeshManager::getAllState(MeshStateEntry*, int) const { return 0; }
int  MeshManager::stateCount() const { return 0; }
int  MeshManager::getTopology(MeshLink*, int) const { return 0; }
int  MeshManager::linkCount() const { return 0; }
bool MeshManager::isGateway() const { return false; }
void MeshManager::electGateway() {}
const MeshManager::MeshStats& MeshManager::getStats() const { return _stats; }

int MeshManager::toJson(char* buf, size_t size) const {
    return snprintf(buf, size, "{\"enabled\":false,\"message\":\"Mesh simulator\"}");
}
int MeshManager::peersToJson(char* buf, size_t size) const {
    return snprintf(buf, size, "[]");
}
int MeshManager::topologyToJson(char* buf, size_t size) const {
    return snprintf(buf, size, "[]");
}
int MeshManager::stateToJson(char* buf, size_t size) const {
    return snprintf(buf, size, "[]");
}

// Stub private helpers
bool MeshManager::isDuplicate(const uint8_t*, uint16_t) { return false; }
void MeshManager::recordDedup(const uint8_t*, uint16_t) {}
void MeshManager::buildHeader(MeshHeaderEx&, MeshMsgTypeEx, const uint8_t*, uint8_t) {}
void MeshManager::handlePacket(const uint8_t*, const uint8_t*, int, int8_t) {}
bool MeshManager::relayPacket(const uint8_t*, uint8_t) { return false; }
void MeshManager::handlePing(const MeshHeaderEx&, const uint8_t*, const uint8_t*, uint8_t, int8_t) {}
void MeshManager::handlePong(const MeshHeaderEx&, const uint8_t*, uint8_t, int8_t) {}
void MeshManager::handleStateSync(const MeshHeaderEx&, const uint8_t*, uint8_t) {}
void MeshManager::handleStateRequest(const MeshHeaderEx&, const uint8_t*) {}
void MeshManager::handleTopology(const MeshHeaderEx&, const uint8_t*, uint8_t) {}
int  MeshManager::findPeerIdx(const uint8_t*) const { return -1; }
void MeshManager::updatePeer(const uint8_t*, int8_t, uint8_t) {}
void MeshManager::updatePeerInfo(int, const MeshPongPayload&) {}
void MeshManager::expirePeers() {}
int  MeshManager::findStateIdx(const char*) const { return -1; }
void MeshManager::broadcastDiscovery() {}
void MeshManager::broadcastTopology() {}
void MeshManager::broadcastStateDump(const uint8_t*) {}
void MeshManager::addLink(const uint8_t*, const uint8_t*, int8_t) {}
uint8_t MeshManager::rssiToQuality(int8_t) const { return 0; }
void MeshManager::onRawReceive(const uint8_t*, const uint8_t*, uint8_t, int8_t) {}

#endif // SIMULATOR
