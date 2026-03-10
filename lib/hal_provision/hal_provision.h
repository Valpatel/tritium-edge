#pragma once
// Secure device provisioning HAL.
// Handles certificates, device identity, and factory WiFi for connecting
// to secure services (Cloudflare tunnels, AI gateways, etc.).
//
// Storage layout:
//   LittleFS /prov/  - Permanent cert storage, device identity, server URLs
//   NVS              - User WiFi credentials (encrypted if flash encryption on)
//   SD card          - Temporary provisioning source (read once and forget)
//
// Usage:
//   #include "hal_provision.h"
//   ProvisionHAL prov;
//   prov.init();
//   if (!prov.isProvisioned()) {
//       prov.provisionFromSD();   // or prov.startUSBProvision();
//   }
//   // Access certs for WiFiClientSecure, MQTT, etc.
//   char ca[4096];
//   prov.getCACert(ca, sizeof(ca));

#include <cstdint>
#include <cstddef>

enum class ProvisionSource : uint8_t {
    SD_CARD,        // Read provisioning data from SD card
    USB_SERIAL,     // Receive provisioning data over USB serial
    NONE,
};

enum class ProvisionState : uint8_t {
    UNPROVISIONED,  // No certs/identity loaded
    PROVISIONED,    // Certs and identity present
    ERROR,
};

struct DeviceIdentity {
    char device_id[64];      // Unique device identifier
    char device_name[64];    // Human-friendly name
    char server_url[256];    // Server endpoint URL
    char mqtt_broker[128];   // MQTT broker address
    uint16_t mqtt_port;      // MQTT port
    bool provisioned;        // Has been provisioned
};

class ProvisionHAL {
public:
    bool init();  // Mount LittleFS, check provisioning state

    // Provisioning state
    ProvisionState getState() const;
    bool isProvisioned() const;
    const DeviceIdentity& getIdentity() const;

    // Provision from SD card
    // Looks for /provision/ directory on SD with:
    //   device.json       - device identity and server config
    //   ca.pem            - CA certificate
    //   client.crt        - Client certificate
    //   client.key        - Client private key
    //   factory_wifi.json - temporary WiFi for initial setup (optional)
    bool provisionFromSD(const char* sdPath = "/provision");

    // Provision from USB serial (JSON protocol)
    // Host sends: {"cmd":"provision","device_id":"...","ca_pem":"...","client_crt":"...","client_key":"..."}
    bool startUSBProvision();
    bool processUSBProvision();  // Call in loop() during USB provisioning
    bool isUSBProvisionActive() const;

    // Certificate access (for WiFiClientSecure, MQTT, etc.)
    bool getCACert(char* buf, size_t bufSize, size_t* outLen = nullptr);
    bool getClientCert(char* buf, size_t bufSize, size_t* outLen = nullptr);
    bool getClientKey(char* buf, size_t bufSize, size_t* outLen = nullptr);

    // Factory WiFi (temporary, used during provisioning only)
    bool hasFactoryWiFi() const;
    bool getFactoryWiFi(char* ssid, size_t ssidLen, char* pass, size_t passLen);
    bool clearFactoryWiFi();  // Remove after first successful connection

    // Manual cert management
    bool importCACert(const char* pem, size_t len);
    bool importClientCert(const char* pem, size_t len);
    bool importClientKey(const char* pem, size_t len);
    bool setDeviceIdentity(const DeviceIdentity& id);

    // Wipe all provisioning data (factory reset)
    bool factoryReset();

    // Test harness
    struct TestResult {
        bool init_ok;
        bool fs_ok;             // LittleFS accessible
        bool cert_write_ok;     // Can write test cert
        bool cert_read_ok;      // Can read it back
        bool cert_verify_ok;    // Data integrity
        bool identity_ok;       // Can save/load identity
        bool factory_wifi_ok;   // Factory WiFi save/load/clear
        bool factory_reset_ok;  // Factory reset works
        ProvisionState state;
        uint32_t test_duration_ms;
    };
    TestResult runTest();

private:
    static constexpr const char* PROV_DIR = "/prov";
    static constexpr const char* CA_CERT_PATH = "/prov/ca.pem";
    static constexpr const char* CLIENT_CERT_PATH = "/prov/client.crt";
    static constexpr const char* CLIENT_KEY_PATH = "/prov/client.key";
    static constexpr const char* IDENTITY_PATH = "/prov/device.json";
    static constexpr const char* FACTORY_WIFI_PATH = "/prov/factory_wifi.json";

    ProvisionState _state = ProvisionState::UNPROVISIONED;
    DeviceIdentity _identity = {};
    bool _usbActive = false;
    char* _usbBuf = nullptr;    // Lazy-allocated in PSRAM (4096)
    size_t _usbBufLen = 0;
    static constexpr size_t PROV_BUF_SIZE = 4096;

    bool _loadIdentity();
    bool _saveIdentity();
    bool _checkProvisioned();
    bool _copyFileFromSD(const char* sdPath, const char* fsPath);
    bool _parseDeviceJson(const char* json);
    bool _parseFactoryWifiJson(const char* json);
    bool _processUSBCommand(const char* json, size_t len);
};
