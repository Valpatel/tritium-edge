/// @file wifi_manager.cpp
/// @brief Enhanced WiFi management — ESP32 + simulator implementations.
/// @copyright 2026 Valpatel Software LLC
/// @license AGPL-3.0-or-later
#include "wifi_manager.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

static constexpr uint32_t MONITOR_INTERVAL_MS = 5000;
static constexpr uint32_t CONNECT_TIMEOUT_MS  = 15000;

WifiManager* WifiManager::_instance = nullptr;

// ============================================================================
// Platform: ESP32
// ============================================================================
#ifndef SIMULATOR

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>

static constexpr const char* NVS_NAMESPACE   = "trit_wifi";
static constexpr const char* NVS_KEY_COUNT   = "count";
static constexpr uint32_t MONITOR_STACK_SIZE = 4096;

static DNSServer* _apDns = nullptr;

// ── Auth mode mapping ────────────────────────────────────────────────────────

static WifiAuth mapAuthMode(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:            return WifiAuth::OPEN;
        case WIFI_AUTH_WEP:             return WifiAuth::WEP;
        case WIFI_AUTH_WPA_PSK:         return WifiAuth::WPA_PSK;
        case WIFI_AUTH_WPA2_PSK:        return WifiAuth::WPA2_PSK;
        case WIFI_AUTH_WPA_WPA2_PSK:    return WifiAuth::WPA_WPA2_PSK;
        case WIFI_AUTH_WPA3_PSK:        return WifiAuth::WPA3_PSK;
        default:                        return WifiAuth::UNKNOWN;
    }
}

// ── WiFi event handler ───────────────────────────────────────────────────────

static void wifiEventHandler(WiFiEvent_t event) {
    WifiManager* mgr = WifiManager::_instance;
    if (!mgr) return;
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            mgr->setState(WifiState::CONNECTED);
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (mgr->getState() == WifiState::CONNECTED) {
                mgr->setState(WifiState::DISCONNECTED);
            }
            break;
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.println("[WiFi] AP: client connected");
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.println("[WiFi] AP: client disconnected");
            break;
        default:
            break;
    }
}

// ── Constructor / destructor ─────────────────────────────────────────────────

WifiManager::WifiManager() {
    _instance = this;
    memset(_saved, 0, sizeof(_saved));
    memset(_scanResults, 0, sizeof(_scanResults));
    memset(&_status, 0, sizeof(_status));
}

WifiManager::~WifiManager() {
    shutdown();
    if (_instance == this) _instance = nullptr;
}

// ── init() ───────────────────────────────────────────────────────────────────

void WifiManager::init() {
    // Check if already connected (e.g., build-flag WiFi.begin() ran before init)
    bool already_connected = (WiFi.status() == WL_CONNECTED);

    if (!already_connected) {
        WiFi.mode(WIFI_STA);
    }
    WiFi.setAutoReconnect(false);
    WiFi.onEvent(wifiEventHandler);

    loadNetworks();

    if (already_connected) {
        // Preserve existing connection state
        strncpy(_ssid, WiFi.SSID().c_str(), sizeof(_ssid) - 1);
        strncpy(_ip, WiFi.localIP().toString().c_str(), sizeof(_ip) - 1);
        setState(WifiState::CONNECTED);
        _status.connected_since = millis();
        _wasConnected = true;
        Serial.printf("[WiFi] Already connected to %s, IP: %s\n", _ssid, _ip);
    } else {
        setState(WifiState::DISCONNECTED);
    }

    _running = true;
    TaskHandle_t handle = nullptr;
    xTaskCreatePinnedToCore(
        monitorTask, "wifi_mon", MONITOR_STACK_SIZE,
        this, 1, &handle, 0
    );
    _monitorTaskHandle = handle;

    if (!already_connected) {
        autoConnect();
    }

    updateStatus();
}

// ── tick() — call from main loop ─────────────────────────────────────────────

void WifiManager::tick() {
    // Service captive portal DNS if AP is active
    if (_apActive && _apDns) {
        _apDns->processNextRequest();
    }
}

