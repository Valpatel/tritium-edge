#pragma once
#include <cstdint>
#include <cstring>
#include <functional>

static constexpr int WIFI_MAX_SAVED_NETWORKS = 10;
static constexpr int WIFI_MAX_SCAN_RESULTS = 20;

enum class WifiState : uint8_t {
    IDLE,
    SCANNING,
    CONNECTING,
    CONNECTED,
    DISCONNECTED,
    FAILED,
    AP_MODE
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

struct SavedNetwork {
    char ssid[33];
    char password[65];
};

struct ScanResult {
    char ssid[33];
    int32_t rssi;
    WifiAuth auth;
    uint8_t channel;
};

using WifiStateCallback = std::function<void(WifiState state)>;

class WifiManager {
public:
    WifiManager();
    ~WifiManager();

    void init();
    void shutdown();

    // Network management (persisted to NVS on ESP32, file on desktop)
    bool addNetwork(const char* ssid, const char* password);
    bool removeNetwork(const char* ssid);
    int getSavedNetworks(SavedNetwork* out, int maxCount) const;
    int getSavedCount() const;

    // Connection control
    bool connect();
    bool connect(const char* ssid);
    void disconnect();

    // AP mode for commissioning (phone connects to ESP32 directly)
    bool startAP(const char* ssid = nullptr, const char* password = nullptr);
    void stopAP();
    bool isAPMode() const;
    const char* getAPIP() const;

    // Scanning
    bool startScan();
    int getScanResults(ScanResult* out, int maxCount) const;

    // Status
    WifiState getState() const;
    bool isConnected() const;
    const char* getIP() const;
    const char* getSSID() const;
    int32_t getRSSI() const;

    void onStateChange(WifiStateCallback cb);
    void setState(WifiState s);

    static WifiManager* _instance;

private:
    void loadNetworks();
    void saveNetworks();
    bool connectToNetwork(const SavedNetwork& net);
    bool autoConnect();

    SavedNetwork _saved[WIFI_MAX_SAVED_NETWORKS];
    int _savedCount = 0;

    ScanResult _scanResults[WIFI_MAX_SCAN_RESULTS];
    int _scanResultCount = 0;

    char _ip[16] = {};
    char _ssid[33] = {};

    volatile WifiState _state = WifiState::IDLE;
    WifiStateCallback _callback = nullptr;
    volatile bool _running = false;

    void* _monitorTaskHandle = nullptr;

    static void monitorTask(void* param);
};
