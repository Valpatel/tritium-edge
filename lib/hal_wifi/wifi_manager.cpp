#include "wifi_manager.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

static constexpr uint32_t MONITOR_INTERVAL_MS = 10000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

WifiManager* WifiManager::_instance = nullptr;

// ============================================================================
// Platform: ESP32
// ============================================================================
#ifndef SIMULATOR

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#if __has_include("hal_diag.h")
#include "hal_diag.h"
#define WIFI_HAS_DIAG 1
#else
#define WIFI_HAS_DIAG 0
#endif

static constexpr const char* NVS_NAMESPACE = "wifi_mgr";
static constexpr const char* NVS_KEY_COUNT = "count";
static constexpr uint32_t MONITOR_STACK_SIZE = 4096;

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

static uint32_t _wifi_disconnect_count = 0;

static void wifiEventHandler(WiFiEvent_t event) {
    WifiManager* mgr = WifiManager::_instance;
    if (!mgr) return;
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            mgr->setState(WifiState::CONNECTED);
#if WIFI_HAS_DIAG
            hal_diag::log(hal_diag::Severity::INFO, "wifi", "Connected IP: %s",
                          WiFi.localIP().toString().c_str());
#endif
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (mgr->getState() == WifiState::CONNECTED) {
                _wifi_disconnect_count++;
                mgr->setState(WifiState::DISCONNECTED);
#if WIFI_HAS_DIAG
                hal_diag::log(hal_diag::Severity::WARN, "wifi",
                              "Disconnected (#%lu)", (unsigned long)_wifi_disconnect_count);
#endif
            }
            break;
        default:
            break;
    }
}

WifiManager::WifiManager() {
    _instance = this;
    memset(_saved, 0, sizeof(_saved));
    memset(_scanResults, 0, sizeof(_scanResults));
}

WifiManager::~WifiManager() {
    shutdown();
    if (_instance == this) _instance = nullptr;
}

void WifiManager::init() {
    WiFi.persistent(true);           // Cache last connection in flash for fast reconnect
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);     // Auto-reconnect on disconnect
    WiFi.onEvent(wifiEventHandler);

    loadNetworks();
    setState(WifiState::DISCONNECTED);

    _running = true;
    TaskHandle_t handle = nullptr;
    xTaskCreatePinnedToCore(
        monitorTask, "wifi_mon", MONITOR_STACK_SIZE,
        this, 1, &handle, 0
    );
    _monitorTaskHandle = handle;

    autoConnect();
}

void WifiManager::shutdown() {
    _running = false;
    if (_monitorTaskHandle) {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(static_cast<TaskHandle_t>(_monitorTaskHandle));
        _monitorTaskHandle = nullptr;
    }
    disconnect();
    WiFi.removeEvent(wifiEventHandler);
}

void WifiManager::disconnect() {
    WiFi.disconnect(true);
    _ssid[0] = '\0';
    _ip[0] = '\0';
    setState(WifiState::DISCONNECTED);
}

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
        setState(WifiState::CONNECTED);
        Serial.printf("[WiFi] Connected to %s, IP: %s\n", _ssid, _ip);
        return true;
    }

    WiFi.disconnect(true);
    setState(WifiState::FAILED);
    Serial.printf("[WiFi] Failed to connect to %s\n", net.ssid);
#if WIFI_HAS_DIAG
    hal_diag::log(hal_diag::Severity::WARN, "wifi", "Connect failed: %s", net.ssid);
#endif
    return false;
}

bool WifiManager::autoConnect() {
    if (_savedCount == 0) return false;

    Serial.println("[WiFi] Scanning for known networks...");
    int found = WiFi.scanNetworks(false, false, false, 300);
    if (found <= 0) {
        Serial.println("[WiFi] No networks found");
        return false;
    }

    struct Candidate { int savedIdx; int32_t rssi; };
    Candidate candidates[WIFI_MAX_SAVED_NETWORKS];
    int candidateCount = 0;

    for (int s = 0; s < found && candidateCount < WIFI_MAX_SAVED_NETWORKS; s++) {
        String scannedSSID = WiFi.SSID(s);
        for (int i = 0; i < _savedCount; i++) {
            if (scannedSSID == _saved[i].ssid) {
                candidates[candidateCount++] = {i, WiFi.RSSI(s)};
                break;
            }
        }
    }
    WiFi.scanDelete();

    if (candidateCount == 0) {
        Serial.println("[WiFi] No known networks in range");
        return false;
    }

    std::sort(candidates, candidates + candidateCount,
              [](const Candidate& a, const Candidate& b) { return a.rssi > b.rssi; });

    for (int i = 0; i < candidateCount; i++) {
        if (connectToNetwork(_saved[candidates[i].savedIdx])) return true;
    }
    return false;
}

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
    }
    WiFi.scanDelete();

    setState(WiFi.status() == WL_CONNECTED ? WifiState::CONNECTED : WifiState::DISCONNECTED);
    return true;
}