// ── shutdown() ───────────────────────────────────────────────────────────────

void WifiManager::shutdown() {
    _running = false;
    if (_monitorTaskHandle) {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(static_cast<TaskHandle_t>(_monitorTaskHandle));
        _monitorTaskHandle = nullptr;
    }
    stopAP();
    disconnect();
    WiFi.removeEvent(wifiEventHandler);
}

// ── disconnect() ─────────────────────────────────────────────────────────────

void WifiManager::disconnect() {
    WiFi.disconnect(true);
    _ssid[0] = '\0';
    _ip[0] = '\0';
    _wasConnected = false;
    setState(WifiState::DISCONNECTED);
    updateStatus();
}

// ── connectTo() — connect to arbitrary SSID, optionally save ─────────────────

bool WifiManager::connectTo(const char* ssid, const char* password, bool save) {
    if (save) {
        addNetwork(ssid, password);
    }
    SavedNetwork tmp;
    memset(&tmp, 0, sizeof(tmp));
    strncpy(tmp.ssid, ssid, sizeof(tmp.ssid) - 1);
    if (password) strncpy(tmp.password, password, sizeof(tmp.password) - 1);
    tmp.enabled = true;
    return connectToNetwork(tmp);
}

// ── connectToNetwork() ───────────────────────────────────────────────────────

bool WifiManager::connectToNetwork(const SavedNetwork& net) {
    setState(WifiState::CONNECTING);
    Serial.printf("[WiFi] Connecting to %s...\n", net.ssid);

    WiFi.begin(net.ssid, net.password[0] ? net.password : nullptr);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (WiFi.status() == WL_CONNECTED) {
        strncpy(_ssid, net.ssid, sizeof(_ssid) - 1);
        strncpy(_ip, WiFi.localIP().toString().c_str(), sizeof(_ip) - 1);
        _status.connected_since = millis();
        _reconnectAttempts = 0;
        _wasConnected = true;

        // Update saved network stats
        for (int i = 0; i < _savedCount; i++) {
            if (strcmp(_saved[i].ssid, net.ssid) == 0) {
                _saved[i].rssi_last = (int8_t)WiFi.RSSI();
                _saved[i].last_connected = millis();
                _saved[i].fail_count = 0;
                _currentNetworkIdx = i;
                saveNetworks();
                break;
            }
        }

        setState(WifiState::CONNECTED);
        updateStatus();
        fireConnectCallbacks();
        Serial.printf("[WiFi] Connected to %s, IP: %s\n", _ssid, _ip);
        return true;
    }

    WiFi.disconnect(true);

    // Increment fail count on saved network
    for (int i = 0; i < _savedCount; i++) {
        if (strcmp(_saved[i].ssid, net.ssid) == 0) {
            if (_saved[i].fail_count < 255) _saved[i].fail_count++;
            break;
        }
    }

    setState(WifiState::FAILED);
    updateStatus();
    Serial.printf("[WiFi] Failed to connect to %s\n", net.ssid);
    return false;
}

// ── autoConnect() — scan and connect to best available saved network ─────────

bool WifiManager::autoConnect() {
    if (_savedCount == 0) return false;

    Serial.println("[WiFi] Scanning for known networks...");
    int found = WiFi.scanNetworks(false, false, false, 300);
    if (found <= 0) {
        Serial.println("[WiFi] No networks found");
        return false;
    }

    // Build candidate list: saved networks visible in scan, sorted by priority
    struct Candidate { int savedIdx; int32_t rssi; int8_t priority; };
    Candidate candidates[WIFI_MAX_SAVED_NETWORKS];
    int candidateCount = 0;

    for (int s = 0; s < found && candidateCount < WIFI_MAX_SAVED_NETWORKS; s++) {
        String scannedSSID = WiFi.SSID(s);
        for (int i = 0; i < _savedCount; i++) {
            if (!_saved[i].enabled) continue;
            if (scannedSSID == _saved[i].ssid) {
                _saved[i].rssi_last = (int8_t)WiFi.RSSI(s);
                candidates[candidateCount++] = {i, WiFi.RSSI(s), _saved[i].priority};
                break;
            }
        }
    }
    WiFi.scanDelete();

    if (candidateCount == 0) {
        Serial.println("[WiFi] No known networks in range");
        return false;
    }

    // Sort by priority first (lower = higher priority), then RSSI
    std::sort(candidates, candidates + candidateCount,
              [](const Candidate& a, const Candidate& b) {
                  if (a.priority != b.priority) return a.priority < b.priority;
                  return a.rssi > b.rssi;
              });

    for (int i = 0; i < candidateCount; i++) {
        if (connectToNetwork(_saved[candidates[i].savedIdx])) return true;
    }
    return false;
}

