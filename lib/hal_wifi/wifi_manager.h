/// @file wifi_manager.h
/// @brief Enhanced WiFi management — multi-network, AP mode, captive portal, failover.
/// @copyright 2026 Valpatel Software LLC
/// @license AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>

static constexpr int WIFI_MAX_SAVED_NETWORKS = 10;
static constexpr int WIFI_MAX_SCAN_RESULTS   = 20;
static constexpr int WIFI_MAX_CALLBACKS       = 4;

// ── Enumerations ─────────────────────────────────────────────────────────────

enum class WifiState : uint8_t {
    IDLE,
    SCANNING,
    CONNECTING,
    CONNECTED,
    DISCONNECTED,
    RECONNECTING,
    SCANNING_FALLBACK,
    AP_ONLY,
    AP_AND_STA,
    FAILED
};

enum class WifiAuth : uint8_t {
    OPEN,
    WEP,
    WPA_PSK,
    WPA2_PSK,
    WPA_WPA2_PSK,
    WPA3_PSK,
    UNKNOWN
};

// ── Data structures ──────────────────────────────────────────────────────────

/// Persisted network entry with priority and health tracking.
struct SavedNetwork {
    char     ssid[33];
    char     password[65];
    int8_t   priority;         ///< 0 = highest, -1 = auto (append)
    int8_t   rssi_last;        ///< Last known RSSI (updated on scan/connect)
    uint32_t last_connected;   ///< millis() timestamp of last successful connect
    uint8_t  fail_count;       ///< Consecutive connection failures
    bool     enabled;          ///< false = skip during auto-connect
};

/// Real-time WiFi status snapshot.
struct WifiStatus {
    bool     connected;
    bool     ap_active;
    char     ssid[33];
    char     ip[16];
    char     ap_ip[16];
    char     ap_ssid[33];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  mac[6];
    uint32_t connected_since;   ///< millis() when STA connected
    uint8_t  clients_connected; ///< AP mode client count
};

/// Single scan result entry.
struct ScanResult {
    char     ssid[33];
    int32_t  rssi;
    WifiAuth auth;
    uint8_t  channel;
    bool     known;   ///< true if SSID matches a saved network
};

// ── Callback types ───────────────────────────────────────────────────────────

using WifiStateCallback  = std::function<void(WifiState state)>;
using WifiStatusCallback = std::function<void(const WifiStatus& status, void* user_data)>;

// ── WifiManager ──────────────────────────────────────────────────────────────

/// Full WiFi lifecycle manager: multi-network NVS persistence, AP mode with
/// captive portal, priority-based failover, exponential backoff reconnect,
/// background RSSI tracking, and event callbacks.
class WifiManager {
public:
    WifiManager();
    ~WifiManager();

    /// Initialise WiFi hardware, load saved networks, start monitor task.
    void init();

    /// Call from main loop to service captive portal DNS + periodic work.
    void tick();

    /// Tear down WiFi — stop tasks, disconnect, remove event handlers.
    void shutdown();

    // ── Station mode ─────────────────────────────────────────────────────

    /// Connect to the best available saved network (scan + priority sort).
    bool connect();

    /// Connect to a specific saved network by SSID.
    bool connect(const char* ssid);

    /// Connect to an arbitrary network (optionally saving it).
    bool connectTo(const char* ssid, const char* password, bool save = true);

    /// Disconnect STA.
    void disconnect();

    /// True if STA is associated and has an IP.
    bool isConnected() const;

    // ── Saved networks (NVS-backed) ──────────────────────────────────────

    /// Copy up to @p maxCount saved networks into @p out. Returns count.
    int  getSavedNetworks(SavedNetwork* out, int maxCount) const;

    /// Add or update a saved network. priority -1 = append at end.
    bool addNetwork(const char* ssid, const char* password, int8_t priority = -1);

    /// Remove a saved network by SSID.
    bool removeNetwork(const char* ssid);

    /// Change priority of a saved network (0 = highest).
    bool reorderNetwork(const char* ssid, int8_t new_priority);

