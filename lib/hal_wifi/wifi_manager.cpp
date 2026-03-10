/// @file wifi_manager.cpp
/// @brief Enhanced WiFi management — ESP32 (pure ESP-IDF) + simulator implementations.
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
// Platform: ESP32 (pure ESP-IDF — no Arduino WiFi.h)
// ============================================================================
#ifndef SIMULATOR

#include "tritium_compat.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/inet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static constexpr const char* NVS_NAMESPACE   = "trit_wifi";
static constexpr const char* NVS_KEY_COUNT   = "count";
static constexpr uint32_t MONITOR_STACK_SIZE = 4096;

/// ESP-IDF netif handles for STA and AP interfaces.
static esp_netif_t* s_sta_netif = nullptr;
static esp_netif_t* s_ap_netif  = nullptr;

/// Track whether the global event loop and netif have been initialized
/// (they may be initialized by another component).
static bool s_netif_initialized = false;
static bool s_event_loop_initialized = false;
static bool s_wifi_started = false;

// ── Captive portal DNS task ──────────────────────────────────────────────────
// Minimal DNS server: respond to every query with the AP IP address.

static volatile bool s_dns_running = false;
static TaskHandle_t  s_dns_task_handle = nullptr;

static void captiveDnsTask(void* param) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        s_dns_running = false;
        vTaskDelete(nullptr);
        return;
    }

    struct sockaddr_in listen_addr = {};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port   = htons(53);
    listen_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
        close(sock);
        s_dns_running = false;
        vTaskDelete(nullptr);
        return;
    }

    // Set receive timeout so we can check the running flag periodically
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Get AP IP to respond with
    esp_netif_ip_info_t ip_info;
    uint32_t ap_ip = 0;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        ap_ip = ip_info.ip.addr;
    }

    while (s_dns_running) {
        uint8_t buf[512];
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr*)&client_addr, &addr_len);
        if (len < 12) continue;  // Too short for DNS header

        // Build response: copy the query and set response flags
        // DNS header: ID(2) FLAGS(2) QDCOUNT(2) ANCOUNT(2) NSCOUNT(2) ARCOUNT(2)
        buf[2] = 0x81;  // QR=1, Opcode=0, AA=1
        buf[3] = 0x80;  // RA=1, RCODE=0 (no error)
        // ANCOUNT = QDCOUNT (answer for each question)
        buf[6] = buf[4];
        buf[7] = buf[5];

        // Find end of question section
        int pos = 12;
        while (pos < len && buf[pos] != 0) {
            pos += buf[pos] + 1;
        }
        pos += 5;  // Skip null terminator + QTYPE(2) + QCLASS(2)

        if (pos + 16 <= (int)sizeof(buf)) {
            // Append answer: pointer to name, type A, class IN, TTL 60, rdlength 4, IP
            buf[pos++] = 0xC0;  // Pointer to name in question
            buf[pos++] = 0x0C;
            buf[pos++] = 0x00; buf[pos++] = 0x01;  // Type A
            buf[pos++] = 0x00; buf[pos++] = 0x01;  // Class IN
            buf[pos++] = 0x00; buf[pos++] = 0x00;
            buf[pos++] = 0x00; buf[pos++] = 0x3C;  // TTL = 60
            buf[pos++] = 0x00; buf[pos++] = 0x04;  // RDLENGTH = 4
            memcpy(&buf[pos], &ap_ip, 4);
            pos += 4;

            sendto(sock, buf, pos, 0,
                   (struct sockaddr*)&client_addr, addr_len);
        }
    }

    close(sock);
    vTaskDelete(nullptr);
}

static void startCaptiveDns() {
    if (s_dns_running) return;
    s_dns_running = true;
    xTaskCreatePinnedToCore(captiveDnsTask, "dns_cap", 3072, nullptr, 2,
                            &s_dns_task_handle, 0);
}

static void stopCaptiveDns() {
    if (!s_dns_running) return;
    s_dns_running = false;
    // Task will self-delete after the recv timeout expires
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_dns_task_handle = nullptr;
}

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