// ── tryNextNetwork() — failover to next network on disconnect ────────────────

void WifiManager::tryNextNetwork() {
    if (!_autoFailover || _savedCount == 0) return;

    setState(WifiState::SCANNING_FALLBACK);
    Serial.println("[WiFi] Failover: trying next network...");

    // Calculate backoff delay
    uint32_t backoff = RECONNECT_BASE_MS << _reconnectAttempts;
    if (backoff > RECONNECT_MAX_MS) backoff = RECONNECT_MAX_MS;
    if (_reconnectAttempts < 15) _reconnectAttempts++;

    vTaskDelay(pdMS_TO_TICKS(backoff));

    // Try current network first, then scan for others
    if (_currentNetworkIdx >= 0 && _currentNetworkIdx < _savedCount) {
        if (_saved[_currentNetworkIdx].enabled) {
            if (connectToNetwork(_saved[_currentNetworkIdx])) return;
        }
    }

    // Full auto-connect: scan and try all by priority
    autoConnect();
}

// ── startScan() ──────────────────────────────────────────────────────────────

bool WifiManager::startScan() {
    WifiState prev = _state;
    setState(WifiState::SCANNING);

    int found = WiFi.scanNetworks(false, false, false, 300);
    if (found < 0) {
        _scanResultCount = 0;
        setState(prev);
        return false;
    }

    _scanResultCount = (found < WIFI_MAX_SCAN_RESULTS) ? found : WIFI_MAX_SCAN_RESULTS;
    for (int i = 0; i < _scanResultCount; i++) {
        strncpy(_scanResults[i].ssid, WiFi.SSID(i).c_str(), sizeof(_scanResults[i].ssid) - 1);
        _scanResults[i].ssid[sizeof(_scanResults[i].ssid) - 1] = '\0';
        _scanResults[i].rssi = WiFi.RSSI(i);
        _scanResults[i].auth = mapAuthMode(WiFi.encryptionType(i));
        _scanResults[i].channel = WiFi.channel(i);

        // Mark known networks
        _scanResults[i].known = false;
        for (int j = 0; j < _savedCount; j++) {
            if (strcmp(_scanResults[i].ssid, _saved[j].ssid) == 0) {
                _scanResults[i].known = true;
                _saved[j].rssi_last = (int8_t)_scanResults[i].rssi;
                break;
            }
        }
    }
    WiFi.scanDelete();

    setState(WiFi.status() == WL_CONNECTED ? WifiState::CONNECTED : WifiState::DISCONNECTED);
    return true;
}

// ── AP mode ──────────────────────────────────────────────────────────────────

