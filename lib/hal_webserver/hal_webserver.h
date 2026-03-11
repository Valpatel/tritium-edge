#pragma once
// ESP32 WebServer HAL — built-in HTTP server with dashboard, OTA, config editor,
// file manager, and REST API endpoints.
//
// Usage:
//   #include "hal_webserver.h"
//   WebServerHAL web;
//   web.init(80);
//   web.addDashboard();
//   web.addOtaPage();
//   web.addApiEndpoints();
//   web.startMDNS("esp32");
//   // in loop():
//   web.process();

#include <cstdint>
#include <functional>

// ESP-IDF HTTP server handle (opaque pointer when header not included)
#ifndef HTTPD_HANDLE_T_DEFINED
typedef void* httpd_handle_t;
#endif

using WebRequestHandler = std::function<void(void* server)>;

class WebServerHAL {
public:
    bool init(uint16_t port = 80);
    void stop();
    bool isRunning() const;

    // Must call in loop()
    void process();

    // Route registration
    void on(const char* uri, const char* method, WebRequestHandler handler);

    // Built-in pages (call after init)
    void addDashboard();        // System info dashboard at /
    void addOtaPage();          // Firmware upload at /update and /ota
    void addConfigEditor();     // JSON config editor at /config
    void addFileManager();      // LittleFS file browser at /files
    void addApiEndpoints();     // REST API at /api/*
    void addWiFiSetup();        // WiFi network configuration at /wifi
    void addBleViewer();        // Live BLE device list at /ble
    void addCommissionPage();   // QR code commissioning at /commission
    void addSystemPage();       // Detailed hardware info at /system
    void addLogsPage();         // Live serial log viewer at /logs
    void addMapPage();          // Leaflet.js slippy map at /map
    void addScreenshotPage();   // Live screen capture viewer at /screenshot
    void addRemotePage();       // Remote control (touch + screenshot) at /remote
    void addMeshPage();         // Mesh topology viewer at /mesh
    void addErrorPages();       // 404 and 500 error handlers
    void addAllPages();         // Register all built-in pages

    // Log ring buffer — call from debug_log output hook or Serial redirect
    static void captureLog(const char* line);
    static int getLogJson(char* buf, size_t size);

    // Data provider callbacks (set from main.cpp to inject sensor data)
    using BleJsonProvider = std::function<int(char* buf, size_t size)>;
    using DiagJsonProvider = std::function<int(char* buf, size_t size)>;
    using MeshJsonProvider = std::function<int(char* buf, size_t size)>;
    void setBleProvider(BleJsonProvider provider);
    void setDiagProvider(DiagJsonProvider provider);
    void setDiagHealthProvider(DiagJsonProvider provider);
    void setDiagEventsProvider(DiagJsonProvider provider);
    void setDiagAnomaliesProvider(DiagJsonProvider provider);
    void setMeshProvider(MeshJsonProvider provider);

    // Screen capture provider — returns RGB565 framebuffer pointer + dimensions
    using ScreenshotProvider = std::function<uint16_t*(int& width, int& height)>;
    void setScreenshotProvider(ScreenshotProvider provider);

    // GIS tile data providers (set from main.cpp to inject map tile data)
    using GisTileProvider = std::function<uint8_t*(const char* layer, uint8_t z, uint32_t x, uint32_t y, size_t& outLen)>;
    using GisLayerProvider = std::function<int(char* buf, size_t size)>;
    void setGisTileProvider(GisTileProvider provider);
    void setGisLayerProvider(GisLayerProvider provider);

    // Helpers for handlers
    void sendResponse(int code, const char* contentType, const char* body);
    void sendJson(int code, const char* json);

    // Captive portal (for AP commissioning mode)
    void startCaptivePortal();
    void stopCaptivePortal();

    // mDNS
    bool startMDNS(const char* hostname = "esp32");

    // Info
    uint16_t getPort() const;
    uint32_t getRequestCount() const;
    const char* getIP() const;

    // Test harness
    struct TestResult {
        bool init_ok;
        bool mdns_ok;
        bool dashboard_ok;
        bool api_ok;
        uint16_t port;
        const char* ip;
        uint32_t test_duration_ms;
    };
    TestResult runTest();

    // Internal state — accessed by static ESP-IDF HTTP handler functions
    bool _running = false;
    uint16_t _port = 80;
    uint32_t _requestCount = 0;
    uint32_t _lastRequestMs = 0;  // millis() of last successful request
    char _ip[16] = {0};
    BleJsonProvider _bleProvider = nullptr;
    DiagJsonProvider _diagProvider = nullptr;
    DiagJsonProvider _diagHealthProvider = nullptr;
    DiagJsonProvider _diagEventsProvider = nullptr;
    DiagJsonProvider _diagAnomaliesProvider = nullptr;
    MeshJsonProvider _meshProvider = nullptr;
    ScreenshotProvider _screenshotProvider = nullptr;
    GisTileProvider _gisTileProvider = nullptr;
    GisLayerProvider _gisLayerProvider = nullptr;
};