// ── IP string helper ─────────────────────────────────────────────────────────

static void ipToStr(esp_ip4_addr_t ip, char* buf, size_t len) {
    snprintf(buf, len, IPSTR, IP2STR(&ip));
}

// ── ESP-IDF WiFi + IP event handler ─────────────────────────────────────────

static void wifiEventHandler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    WifiManager* mgr = WifiManager::_instance;
    if (!mgr) return;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_DISCONNECTED:
                mgr->notifyDisconnected();
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                mgr->notifyAPClientEvent(true);
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                mgr->notifyAPClientEvent(false);
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            mgr->notifyGotIP();
        }
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

// ── Event notification methods ───────────────────────────────────────────────

void WifiManager::notifyGotIP() {
    _staConnected = true;
    setState(WifiState::CONNECTED);
}

void WifiManager::notifyDisconnected() {
    _staConnected = false;
    if (_state == WifiState::CONNECTED || _state == WifiState::AP_AND_STA) {
        setState(WifiState::DISCONNECTED);
    }
}

void WifiManager::notifyAPClientEvent(bool connected) {
    Serial.printf("[WiFi] AP: client %s\n", connected ? "connected" : "disconnected");
}

// ── init() ───────────────────────────────────────────────────────────────────

void WifiManager::init() {
    // Initialize NVS (may already be done elsewhere)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Initialize TCP/IP stack (idempotent in ESP-IDF >=5.x)
    if (!s_netif_initialized) {
        esp_netif_init();
        s_netif_initialized = true;
    }

    // Create default event loop (idempotent — returns ESP_ERR_INVALID_STATE if exists)
    if (!s_event_loop_initialized) {
        ret = esp_event_loop_create_default();
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
            s_event_loop_initialized = true;
        }
    }

    // Create default STA netif if not already created
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    // Initialize WiFi with default config
    if (!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        s_wifi_started = true;
    }

    // Register event handlers
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, nullptr);

    // Check if already connected (e.g., build-flag connect ran before init)
    bool already_connected = _staConnected;
    if (!already_connected) {
        // Check current state from driver
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            already_connected = true;
            _staConnected = true;
        }
    }

    if (!already_connected) {
        esp_wifi_set_mode(WIFI_MODE_STA);
    }

    // Disable auto-reconnect — we handle reconnection ourselves
    esp_wifi_set_config(WIFI_IF_STA, nullptr);  // Ensure config is clean

    esp_wifi_start();

    loadNetworks();

    if (already_connected) {
        // Read current SSID from driver
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            strncpy(_ssid, (const char*)ap_info.ssid, sizeof(_ssid) - 1);
        }
        // Read current IP
        esp_netif_ip_info_t ip_info;
        if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            ipToStr(ip_info.ip, _ip, sizeof(_ip));
        }
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
    // Captive portal DNS runs in its own task — nothing to do here.
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

    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler);
}

// ── disconnect() ─────────────────────────────────────────────────────────────