bool WifiManager::startAP(const char* ssid, const char* password) {
    // Generate default SSID from MAC if not provided
    if (!ssid || strlen(ssid) == 0) {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(_apSSID, sizeof(_apSSID), "Tritium-%02X%02X", mac[4], mac[5]);
    } else {
        strncpy(_apSSID, ssid, sizeof(_apSSID) - 1);
        _apSSID[sizeof(_apSSID) - 1] = '\0';
    }

    if (password && strlen(password) >= 8) {
        strncpy(_apPassword, password, sizeof(_apPassword) - 1);
        _apPassword[sizeof(_apPassword) - 1] = '\0';
    } else {
        _apPassword[0] = '\0';
    }

    // Switch to AP+STA mode so we can maintain STA connection while hosting AP
    WiFi.mode(WIFI_AP_STA);

    bool ok;
    if (_apPassword[0]) {
        ok = WiFi.softAP(_apSSID, _apPassword);
    } else {
        ok = WiFi.softAP(_apSSID);
    }

    if (!ok) {
        Serial.printf("[WiFi] Failed to start AP: %s\n", _apSSID);
        return false;
    }

    // Start captive portal DNS — redirect all queries to our AP IP
    if (!_apDns) {
        _apDns = new DNSServer();
    }
    _apDns->start(53, "*", WiFi.softAPIP());

    _apActive = true;
    strncpy(_status.ap_ssid, _apSSID, sizeof(_status.ap_ssid) - 1);
    strncpy(_status.ap_ip, WiFi.softAPIP().toString().c_str(), sizeof(_status.ap_ip) - 1);

    if (_state == WifiState::CONNECTED) {
        setState(WifiState::AP_AND_STA);
    } else {
        setState(WifiState::AP_ONLY);
    }

    updateStatus();
    Serial.printf("[WiFi] AP started: %s (IP: %s, %s)\n",
                  _apSSID, _status.ap_ip,
                  _apPassword[0] ? "secured" : "open");
    return true;
}

bool WifiManager::stopAP() {
    if (!_apActive) return false;

    if (_apDns) {
        _apDns->stop();
        delete _apDns;
        _apDns = nullptr;
    }

    WiFi.softAPdisconnect(true);

    // Switch back to STA-only if still connected
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.mode(WIFI_STA);
    }

    _apActive = false;
    _status.ap_active = false;
    _status.ap_ip[0] = '\0';
    _status.ap_ssid[0] = '\0';
    _status.clients_connected = 0;

    if (_state == WifiState::AP_AND_STA) {
        setState(WifiState::CONNECTED);
    } else if (_state == WifiState::AP_ONLY) {
        setState(WifiState::DISCONNECTED);
    }

    updateStatus();
    Serial.println("[WiFi] AP stopped");
    return true;
}

bool WifiManager::isAPActive() const { return _apActive; }

// ── Status helpers ───────────────────────────────────────────────────────────

bool    WifiManager::isConnected() const { return WiFi.status() == WL_CONNECTED; }
int32_t WifiManager::getRSSI()     const { return WiFi.RSSI(); }

void WifiManager::updateStatus() {
    _status.connected = (WiFi.status() == WL_CONNECTED);
    _status.ap_active = _apActive;

    if (_status.connected) {
        strncpy(_status.ssid, _ssid, sizeof(_status.ssid) - 1);
        strncpy(_status.ip, _ip, sizeof(_status.ip) - 1);
        _status.rssi = (int8_t)WiFi.RSSI();
        _status.channel = WiFi.channel();
    } else {
        _status.ssid[0] = '\0';
        _status.ip[0] = '\0';
        _status.rssi = 0;
        _status.channel = 0;
    }

    WiFi.macAddress(_status.mac);

    if (_apActive) {
        _status.clients_connected = WiFi.softAPgetStationNum();
        strncpy(_status.ap_ssid, _apSSID, sizeof(_status.ap_ssid) - 1);
        strncpy(_status.ap_ip, WiFi.softAPIP().toString().c_str(), sizeof(_status.ap_ip) - 1);
    }
}

WifiStatus WifiManager::getStatus() const {
    return _status;
}

// ── Callbacks ────────────────────────────────────────────────────────────────

void WifiManager::fireConnectCallbacks() {
    updateStatus();
    for (int i = 0; i < _connectCBCount; i++) {
        if (_connectCBs[i].cb) {
            _connectCBs[i].cb(_status, _connectCBs[i].user_data);
        }
    }
}

void WifiManager::fireDisconnectCallbacks() {
    updateStatus();
    for (int i = 0; i < _disconnectCBCount; i++) {
        if (_disconnectCBs[i].cb) {
            _disconnectCBs[i].cb(_status, _disconnectCBs[i].user_data);
        }
    }
}

// ── NVS persistence ──────────────────────────────────────────────────────────

