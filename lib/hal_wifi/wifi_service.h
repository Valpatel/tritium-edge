/// @file wifi_service.h
/// @brief WiFi service adapter — wraps WifiManager as a ServiceInterface.
/// @copyright 2026 Valpatel Software LLC
/// @license AGPL-3.0-or-later
///
/// Priority 10: must init before all other network-dependent services.
/// Serial commands: WIFI_SCAN, WIFI_CONNECT, WIFI_ADD, WIFI_REMOVE,
///                  WIFI_LIST, WIFI_AP, WIFI_STATUS, WIFI_REORDER
#pragma once

#include "service.h"
#include "wifi_manager.h"
#include <cstdio>
#include <cstring>

#ifndef SIMULATOR
#include <WiFi.h>
#endif

class WifiService : public ServiceInterface {
public:
    const char* name() const override { return "wifi"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD | SVC_WEB_API; }
    int initPriority() const override { return 10; }

    bool init() override {
#ifndef SIMULATOR
        Serial.printf("[tritium] WiFi: connecting...\n");

#if defined(DEFAULT_WIFI_SSID) && defined(DEFAULT_WIFI_PASS)
        // Build-flag network: connect directly before WifiManager init
        // to avoid autoConnect picking a different saved network.
        WiFi.mode(WIFI_STA);
        WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
        Serial.printf("[WiFi] Connecting to %s...\n", DEFAULT_WIFI_SSID);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
            delay(250);
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WiFi] Connected to %s, IP: %s\n",
                          DEFAULT_WIFI_SSID, WiFi.localIP().toString().c_str());
        } else {
            Serial.printf("[WiFi] Failed to connect to %s\n", DEFAULT_WIFI_SSID);
            WiFi.disconnect(true);
        }

        // Now init the manager (it will detect the existing connection)
        _wifi.init();
        _wifi.addNetwork(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
#else
        _wifi.init();

        uint32_t start = millis();
        while (!_wifi.isConnected() && (millis() - start) < 10000) {
            delay(100);
        }
#endif

        if (_wifi.isConnected()) {
            Serial.printf("[tritium] WiFi: %s (%s)\n", _wifi.getSSID(), _wifi.getIP());
        } else {
            Serial.printf("[tritium] WiFi: not connected\n");
#if defined(ENABLE_WEBSERVER)
            // No WiFi? Start AP for commissioning
            Serial.printf("[tritium] Starting AP for commissioning...\n");
            _wifi.startAP();
            _wifi.startScan();
#else
            Serial.printf("[tritium] Will retry in background\n");
#endif
        }
#else
        _wifi.init();
#endif
        _initialized = true;
        return true;
    }

    void tick() override {
        _wifi.tick();
    }

    void shutdown() override {
        _wifi.shutdown();
    }

    // ── Serial command handler ───────────────────────────────────────────

    bool handleCommand(const char* cmd, const char* args) override {
        if (strcmp(cmd, "WIFI_STATUS") == 0) {
            return cmdStatus();
        }
        if (strcmp(cmd, "WIFI_SCAN") == 0) {
            return cmdScan();
        }
        if (strcmp(cmd, "WIFI_CONNECT") == 0) {
            return cmdConnect(args);
        }
        if (strcmp(cmd, "WIFI_ADD") == 0) {
            return cmdAdd(args);
        }
        if (strcmp(cmd, "WIFI_REMOVE") == 0) {
            return cmdRemove(args);
        }
        if (strcmp(cmd, "WIFI_LIST") == 0) {
            return cmdList();
        }
        if (strcmp(cmd, "WIFI_AP") == 0) {
            return cmdAP(args);
        }
        if (strcmp(cmd, "WIFI_REORDER") == 0) {
            return cmdReorder(args);
        }
        return false;
    }

    // ── JSON serialization for web API ───────────────────────────────────

    int toJson(char* buf, size_t size) override {
        WifiStatus st = _wifi.getStatus();

        int pos = snprintf(buf, size,
            "{\"connected\":%s,\"ap_active\":%s,"
            "\"ssid\":\"%s\",\"ip\":\"%s\","
            "\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
            "\"rssi\":%d,\"channel\":%u,"
            "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
            "\"clients\":%u,"
            "\"failover\":%s,"
            "\"saved_count\":%d,"
            "\"state\":\"%s\"",
            st.connected ? "true" : "false",
            st.ap_active ? "true" : "false",
            st.ssid, st.ip,
            st.ap_ssid, st.ap_ip,
            st.rssi, st.channel,
            st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5],
            st.clients_connected,
            _wifi.isAutoFailoverEnabled() ? "true" : "false",
            _wifi.getSavedCount(),
            stateStr(_wifi.getState()));

        // Append saved networks array
        pos += snprintf(buf + pos, size - pos, ",\"saved\":[");
        SavedNetwork nets[WIFI_MAX_SAVED_NETWORKS];
        int count = _wifi.getSavedNetworks(nets, WIFI_MAX_SAVED_NETWORKS);
        for (int i = 0; i < count && pos < (int)size - 120; i++) {
            if (i > 0) buf[pos++] = ',';
            pos += snprintf(buf + pos, size - pos,
                "{\"ssid\":\"%s\",\"priority\":%d,\"rssi\":%d,"
                "\"fails\":%u,\"enabled\":%s}",
                nets[i].ssid, nets[i].priority, nets[i].rssi_last,
                nets[i].fail_count, nets[i].enabled ? "true" : "false");
        }
        pos += snprintf(buf + pos, size - pos, "]}");
        return pos;
    }

    // ── Accessors for other services ─────────────────────────────────────

    bool isConnected() const { return _wifi.isConnected(); }
    const char* getIP() const { return _wifi.getIP(); }
    const char* getSSID() const { return _wifi.getSSID(); }
    bool isAPMode() const { return _wifi.isAPActive(); }
    const char* getAPIP() const {
        static char ap_ip[16];
        WifiStatus st = _wifi.getStatus();
        strncpy(ap_ip, st.ap_ip, sizeof(ap_ip) - 1);
        ap_ip[sizeof(ap_ip) - 1] = '\0';
        return ap_ip;
    }
    WifiManager& manager() { return _wifi; }