    /// Number of saved networks.
    int  getSavedCount() const;

    // ── AP mode ──────────────────────────────────────────────────────────

    /// Start soft-AP. If ssid is null, defaults to "Tritium-XXXX" (last 4 MAC).
    bool startAP(const char* ssid = nullptr, const char* password = nullptr);

    /// Stop soft-AP and captive portal DNS.
    bool stopAP();

    /// True if soft-AP is active.
    bool isAPActive() const;

    // ── Scanning ─────────────────────────────────────────────────────────

    /// Trigger a synchronous scan. Returns true on success.
    bool startScan();

    /// Copy up to @p maxCount scan results into @p out. Returns count.
    int  getScanResults(ScanResult* out, int maxCount) const;

    // ── Status ───────────────────────────────────────────────────────────

    /// Get current status snapshot.
    WifiStatus getStatus() const;

    /// Individual accessors (backwards-compat).
    WifiState   getState() const;
    const char* getIP() const;
    const char* getSSID() const;
    int32_t     getRSSI() const;

    // ── Auto-failover ────────────────────────────────────────────────────

    /// Enable/disable automatic failover to next network on disconnect.
    void enableAutoFailover(bool enable);
    bool isAutoFailoverEnabled() const;

    // ── Callbacks ────────────────────────────────────────────────────────

    void onStateChange(WifiStateCallback cb);
    void onConnect(WifiStatusCallback cb, void* user_data = nullptr);
    void onDisconnect(WifiStatusCallback cb, void* user_data = nullptr);

    /// Set state and fire callbacks (public for event handler access).
    void setState(WifiState s);

    /// Singleton-ish pointer for WiFi event handler.
    static WifiManager* _instance;

private:
    // ── Internal helpers ─────────────────────────────────────────────────

    void loadNetworks();
    void saveNetworks();
    bool connectToNetwork(const SavedNetwork& net);
    bool autoConnect();
    void tryNextNetwork();
    void updateStatus();
    void fireConnectCallbacks();
    void fireDisconnectCallbacks();

    // ── Saved networks ───────────────────────────────────────────────────

    SavedNetwork _saved[WIFI_MAX_SAVED_NETWORKS];
    int          _savedCount = 0;

    // ── Scan results ─────────────────────────────────────────────────────

    ScanResult _scanResults[WIFI_MAX_SCAN_RESULTS];
    int        _scanResultCount = 0;

    // ── Status ───────────────────────────────────────────────────────────

    WifiStatus      _status = {};
    char            _ip[16]   = {};
    char            _ssid[33] = {};
    volatile WifiState _state = WifiState::IDLE;

    // ── AP mode ──────────────────────────────────────────────────────────

    bool _apActive = false;
    char _apSSID[33] = {};
    char _apPassword[65] = {};

    // ── Reconnect / failover state machine ───────────────────────────────

    bool     _autoFailover     = true;
    uint8_t  _reconnectAttempts = 0;
    uint32_t _reconnectTimer   = 0;
    uint32_t _lastBackgroundScan = 0;
    int      _currentNetworkIdx = -1;
    bool     _wasConnected     = false;

    static constexpr uint32_t RECONNECT_BASE_MS   = 1000;
    static constexpr uint32_t RECONNECT_MAX_MS    = 30000;
    static constexpr uint32_t BACKGROUND_SCAN_MS  = 60000;

    // ── Callbacks ────────────────────────────────────────────────────────

    WifiStateCallback _stateCallback = nullptr;

    struct StatusCBEntry {
        WifiStatusCallback cb;
        void*              user_data = nullptr;
    };
    StatusCBEntry _connectCBs[WIFI_MAX_CALLBACKS]    = {};
    StatusCBEntry _disconnectCBs[WIFI_MAX_CALLBACKS] = {};
    int           _connectCBCount    = 0;
    int           _disconnectCBCount = 0;

    // ── Monitor task ─────────────────────────────────────────────────────

    volatile bool _running = false;
    void*         _monitorTaskHandle = nullptr;
    static void   monitorTask(void* param);
};