void WifiManager::loadNetworks() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
        // Namespace doesn't exist — create it with a write-mode open
        prefs.begin(NVS_NAMESPACE, false);
        prefs.end();
        prefs.begin(NVS_NAMESPACE, true);
    }
    _savedCount = prefs.getInt(NVS_KEY_COUNT, 0);
    if (_savedCount > WIFI_MAX_SAVED_NETWORKS) _savedCount = WIFI_MAX_SAVED_NETWORKS;

    for (int i = 0; i < _savedCount; i++) {
        char keyS[8], keyP[8], keyPr[8], keyE[8];
        snprintf(keyS,  sizeof(keyS),  "s%d", i);
        snprintf(keyP,  sizeof(keyP),  "p%d", i);
        snprintf(keyPr, sizeof(keyPr), "pr%d", i);
        snprintf(keyE,  sizeof(keyE),  "e%d", i);

        String ssid = prefs.getString(keyS, "");
        String pass = prefs.getString(keyP, "");

        strncpy(_saved[i].ssid, ssid.c_str(), sizeof(_saved[i].ssid) - 1);
        _saved[i].ssid[sizeof(_saved[i].ssid) - 1] = '\0';
        strncpy(_saved[i].password, pass.c_str(), sizeof(_saved[i].password) - 1);
        _saved[i].password[sizeof(_saved[i].password) - 1] = '\0';

        _saved[i].priority       = prefs.getChar(keyPr, (int8_t)i);
        _saved[i].enabled        = prefs.getBool(keyE, true);
        _saved[i].rssi_last      = 0;
        _saved[i].last_connected = 0;
        _saved[i].fail_count     = 0;
    }
    prefs.end();
    Serial.printf("[WiFi] Loaded %d saved networks from NVS\n", _savedCount);
}

void WifiManager::saveNetworks() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putInt(NVS_KEY_COUNT, _savedCount);

    for (int i = 0; i < WIFI_MAX_SAVED_NETWORKS; i++) {
        char keyS[8], keyP[8], keyPr[8], keyE[8];
        snprintf(keyS,  sizeof(keyS),  "s%d", i);
        snprintf(keyP,  sizeof(keyP),  "p%d", i);
        snprintf(keyPr, sizeof(keyPr), "pr%d", i);
        snprintf(keyE,  sizeof(keyE),  "e%d", i);

        if (i < _savedCount) {
            prefs.putString(keyS, _saved[i].ssid);
            prefs.putString(keyP, _saved[i].password);
            prefs.putChar(keyPr, _saved[i].priority);
            prefs.putBool(keyE, _saved[i].enabled);
        } else {
            prefs.remove(keyS);
            prefs.remove(keyP);
            prefs.remove(keyPr);
            prefs.remove(keyE);
        }
    }
    prefs.end();
}

// ── Monitor task ─────────────────────────────────────────────────────────────

void WifiManager::monitorTask(void* param) {
    WifiManager* mgr = static_cast<WifiManager*>(param);
    while (mgr->_running) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));
        if (!mgr->_running) break;

        // Detect disconnect and attempt failover
        if (mgr->_wasConnected && WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Connection lost, attempting failover...");
            mgr->_wasConnected = false;
            mgr->setState(WifiState::RECONNECTING);
            mgr->fireDisconnectCallbacks();
            mgr->tryNextNetwork();
        }

        // Background scan to update RSSI of saved networks
        uint32_t now = millis();
        if (mgr->_autoFailover &&
            (now - mgr->_lastBackgroundScan) >= mgr->BACKGROUND_SCAN_MS &&
            WiFi.status() == WL_CONNECTED)
        {
            mgr->_lastBackgroundScan = now;
            // Quick scan to update RSSI values (don't change state)
            int found = WiFi.scanNetworks(false, false, false, 200);
            if (found > 0) {
                for (int s = 0; s < found; s++) {
                    String scannedSSID = WiFi.SSID(s);
                    for (int i = 0; i < mgr->_savedCount; i++) {
                        if (scannedSSID == mgr->_saved[i].ssid) {
                            mgr->_saved[i].rssi_last = (int8_t)WiFi.RSSI(s);
                            break;
                        }
                    }
                }
            }
            WiFi.scanDelete();
        }

        // Update status periodically
        mgr->updateStatus();
    }
    vTaskDelete(nullptr);
}