void WifiManager::disconnect() {
    esp_wifi_disconnect();
    _staConnected = false;
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

    // Disconnect any current connection first
    esp_wifi_disconnect();
    _staConnected = false;

    // Ensure we are in STA (or AP+STA) mode
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL || mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(_apActive ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    }

    // Configure STA
    wifi_config_t wifi_cfg = {};
    strncpy((char*)wifi_cfg.sta.ssid, net.ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (net.password[0]) {
        strncpy((char*)wifi_cfg.sta.password, net.password, sizeof(wifi_cfg.sta.password) - 1);
    }
    wifi_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();

    // Wait for connection (event-driven flag)
    uint32_t start = millis();
    while (!_staConnected && (millis() - start) < CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (_staConnected) {
        strncpy(_ssid, net.ssid, sizeof(_ssid) - 1);

        // Get IP from netif
        esp_netif_ip_info_t ip_info;
        if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            ipToStr(ip_info.ip, _ip, sizeof(_ip));
        }

        _status.connected_since = millis();
        _reconnectAttempts = 0;
        _wasConnected = true;

        // Get RSSI from driver
        wifi_ap_record_t ap_info;
        int8_t rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }

        // Update saved network stats
        for (int i = 0; i < _savedCount; i++) {
            if (strcmp(_saved[i].ssid, net.ssid) == 0) {
                _saved[i].rssi_last = rssi;
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

    esp_wifi_disconnect();

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

    Serial.printf("[WiFi] Scanning for known networks...\n");

    // Start a blocking scan
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // blocking
    if (err != ESP_OK) {
        Serial.printf("[WiFi] Scan failed: %s\n", esp_err_to_name(err));
        return false;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        Serial.printf("[WiFi] No networks found\n");
        return false;
    }

    // Get scan results
    uint16_t max_records = (found > 32) ? 32 : found;
    wifi_ap_record_t* records = new wifi_ap_record_t[max_records];
    esp_wifi_scan_get_ap_records(&max_records, records);

    // Build candidate list: saved networks visible in scan, sorted by priority
    struct Candidate { int savedIdx; int32_t rssi; int8_t priority; };
    Candidate candidates[WIFI_MAX_SAVED_NETWORKS];
    int candidateCount = 0;

    for (uint16_t s = 0; s < max_records && candidateCount < WIFI_MAX_SAVED_NETWORKS; s++) {
        const char* scannedSSID = (const char*)records[s].ssid;
        for (int i = 0; i < _savedCount; i++) {
            if (!_saved[i].enabled) continue;
            if (strcmp(scannedSSID, _saved[i].ssid) == 0) {
                _saved[i].rssi_last = records[s].rssi;
                candidates[candidateCount++] = {i, (int32_t)records[s].rssi, _saved[i].priority};
                break;
            }
        }
    }

    delete[] records;

    if (candidateCount == 0) {
        Serial.printf("[WiFi] No known networks in range\n");
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
    Serial.printf("[WiFi] Failover: trying next network...\n");

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

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 100;
    scan_cfg.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // blocking
    if (err != ESP_OK) {
        _scanResultCount = 0;
        setState(prev);
        return false;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);

    uint16_t max_records = (found > (uint16_t)WIFI_MAX_SCAN_RESULTS)
                            ? (uint16_t)WIFI_MAX_SCAN_RESULTS : found;
    wifi_ap_record_t records[WIFI_MAX_SCAN_RESULTS];
    esp_wifi_scan_get_ap_records(&max_records, records);

    _scanResultCount = max_records;
    for (int i = 0; i < _scanResultCount; i++) {
        strncpy(_scanResults[i].ssid, (const char*)records[i].ssid,
                sizeof(_scanResults[i].ssid) - 1);
        _scanResults[i].ssid[sizeof(_scanResults[i].ssid) - 1] = '\0';
        _scanResults[i].rssi = records[i].rssi;
        _scanResults[i].auth = mapAuthMode(records[i].authmode);
        _scanResults[i].channel = records[i].primary;

        // Mark known networks
        _scanResults[i].known = false;
        for (int j = 0; j < _savedCount; j++) {
            if (strcmp(_scanResults[i].ssid, _saved[j].ssid) == 0) {
                _scanResults[i].known = true;
                _saved[j].rssi_last = (int8_t)records[i].rssi;
                break;
            }
        }
    }

    setState(_staConnected ? WifiState::CONNECTED : WifiState::DISCONNECTED);
    return true;
}

// ── AP mode ──────────────────────────────────────────────────────────────────

bool WifiManager::startAP(const char* ssid, const char* password) {
    // Generate default SSID from MAC if not provided
    if (!ssid || strlen(ssid) == 0) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
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

    // Create AP netif if not already created
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    // Switch to AP+STA mode so we can maintain STA connection while hosting AP
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    // Configure AP
    wifi_config_t ap_cfg = {};
    strncpy((char*)ap_cfg.ap.ssid, _apSSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(_apSSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;

    if (_apPassword[0]) {
        strncpy((char*)ap_cfg.ap.password, _apPassword, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        Serial.printf("[WiFi] Failed to configure AP: %s\n", esp_err_to_name(err));
        return false;
    }

    // Start captive portal DNS
    startCaptiveDns();

    _apActive = true;
    strncpy(_status.ap_ssid, _apSSID, sizeof(_status.ap_ssid) - 1);

    // Get AP IP
    esp_netif_ip_info_t ip_info;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        ipToStr(ip_info.ip, _status.ap_ip, sizeof(_status.ap_ip));
    } else {
        strncpy(_status.ap_ip, "192.168.4.1", sizeof(_status.ap_ip) - 1);
    }

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

    stopCaptiveDns();

    // Deconfigure AP: set to empty config then switch back to STA mode
    wifi_config_t ap_cfg = {};
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    // Switch back to STA-only if still connected
    if (_staConnected) {
        esp_wifi_set_mode(WIFI_MODE_STA);
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
    Serial.printf("[WiFi] AP stopped\n");
    return true;
}

bool WifiManager::isAPActive() const { return _apActive; }

// ── Status helpers ───────────────────────────────────────────────────────────

bool WifiManager::isConnected() const { return _staConnected; }

int32_t WifiManager::getRSSI() const {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

void WifiManager::updateStatus() {
    _status.connected = _staConnected;
    _status.ap_active = _apActive;

    if (_status.connected) {
        strncpy(_status.ssid, _ssid, sizeof(_status.ssid) - 1);
        strncpy(_status.ip, _ip, sizeof(_status.ip) - 1);

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            _status.rssi = ap_info.rssi;
            _status.channel = ap_info.primary;
        }
    } else {
        _status.ssid[0] = '\0';
        _status.ip[0] = '\0';
        _status.rssi = 0;
        _status.channel = 0;
    }

    // Get MAC address
    esp_efuse_mac_get_default(_status.mac);

    if (_apActive) {
        wifi_sta_list_t sta_list;
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
            _status.clients_connected = sta_list.num;
        }
        strncpy(_status.ap_ssid, _apSSID, sizeof(_status.ap_ssid) - 1);
        if (s_ap_netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
                ipToStr(ip_info.ip, _status.ap_ip, sizeof(_status.ap_ip));
            }
        }
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
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // Namespace doesn't exist yet — create it
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            _savedCount = 0;
            Serial.printf("[WiFi] NVS open failed: %s\n", esp_err_to_name(err));
            return;
        }
        nvs_commit(handle);
        nvs_close(handle);
        // Re-open read-only
        err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
        if (err != ESP_OK) {
            _savedCount = 0;
            return;
        }
    }

    int32_t count = 0;
    err = nvs_get_i32(handle, NVS_KEY_COUNT, &count);
    if (err != ESP_OK) count = 0;
    _savedCount = (count > WIFI_MAX_SAVED_NETWORKS) ? WIFI_MAX_SAVED_NETWORKS : (int)count;

    for (int i = 0; i < _savedCount; i++) {
        char keyS[8], keyP[8], keyPr[8], keyE[8];
        snprintf(keyS,  sizeof(keyS),  "s%d", i);
        snprintf(keyP,  sizeof(keyP),  "p%d", i);
        snprintf(keyPr, sizeof(keyPr), "pr%d", i);
        snprintf(keyE,  sizeof(keyE),  "e%d", i);

        // Read SSID
        size_t ssid_len = sizeof(_saved[i].ssid);
        err = nvs_get_str(handle, keyS, _saved[i].ssid, &ssid_len);
        if (err != ESP_OK) _saved[i].ssid[0] = '\0';

        // Read password
        size_t pass_len = sizeof(_saved[i].password);
        err = nvs_get_str(handle, keyP, _saved[i].password, &pass_len);
        if (err != ESP_OK) _saved[i].password[0] = '\0';

        // Read priority
        int8_t pri = (int8_t)i;
        err = nvs_get_i8(handle, keyPr, &pri);
        _saved[i].priority = pri;

        // Read enabled flag
        uint8_t enabled = 1;
        err = nvs_get_u8(handle, keyE, &enabled);
        _saved[i].enabled = (enabled != 0);

        _saved[i].rssi_last      = 0;
        _saved[i].last_connected = 0;
        _saved[i].fail_count     = 0;
    }

    nvs_close(handle);
    Serial.printf("[WiFi] Loaded %d saved networks from NVS\n", _savedCount);
}

void WifiManager::saveNetworks() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        Serial.printf("[WiFi] NVS open for write failed: %s\n", esp_err_to_name(err));
        return;
    }

    nvs_set_i32(handle, NVS_KEY_COUNT, _savedCount);

    for (int i = 0; i < WIFI_MAX_SAVED_NETWORKS; i++) {
        char keyS[8], keyP[8], keyPr[8], keyE[8];
        snprintf(keyS,  sizeof(keyS),  "s%d", i);
        snprintf(keyP,  sizeof(keyP),  "p%d", i);
        snprintf(keyPr, sizeof(keyPr), "pr%d", i);
        snprintf(keyE,  sizeof(keyE),  "e%d", i);

        if (i < _savedCount) {
            nvs_set_str(handle, keyS, _saved[i].ssid);
            nvs_set_str(handle, keyP, _saved[i].password);
            nvs_set_i8(handle, keyPr, _saved[i].priority);
            nvs_set_u8(handle, keyE, _saved[i].enabled ? 1 : 0);
        } else {
            nvs_erase_key(handle, keyS);
            nvs_erase_key(handle, keyP);
            nvs_erase_key(handle, keyPr);
            nvs_erase_key(handle, keyE);
        }
    }

    nvs_commit(handle);
    nvs_close(handle);
}

// ── Monitor task ─────────────────────────────────────────────────────────────

void WifiManager::monitorTask(void* param) {
    WifiManager* mgr = static_cast<WifiManager*>(param);
    while (mgr->_running) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));
        if (!mgr->_running) break;

        // Detect disconnect and attempt failover
        if (mgr->_wasConnected && !mgr->_staConnected) {
            Serial.printf("[WiFi] Connection lost, attempting failover...\n");
            mgr->_wasConnected = false;
            mgr->setState(WifiState::RECONNECTING);
            mgr->fireDisconnectCallbacks();
            mgr->tryNextNetwork();
        }

        // Background scan to update RSSI of saved networks
        uint32_t now = millis();
        if (mgr->_autoFailover &&
            (now - mgr->_lastBackgroundScan) >= mgr->BACKGROUND_SCAN_MS &&
            mgr->_staConnected)
        {
            mgr->_lastBackgroundScan = now;

            wifi_scan_config_t scan_cfg = {};
            scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            scan_cfg.scan_time.active.min = 50;
            scan_cfg.scan_time.active.max = 200;

            if (esp_wifi_scan_start(&scan_cfg, true) == ESP_OK) {
                uint16_t found = 0;
                esp_wifi_scan_get_ap_num(&found);
                if (found > 0) {
                    uint16_t max_records = (found > 32) ? 32 : found;
                    wifi_ap_record_t* records = new wifi_ap_record_t[max_records];
                    esp_wifi_scan_get_ap_records(&max_records, records);

                    for (uint16_t s = 0; s < max_records; s++) {
                        const char* scannedSSID = (const char*)records[s].ssid;
                        for (int i = 0; i < mgr->_savedCount; i++) {
                            if (strcmp(scannedSSID, mgr->_saved[i].ssid) == 0) {
                                mgr->_saved[i].rssi_last = records[s].rssi;
                                break;
                            }
                        }
                    }

                    delete[] records;
                }
            }
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
    _staConnected = false;
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
    _staConnected = true;
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

// Stub event notification methods for simulator
void WifiManager::notifyGotIP() { _staConnected = true; }
void WifiManager::notifyDisconnected() { _staConnected = false; }
void WifiManager::notifyAPClientEvent(bool) {}

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