bool WifiManager::startAP(const char* ssid, const char* password) {
    char ap_ssid[33];
    if (ssid) {
        strncpy(ap_ssid, ssid, sizeof(ap_ssid) - 1);
        ap_ssid[sizeof(ap_ssid) - 1] = '\0';
    } else {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(ap_ssid, sizeof(ap_ssid), "Tritium-%02X%02X", mac[4], mac[5]);
    }

    WiFi.mode(WIFI_AP_STA);
    bool ok;
    if (password && strlen(password) >= 8) {
        ok = WiFi.softAP(ap_ssid, password);
    } else {
        ok = WiFi.softAP(ap_ssid);
    }

    if (ok) {
        IPAddress apIP = WiFi.softAPIP();
        snprintf(_ip, sizeof(_ip), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
        strncpy(_ssid, ap_ssid, sizeof(_ssid) - 1);
        setState(WifiState::AP_MODE);
        Serial.printf("[WiFi] AP started: %s  IP: %s\n", ap_ssid, _ip);
    } else {
        Serial.printf("[WiFi] AP start failed\n");
    }
    return ok;
}

void WifiManager::stopAP() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    if (_state == WifiState::AP_MODE) {
        setState(WifiState::DISCONNECTED);
    }
    Serial.printf("[WiFi] AP stopped\n");
}

bool WifiManager::isAPMode() const { return _state == WifiState::AP_MODE; }

const char* WifiManager::getAPIP() const {
    if (_state == WifiState::AP_MODE) return _ip;
    return "0.0.0.0";
}

bool WifiManager::isConnected() const { return WiFi.status() == WL_CONNECTED; }
int32_t WifiManager::getRSSI() const { return WiFi.RSSI(); }

void WifiManager::loadNetworks() {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
        // Namespace doesn't exist yet - create it with a write-mode open
        prefs.begin(NVS_NAMESPACE, false);
        prefs.end();
        prefs.begin(NVS_NAMESPACE, true);
    }
    _savedCount = prefs.getInt(NVS_KEY_COUNT, 0);
    if (_savedCount > WIFI_MAX_SAVED_NETWORKS) _savedCount = WIFI_MAX_SAVED_NETWORKS;

    for (int i = 0; i < _savedCount; i++) {
        char keyS[4], keyP[4];
        snprintf(keyS, sizeof(keyS), "s%d", i);
        snprintf(keyP, sizeof(keyP), "p%d", i);
        String ssid = prefs.getString(keyS, "");
        String pass = prefs.getString(keyP, "");
        strncpy(_saved[i].ssid, ssid.c_str(), sizeof(_saved[i].ssid) - 1);
        _saved[i].ssid[sizeof(_saved[i].ssid) - 1] = '\0';
        strncpy(_saved[i].password, pass.c_str(), sizeof(_saved[i].password) - 1);
        _saved[i].password[sizeof(_saved[i].password) - 1] = '\0';
    }
    prefs.end();
    Serial.printf("[WiFi] Loaded %d saved networks from NVS\n", _savedCount);
}

void WifiManager::saveNetworks() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putInt(NVS_KEY_COUNT, _savedCount);

    for (int i = 0; i < WIFI_MAX_SAVED_NETWORKS; i++) {
        char keyS[4], keyP[4];
        snprintf(keyS, sizeof(keyS), "s%d", i);
        snprintf(keyP, sizeof(keyP), "p%d", i);
        if (i < _savedCount) {
            prefs.putString(keyS, _saved[i].ssid);
            prefs.putString(keyP, _saved[i].password);
        } else {
            prefs.remove(keyS);
            prefs.remove(keyP);
        }
    }
    prefs.end();
}

void WifiManager::monitorTask(void* param) {
    WifiManager* mgr = static_cast<WifiManager*>(param);
    while (mgr->_running) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));
        if (!mgr->_running) break;

        if (mgr->_state == WifiState::CONNECTED && WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Connection lost, attempting reconnect...");
#if WIFI_HAS_DIAG
            hal_diag::log(hal_diag::Severity::WARN, "wifi", "Monitor: connection lost, reconnecting");
#endif
            mgr->setState(WifiState::DISCONNECTED);
            mgr->autoConnect();
        }
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
        // Simulate auto-connect to first saved network
        connectToNetwork(_saved[0]);
    }
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
    setState(WifiState::DISCONNECTED);
}

bool WifiManager::connectToNetwork(const SavedNetwork& net) {
    setState(WifiState::CONNECTING);
    printf("[WiFi-SIM] Connecting to %s...\n", net.ssid);

    // Simulate connection delay
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    strncpy(_ssid, net.ssid, sizeof(_ssid) - 1);
    strncpy(_ip, "192.168.1.100", sizeof(_ip) - 1);
    setState(WifiState::CONNECTED);
    printf("[WiFi-SIM] Connected to %s, IP: %s\n", _ssid, _ip);
    return true;
}