// ============================================================================
// Platform: Desktop Simulator
// ============================================================================
#else // SIMULATOR

#include <cstdio>
#include <thread>
#include <chrono>
#include <atomic>

static std::atomic<bool> sim_monitor_running{false};
static std::thread sim_monitor_thread;

WifiManager::WifiManager() {
    _instance = this;
    memset(_saved, 0, sizeof(_saved));
    memset(_scanResults, 0, sizeof(_scanResults));
    memset(&_status, 0, sizeof(_status));
}

WifiManager::~WifiManager() {
    shutdown();
    if (_instance == this) _instance = nullptr;
}

void WifiManager::init() {
    loadNetworks();
    setState(WifiState::DISCONNECTED);

    _running = true;
    sim_monitor_running = true;
    sim_monitor_thread = std::thread(monitorTask, this);

    printf("[WiFi-SIM] Initialized with %d saved networks\n", _savedCount);
    if (_savedCount > 0) {
        connectToNetwork(_saved[0]);
    }
}

void WifiManager::tick() {
    // No captive portal DNS in simulator
}

void WifiManager::shutdown() {
    _running = false;
    sim_monitor_running = false;
    if (sim_monitor_thread.joinable()) {
        sim_monitor_thread.join();
    }
    disconnect();
}

void WifiManager::disconnect() {
    _ssid[0] = '\0';
    _ip[0] = '\0';
    _wasConnected = false;
    setState(WifiState::DISCONNECTED);
    updateStatus();
}

bool WifiManager::connectTo(const char* ssid, const char* password, bool save) {
    if (save) addNetwork(ssid, password);
    SavedNetwork tmp;
    memset(&tmp, 0, sizeof(tmp));
    strncpy(tmp.ssid, ssid, sizeof(tmp.ssid) - 1);
    if (password) strncpy(tmp.password, password, sizeof(tmp.password) - 1);
    tmp.enabled = true;
    return connectToNetwork(tmp);
}

bool WifiManager::connectToNetwork(const SavedNetwork& net) {
    setState(WifiState::CONNECTING);
    printf("[WiFi-SIM] Connecting to %s...\n", net.ssid);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    strncpy(_ssid, net.ssid, sizeof(_ssid) - 1);
    strncpy(_ip, "192.168.1.100", sizeof(_ip) - 1);
    _status.connected_since = 0;  // No millis() in sim
    _wasConnected = true;
    _reconnectAttempts = 0;
    setState(WifiState::CONNECTED);
    updateStatus();
    fireConnectCallbacks();
    printf("[WiFi-SIM] Connected to %s, IP: %s\n", _ssid, _ip);
    return true;
}

bool WifiManager::autoConnect() {
    if (_savedCount == 0) return false;
    // Try first enabled network
    for (int i = 0; i < _savedCount; i++) {
        if (_saved[i].enabled) return connectToNetwork(_saved[i]);
    }
    return false;
}

void WifiManager::tryNextNetwork() {
    autoConnect();
}

bool WifiManager::startScan() {
    setState(WifiState::SCANNING);

    _scanResultCount = 3;
    strncpy(_scanResults[0].ssid, "SimNetwork-5G", sizeof(_scanResults[0].ssid));
    _scanResults[0].rssi = -45;
    _scanResults[0].auth = WifiAuth::WPA2_PSK;
    _scanResults[0].channel = 36;
    _scanResults[0].known = false;

    strncpy(_scanResults[1].ssid, "SimNetwork-2G", sizeof(_scanResults[1].ssid));
    _scanResults[1].rssi = -55;
    _scanResults[1].auth = WifiAuth::WPA2_PSK;
    _scanResults[1].channel = 6;
    _scanResults[1].known = false;

    strncpy(_scanResults[2].ssid, "OpenCafe", sizeof(_scanResults[2].ssid));
    _scanResults[2].rssi = -72;
    _scanResults[2].auth = WifiAuth::OPEN;
    _scanResults[2].channel = 1;
    _scanResults[2].known = false;

    // Mark known networks
    for (int i = 0; i < _scanResultCount; i++) {
        for (int j = 0; j < _savedCount; j++) {
            if (strcmp(_scanResults[i].ssid, _saved[j].ssid) == 0) {
                _scanResults[i].known = true;
                break;
            }
        }
    }

    setState(_ssid[0] ? WifiState::CONNECTED : WifiState::DISCONNECTED);
    return true;
}