private:
    WifiManager _wifi;
    bool _initialized = false;

    // ── State name helper ────────────────────────────────────────────────

    static const char* stateStr(WifiState s) {
        switch (s) {
            case WifiState::IDLE:              return "idle";
            case WifiState::SCANNING:          return "scanning";
            case WifiState::CONNECTING:        return "connecting";
            case WifiState::CONNECTED:         return "connected";
            case WifiState::DISCONNECTED:      return "disconnected";
            case WifiState::RECONNECTING:      return "reconnecting";
            case WifiState::SCANNING_FALLBACK: return "failover";
            case WifiState::AP_ONLY:           return "ap_only";
            case WifiState::AP_AND_STA:        return "ap_and_sta";
            case WifiState::FAILED:            return "failed";
            default:                           return "unknown";
        }
    }

    // ── Serial command implementations ───────────────────────────────────

    bool cmdStatus() {
#ifndef SIMULATOR
        WifiStatus st = _wifi.getStatus();
        Serial.printf("[WiFi] State: %s\n", stateStr(_wifi.getState()));
        Serial.printf("[WiFi] SSID: %s  IP: %s  RSSI: %d dBm  Ch: %d\n",
                      st.ssid, st.ip, st.rssi, st.channel);
        Serial.printf("[WiFi] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      st.mac[0], st.mac[1], st.mac[2],
                      st.mac[3], st.mac[4], st.mac[5]);
        if (st.ap_active) {
            Serial.printf("[WiFi] AP: %s  IP: %s  Clients: %d\n",
                          st.ap_ssid, st.ap_ip, st.clients_connected);
        }
        Serial.printf("[WiFi] Failover: %s  Saved: %d\n",
                      _wifi.isAutoFailoverEnabled() ? "on" : "off",
                      _wifi.getSavedCount());
#endif
        return true;
    }

    bool cmdScan() {
#ifndef SIMULATOR
        Serial.println("[WiFi] Scanning...");
        if (!_wifi.startScan()) {
            Serial.println("[WiFi] Scan failed");
            return true;
        }
        ScanResult results[WIFI_MAX_SCAN_RESULTS];
        int count = _wifi.getScanResults(results, WIFI_MAX_SCAN_RESULTS);
        Serial.printf("[WiFi] Found %d networks:\n", count);
        for (int i = 0; i < count; i++) {
            Serial.printf("  %s%s  %d dBm  ch%d  %s\n",
                          results[i].ssid,
                          results[i].known ? " *" : "",
                          results[i].rssi,
                          results[i].channel,
                          results[i].auth == WifiAuth::OPEN ? "OPEN" : "ENC");
        }
#endif
        return true;
    }

    /// WIFI_CONNECT ssid [password]
    bool cmdConnect(const char* args) {
#ifndef SIMULATOR
        if (!args || strlen(args) == 0) {
            Serial.println("[WiFi] Usage: WIFI_CONNECT <ssid> [password]");
            return true;
        }
        char ssid[33] = {};
        char pass[65] = {};
        // Split on first space
        const char* sp = strchr(args, ' ');
        if (sp) {
            int len = sp - args;
            if (len > 32) len = 32;
            strncpy(ssid, args, len);
            strncpy(pass, sp + 1, sizeof(pass) - 1);
        } else {
            strncpy(ssid, args, sizeof(ssid) - 1);
        }
        if (_wifi.connectTo(ssid, pass, true)) {
            Serial.printf("[WiFi] Connected to %s\n", ssid);
        } else {
            Serial.printf("[WiFi] Failed to connect to %s\n", ssid);
        }
#endif
        return true;
    }

    /// WIFI_ADD ssid password [priority]
    bool cmdAdd(const char* args) {
#ifndef SIMULATOR
        if (!args || strlen(args) == 0) {
            Serial.println("[WiFi] Usage: WIFI_ADD <ssid> <password> [priority]");
            return true;
        }
        char ssid[33] = {}, pass[65] = {};
        int8_t priority = -1;
        // Parse: ssid<space>password[<space>priority]
        const char* sp1 = strchr(args, ' ');
        if (sp1) {
            int len = sp1 - args;
            if (len > 32) len = 32;
            strncpy(ssid, args, len);
            const char* sp2 = strchr(sp1 + 1, ' ');
            if (sp2) {
                int plen = sp2 - (sp1 + 1);
                if (plen > 64) plen = 64;
                strncpy(pass, sp1 + 1, plen);
                priority = (int8_t)atoi(sp2 + 1);
            } else {
                strncpy(pass, sp1 + 1, sizeof(pass) - 1);
            }
        } else {
            strncpy(ssid, args, sizeof(ssid) - 1);
        }
        if (_wifi.addNetwork(ssid, pass, priority)) {
            Serial.printf("[WiFi] Added network: %s (priority %d)\n", ssid, priority);
        } else {
            Serial.printf("[WiFi] Failed to add network: %s\n", ssid);
        }
#endif
        return true;
    }

    /// WIFI_REMOVE ssid
    bool cmdRemove(const char* args) {
#ifndef SIMULATOR
        if (!args || strlen(args) == 0) {
            Serial.println("[WiFi] Usage: WIFI_REMOVE <ssid>");
            return true;
        }
        if (_wifi.removeNetwork(args)) {
            Serial.printf("[WiFi] Removed: %s\n", args);
        } else {
            Serial.printf("[WiFi] Not found: %s\n", args);
        }
#endif
        return true;
    }

    /// WIFI_LIST — list all saved networks
    bool cmdList() {
#ifndef SIMULATOR
        SavedNetwork nets[WIFI_MAX_SAVED_NETWORKS];
        int count = _wifi.getSavedNetworks(nets, WIFI_MAX_SAVED_NETWORKS);
        Serial.printf("[WiFi] Saved networks (%d):\n", count);
        for (int i = 0; i < count; i++) {
            Serial.printf("  [%d] %s  pri=%d  rssi=%d  fails=%d  %s\n",
                          i, nets[i].ssid, nets[i].priority,
                          nets[i].rssi_last, nets[i].fail_count,
                          nets[i].enabled ? "enabled" : "disabled");
        }
#endif
        return true;
    }

    /// WIFI_AP [start|stop] [ssid] [password]
    bool cmdAP(const char* args) {
#ifndef SIMULATOR
        if (!args || strlen(args) == 0 || strncmp(args, "start", 5) == 0) {
            const char* ssid = nullptr;
            const char* pass = nullptr;
            // Parse optional ssid and password after "start "
            if (args && strncmp(args, "start", 5) == 0 && strlen(args) > 6) {
                ssid = args + 6;
                const char* sp = strchr(ssid, ' ');
                if (sp) {
                    // There's a password too — need to split
                    static char ap_ssid_buf[33];
                    int len = sp - ssid;
                    if (len > 32) len = 32;
                    strncpy(ap_ssid_buf, ssid, len);
                    ap_ssid_buf[len] = '\0';
                    ssid = ap_ssid_buf;
                    pass = sp + 1;
                }
            }
            if (_wifi.startAP(ssid, pass)) {
                Serial.println("[WiFi] AP started");
            } else {
                Serial.println("[WiFi] AP start failed");
            }
        } else if (strncmp(args, "stop", 4) == 0) {
            _wifi.stopAP();
            Serial.println("[WiFi] AP stopped");
        } else {
            Serial.println("[WiFi] Usage: WIFI_AP [start|stop] [ssid] [password]");
        }
#endif
        return true;
    }

    /// WIFI_REORDER ssid new_priority
    bool cmdReorder(const char* args) {
#ifndef SIMULATOR
        if (!args || strlen(args) == 0) {
            Serial.println("[WiFi] Usage: WIFI_REORDER <ssid> <priority>");
            return true;
        }
        const char* sp = strrchr(args, ' ');
        if (!sp) {
            Serial.println("[WiFi] Usage: WIFI_REORDER <ssid> <priority>");
            return true;
        }
        char ssid[33] = {};
        int len = sp - args;
        if (len > 32) len = 32;
        strncpy(ssid, args, len);
        int8_t pri = (int8_t)atoi(sp + 1);
        if (_wifi.reorderNetwork(ssid, pri)) {
            Serial.printf("[WiFi] %s -> priority %d\n", ssid, pri);
        } else {
            Serial.printf("[WiFi] Not found: %s\n", ssid);
        }
#endif
        return true;
    }
};