bool WifiManager::autoConnect() {
    if (_savedCount == 0) return false;
    return connectToNetwork(_saved[0]);
}

bool WifiManager::startScan() {
    setState(WifiState::SCANNING);

    // Simulate scan results
    _scanResultCount = 3;
    strncpy(_scanResults[0].ssid, "SimNetwork-5G", sizeof(_scanResults[0].ssid));
    _scanResults[0].rssi = -45;
    _scanResults[0].auth = WifiAuth::WPA2_PSK;
    _scanResults[0].channel = 36;

    strncpy(_scanResults[1].ssid, "SimNetwork-2G", sizeof(_scanResults[1].ssid));
    _scanResults[1].rssi = -55;
    _scanResults[1].auth = WifiAuth::WPA2_PSK;
    _scanResults[1].channel = 6;

    strncpy(_scanResults[2].ssid, "OpenCafe", sizeof(_scanResults[2].ssid));
    _scanResults[2].rssi = -72;
    _scanResults[2].auth = WifiAuth::OPEN;
    _scanResults[2].channel = 1;

    setState(_ssid[0] ? WifiState::CONNECTED : WifiState::DISCONNECTED);
    return true;
}

bool WifiManager::startAP(const char* ssid, const char* password) {
    char ap_ssid[33] = "Tritium-SIM";
    if (ssid) strncpy(ap_ssid, ssid, sizeof(ap_ssid) - 1);
    strncpy(_ssid, ap_ssid, sizeof(_ssid) - 1);
    strncpy(_ip, "192.168.4.1", sizeof(_ip) - 1);
    setState(WifiState::AP_MODE);
    printf("[WiFi-SIM] AP started: %s  IP: %s\n", ap_ssid, _ip);
    return true;
}

void WifiManager::stopAP() {
    if (_state == WifiState::AP_MODE) setState(WifiState::DISCONNECTED);
    printf("[WiFi-SIM] AP stopped\n");
}

bool WifiManager::isAPMode() const { return _state == WifiState::AP_MODE; }
const char* WifiManager::getAPIP() const { return "192.168.4.1"; }

bool WifiManager::isConnected() const { return _state == WifiState::CONNECTED; }
int32_t WifiManager::getRSSI() const { return _state == WifiState::CONNECTED ? -50 : 0; }

void WifiManager::loadNetworks() {
    // Simulator: no persistence, start with empty list
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
        // No-op in simulator - connection is always stable
    }
}

#endif // SIMULATOR

// ============================================================================
// Shared (platform-independent) implementation
// ============================================================================

bool WifiManager::addNetwork(const char* ssid, const char* password) {
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) return false;
    if (password && strlen(password) > 64) return false;

    for (int i = 0; i < _savedCount; i++) {
        if (strcmp(_saved[i].ssid, ssid) == 0) {
            strncpy(_saved[i].password, password ? password : "", sizeof(_saved[i].password) - 1);
            _saved[i].password[sizeof(_saved[i].password) - 1] = '\0';
            saveNetworks();
            return true;
        }
    }

    if (_savedCount >= WIFI_MAX_SAVED_NETWORKS) return false;

    strncpy(_saved[_savedCount].ssid, ssid, sizeof(_saved[_savedCount].ssid) - 1);
    _saved[_savedCount].ssid[sizeof(_saved[_savedCount].ssid) - 1] = '\0';
    strncpy(_saved[_savedCount].password, password ? password : "", sizeof(_saved[_savedCount].password) - 1);
    _saved[_savedCount].password[sizeof(_saved[_savedCount].password) - 1] = '\0';
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

bool WifiManager::moveNetwork(int fromIdx, int toIdx) {
    if (fromIdx < 0 || fromIdx >= _savedCount) return false;
    if (toIdx < 0 || toIdx >= _savedCount) return false;
    if (fromIdx == toIdx) return true;

    SavedNetwork tmp = _saved[fromIdx];
    if (fromIdx < toIdx) {
        for (int i = fromIdx; i < toIdx; i++) _saved[i] = _saved[i + 1];
    } else {
        for (int i = fromIdx; i > toIdx; i--) _saved[i] = _saved[i - 1];
    }
    _saved[toIdx] = tmp;
    saveNetworks();
    return true;
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

WifiState WifiManager::getState() const { return _state; }
const char* WifiManager::getIP() const { return _ip; }
const char* WifiManager::getSSID() const { return _ssid; }

void WifiManager::onStateChange(WifiStateCallback cb) { _callback = cb; }

void WifiManager::setState(WifiState s) {
    if (_state != s) {
        _state = s;
        if (_callback) _callback(s);
    }
}
