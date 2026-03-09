#pragma once
// ESP-NOW Mesh service adapter — wraps EspNowHAL + MeshManager as a
// ServiceInterface.  Priority 50.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "service.h"

#if defined(ENABLE_ESPNOW) && __has_include("hal_espnow.h")
#include "hal_espnow.h"
#include "mesh_manager.h"
#endif

#if defined(ENABLE_DIAG) && __has_include("hal_diag.h")
#include "hal_diag.h"
#endif

// Global accessor for the underlying HAL — used by MeshManager to send frames.
// Defined at bottom of this header, declared extern in mesh_manager.cpp.
#if defined(ENABLE_ESPNOW)
inline EspNowHAL* espnow_hal_ptr();
#endif

class EspNowService : public ServiceInterface {
public:
    const char* name() const override { return "espnow"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_WEB_API; }
    int initPriority() const override { return 50; }

    bool init() override {
#if defined(ENABLE_ESPNOW)
        if (_espnow.init(EspNowRole::NODE, 1)) {
            Serial.printf("[tritium] ESP-NOW Mesh: active\n");
            _instance = this;

            // Register the raw receive callback so MeshManager gets enhanced
            // frames (magic 0x54).  The existing EspNowHAL::handleMeshPacket
            // only processes legacy 0xE5 frames; enhanced frames arrive via
            // the raw onReceive callback since they don't match 0xE5.
            _espnow.onReceive([](const uint8_t* src, const uint8_t* data,
                                 uint8_t len, int8_t rssi) {
                MeshManager::onRawReceive(src, data, len, rssi);
            });

            // Initialize the enhanced mesh manager
            MeshManager::instance().init(MESH_ROLE_RELAY);

#if defined(ENABLE_DIAG)
            // Diag logging provided by hal_diag provider, not EspNowHAL itself
            hal_diag::set_mesh_provider([](hal_diag::MeshInfo& out) -> bool {
                if (!_instance) return false;
                auto stats = _instance->_espnow.getStats();
                out.peer_count = _instance->_espnow.getPeerCount();
                out.route_count = (uint8_t)stats.discovery_count;
                out.tx_count = stats.tx_count;
                out.rx_count = stats.rx_count;
                out.tx_fail = stats.tx_fail;
                out.relay_count = stats.relay_count;
                EspNowPeer peers[hal_diag::MeshInfo::MAX_PEERS];
                int n = _instance->_espnow.getPeers(peers, hal_diag::MeshInfo::MAX_PEERS);
                out.peer_list_count = (uint8_t)n;
                for (int i = 0; i < n && i < hal_diag::MeshInfo::MAX_PEERS; i++) {
                    memcpy(out.peers[i].mac, peers[i].mac, 6);
                    out.peers[i].rssi = peers[i].rssi;
                    out.peers[i].hops = peers[i].hop_count;
                }
                return true;
            });
#endif
            // Run initial discovery to find neighbors immediately
            _espnow.meshDiscovery();
            _active = true;
            return true;
        } else {
            Serial.printf("[tritium] ESP-NOW Mesh: init failed\n");
            return false;
        }
#else
        return false;
#endif
    }

    void tick() override {
#if defined(ENABLE_ESPNOW)
        if (_active) {
            _espnow.process();
            MeshManager::instance().tick();
        }
#endif
    }

    int toJson(char* buf, size_t size) override {
#if defined(ENABLE_ESPNOW)
        if (_active) {
            return MeshManager::instance().toJson(buf, size);
        }
#endif
        (void)buf; (void)size;
        return 0;
    }

#if defined(ENABLE_ESPNOW)
    EspNowHAL& hal() { return _espnow; }
    MeshManager& mesh() { return MeshManager::instance(); }
#endif

private:
#if defined(ENABLE_ESPNOW)
    EspNowHAL _espnow;
    static EspNowService* _instance;
    friend EspNowHAL* espnow_hal_ptr();
#endif
    bool _active = false;
};

#if defined(ENABLE_ESPNOW)
inline EspNowService* EspNowService::_instance = nullptr;

// Provide global HAL pointer for MeshManager
inline EspNowHAL* espnow_hal_ptr() {
    return EspNowService::_instance ? &EspNowService::_instance->_espnow : nullptr;
}
#endif