bool WifiManager::startAP(const char* ssid, const char* password) {
    if (!ssid || strlen(ssid) == 0) {
        strncpy(_apSSID, "Tritium-SIM0", sizeof(_apSSID) - 1);
    } else {
        strncpy(_apSSID, ssid, sizeof(_apSSID) - 1);
    }
    if (password && strlen(password) >= 8) {
        strncpy(_apPassword, password, sizeof(_apPassword) - 1);
    }
    _apActive = true;
    strncpy(_status.ap_ssid, _apSSID, sizeof(_status.ap_ssid) - 1);
    strncpy(_status.ap_ip, "192.168.4.1", sizeof(_status.ap_ip) - 1);
    updateStatus();
    printf("[WiFi-SIM] AP started: %s\n", _apSSID);
    return true;
}

bool WifiManager::stopAP() {
    if (!_apActive) return false;
    _apActive = false;
    _status.ap_active = false;
    _status.ap_ip[0] = '\0';
    _status.ap_ssid[0] = '\0';
    updateStatus();
    printf("[WiFi-SIM] AP stopped\n");
    return true;
}

bool WifiManager::isAPActive() const { return _apActive; }
bool WifiManager::isConnected() const { return _state == WifiState::CONNECTED; }
int32_t WifiManager::getRSSI() const { return _state == WifiState::CONNECTED ? -50 : 0; }

void WifiManager::updateStatus() {
    _status.connected = (_state == WifiState::CONNECTED);
    _status.ap_active = _apActive;
    if (_status.connected) {
        strncpy(_status.ssid, _ssid, sizeof(_status.ssid) - 1);
        strncpy(_status.ip, _ip, sizeof(_status.ip) - 1);
        _status.rssi = -50;
        _status.channel = 6;
    }
}

WifiStatus WifiManager::getStatus() const { return _status; }

void WifiManager::fireConnectCallbacks() {
    for (int i = 0; i < _connectCBCount; i++) {
        if (_connectCBs[i].cb) _connectCBs[i].cb(_status, _connectCBs[i].user_data);
    }
}

void WifiManager::fireDisconnectCallbacks() {
    for (int i = 0; i < _disconnectCBCount; i++) {
        if (_disconnectCBs[i].cb) _disconnectCBs[i].cb(_status, _disconnectCBs[i].user_data);
    }
}

void WifiManager::loadNetworks() {
    _savedCount = 0;
    printf("[WiFi-SIM] No persistent storage, starting fresh\n");
}

void WifiManager::saveNetworks() {
    printf("[WiFi-SIM] Saved %d networks (in-memory only)\n", _savedCount);
}

void WifiManager::monitorTask(void* param) {
    WifiManager* mgr = static_cast<WifiManager*>(param);
    while (mgr->_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(MONITOR_INTERVAL_MS));
    }
}

#endif // SIMULATOR

// ============================================================================
// Shared (platform-independent) implementation
// ============================================================================

// ── Network management ───────────────────────────────────────────────────────

bool WifiManager::addNetwork(const char* ssid, const char* password, int8_t priority) {
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) return false;
    if (password && strlen(password) > 64) return false;

    // Update existing network
    for (int i = 0; i < _savedCount; i++) {
        if (strcmp(_saved[i].ssid, ssid) == 0) {
            strncpy(_saved[i].password, password ? password : "", sizeof(_saved[i].password) - 1);
            _saved[i].password[sizeof(_saved[i].password) - 1] = '\0';
            if (priority >= 0) _saved[i].priority = priority;
            _saved[i].enabled = true;
            saveNetworks();
            return true;
        }
    }

    if (_savedCount >= WIFI_MAX_SAVED_NETWORKS) return false;

    memset(&_saved[_savedCount], 0, sizeof(SavedNetwork));
    strncpy(_saved[_savedCount].ssid, ssid, sizeof(_saved[_savedCount].ssid) - 1);
    _saved[_savedCount].ssid[sizeof(_saved[_savedCount].ssid) - 1] = '\0';
    strncpy(_saved[_savedCount].password, password ? password : "", sizeof(_saved[_savedCount].password) - 1);
    _saved[_savedCount].password[sizeof(_saved[_savedCount].password) - 1] = '\0';
    _saved[_savedCount].priority = (priority >= 0) ? priority : (int8_t)_savedCount;
    _saved[_savedCount].enabled = true;
    _saved[_savedCount].fail_count = 0;
    _saved[_savedCount].rssi_last = 0;
    _saved[_savedCount].last_connected = 0;
    _savedCount++;
    saveNetworks();
    return true;
}

bool WifiManager::removeNetwork(const char* ssid) {
    for (int i = 0; i < _savedCount; i++) {
        if (strcmp(_saved[i].ssid, ssid) == 0) {
            for (int j = i; j < _savedCount - 1; j++) {
                _saved[j] = _saved[j + 1];
            }
            _savedCount--;
            memset(&_saved[_savedCount], 0, sizeof(SavedNetwork));
            saveNetworks();
            return true;
        }
    }
    return false;
}

bool WifiManager::reorderNetwork(const char* ssid, int8_t new_priority) {
    for (int i = 0; i < _savedCount; i++) {
        if (strcmp(_saved[i].ssid, ssid) == 0) {
            _saved[i].priority = new_priority;
            // Re-sort all priorities to maintain contiguous ordering
            // Simple: just assign the new priority and let the sort in autoConnect handle it
            saveNetworks();
            return true;
        }
    }
    return false;
}

int WifiManager::getSavedNetworks(SavedNetwork* out, int maxCount) const {
    int count = (_savedCount < maxCount) ? _savedCount : maxCount;
    memcpy(out, _saved, count * sizeof(SavedNetwork));
    return count;
}

int WifiManager::getSavedCount() const { return _savedCount; }

bool WifiManager::connect() { return autoConnect(); }

bool WifiManager::connect(const char* ssid) {
    for (int i = 0; i < _savedCount; i++) {
        if (strcmp(_saved[i].ssid, ssid) == 0) {
            return connectToNetwork(_saved[i]);
        }
    }
    return false;
}

int WifiManager::getScanResults(ScanResult* out, int maxCount) const {
    int count = (_scanResultCount < maxCount) ? _scanResultCount : maxCount;
    memcpy(out, _scanResults, count * sizeof(ScanResult));
    return count;
}

// ── Status accessors ─────────────────────────────────────────────────────────

WifiState   WifiManager::getState() const { return _state; }
const char* WifiManager::getIP()    const { return _ip; }
const char* WifiManager::getSSID()  const { return _ssid; }

// ── Auto-failover ────────────────────────────────────────────────────────────

void WifiManager::enableAutoFailover(bool enable) { _autoFailover = enable; }
bool WifiManager::isAutoFailoverEnabled() const { return _autoFailover; }

// ── Callbacks ────────────────────────────────────────────────────────────────

void WifiManager::onStateChange(WifiStateCallback cb) { _stateCallback = cb; }

void WifiManager::onConnect(WifiStatusCallback cb, void* user_data) {
    if (_connectCBCount < WIFI_MAX_CALLBACKS) {
        _connectCBs[_connectCBCount++] = {cb, user_data};
    }
}

void WifiManager::onDisconnect(WifiStatusCallback cb, void* user_data) {
    if (_disconnectCBCount < WIFI_MAX_CALLBACKS) {
        _disconnectCBs[_disconnectCBCount++] = {cb, user_data};
    }
}

void WifiManager::setState(WifiState s) {
    if (_state != s) {
        _state = s;
        if (_stateCallback) _stateCallback(s);
    }
}
