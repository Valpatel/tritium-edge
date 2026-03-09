#pragma once
// WebServer service adapter — wraps WebServerHAL as a ServiceInterface.
// Priority 200: must init LAST — aggregates all providers from other services.

#include "service.h"
#include "service_registry.h"

#if defined(ENABLE_WEBSERVER) && __has_include("hal_webserver.h")
#include "hal_webserver.h"
#include <WiFi.h>
#include <LittleFS.h>
#endif

// Forward-declare service types for auto-wiring
#if defined(ENABLE_BLE_SCANNER) && __has_include("ble_scanner_service.h")
#include "ble_scanner_service.h"
#endif

#if defined(ENABLE_DIAG) && __has_include("diag_service.h")
#include "diag_service.h"
#endif

#if defined(ENABLE_ESPNOW) && __has_include("espnow_service.h")
#include "espnow_service.h"
#endif

// GIS offline map tiles from SD card
#if defined(HAS_SDCARD) && HAS_SDCARD && __has_include("hal_gis.h")
#include "hal_gis.h"
#endif

// WiFi service for connection state
#if defined(ENABLE_WIFI) && __has_include("wifi_service.h")
#include "wifi_service.h"
#endif

// Forward declare App for screenshot wiring
#include "app.h"

class WebServerService : public ServiceInterface {
public:
    const char* name() const override { return "webserver"; }
    uint8_t capabilities() const override { return SVC_TICK; }
    int initPriority() const override { return 200; }

    // Wire screen capture from any App that implements getFramebuffer()
    void setScreenshotApp(App* app) {
#if defined(ENABLE_WEBSERVER)
        if (app && _active) {
            _webserver.setScreenshotProvider([app](int& w, int& h) -> uint16_t* {
                return app->getFramebuffer(w, h);
            });
        }
#else
        (void)app;
#endif
    }

    // Wire a custom screenshot provider (e.g. LVGL framebuffer for OS shell)
    void setScreenshotProvider(WebServerHAL::ScreenshotProvider provider) {
#if defined(ENABLE_WEBSERVER)
        if (_active) {
            _webserver.setScreenshotProvider(provider);
        }
#else
        (void)provider;
#endif
    }

    bool init() override {
#if defined(ENABLE_WEBSERVER)
        // Check WiFi state via registry
        auto* wifi_svc = ServiceRegistry::getAs<WifiService>("wifi");
        if (!wifi_svc) return false;
        if (!wifi_svc->isConnected() && !wifi_svc->isAPMode()) return false;

        LittleFS.begin(true);  // Format on first mount

        uint16_t web_port = 80;
        if (_webserver.init(web_port)) {
            _webserver.addAllPages();

            // Wire BLE data into web server if BLE scanner service is registered
#if defined(ENABLE_BLE_SCANNER)
            {
                auto* ble = ServiceRegistry::getAs<BleScannerService>("ble_scanner");
                if (ble) {
                    _webserver.setBleProvider([ble](char* buf, size_t size) -> int {
                        return ble->toJson(buf, size);
                    });
                }
            }
#endif

            // Wire diagnostics data into web server
#if defined(ENABLE_DIAG)
            _webserver.setDiagProvider([](char* buf, size_t size) -> int {
                return hal_diag::full_report_json(buf, size);
            });
            _webserver.setDiagHealthProvider([](char* buf, size_t size) -> int {
                return hal_diag::health_to_json(buf, size);
            });
            _webserver.setDiagEventsProvider([](char* buf, size_t size) -> int {
                return hal_diag::events_to_json(buf, size, 50);
            });
            _webserver.setDiagAnomaliesProvider([](char* buf, size_t size) -> int {
                return hal_diag::anomalies_to_json(buf, size);
            });
#endif

            // Wire GIS tile data into web server (boards with SD card)
#if defined(HAS_SDCARD) && HAS_SDCARD && __has_include("hal_gis.h")
            {
                static GisHAL _gis;
                GisConfig gis_cfg;
                gis_cfg.sd_mount_point = "/sdcard";
                gis_cfg.tile_base_path = "/gis";
                gis_cfg.max_cache_tiles = 16;
                if (_gis.init(gis_cfg)) {
                    _webserver.setGisTileProvider([](const char* layer, uint8_t z, uint32_t x, uint32_t y, size_t& outLen) -> uint8_t* {
                        return _gis.getTile(layer, z, x, y, outLen);
                    });
                    _webserver.setGisLayerProvider([](char* buf, size_t size) -> int {
                        int n = _gis.getLayerCount();
                        int pos = snprintf(buf, size, "[");
                        for (int i = 0; i < n && pos < (int)size - 200; i++) {
                            GisLayer layer;
                            if (!_gis.getLayer(i, layer)) continue;
                            if (i > 0) buf[pos++] = ',';
                            pos += snprintf(buf + pos, size - pos,
                                "{\"name\":\"%s\",\"tile_count\":%u,\"zoom_min\":%u,\"zoom_max\":%u}",
                                layer.name, (unsigned)layer.tile_count, layer.zoom_min, layer.zoom_max);
                        }
                        pos += snprintf(buf + pos, size - pos, "]");
                        return pos;
                    });
                    Serial.printf("[tritium] GIS: %d layers, %u tiles\n",
                                  _gis.getLayerCount(), (unsigned)_gis.getTileCount());
                }
            }
#endif

            // Wire mesh topology data into web server
#if defined(ENABLE_ESPNOW)
            {
                auto* mesh = ServiceRegistry::getAs<EspNowService>("espnow");
                if (mesh) {
                    _webserver.setMeshProvider([mesh](char* buf, size_t size) -> int {
                        return mesh->toJson(buf, size);
                    });
                }
            }
#endif

            // In AP mode, start captive portal for auto-redirect
            if (wifi_svc->isAPMode()) {
                _webserver.startCaptivePortal();
                Serial.printf("[tritium] Web server: http://%s:%u/ (captive portal)\n",
                              wifi_svc->getAPIP(), web_port);
                Serial.printf("[tritium] Connect phone to WiFi '%s' for setup\n",
                              wifi_svc->getSSID());
            } else {
                // Normal mode: start mDNS for easy phone discovery
                uint8_t mac[6];
                WiFi.macAddress(mac);
                char hostname[32];
                snprintf(hostname, sizeof(hostname), "tritium-%02x%02x", mac[4], mac[5]);
                _webserver.startMDNS(hostname);
                Serial.printf("[tritium] Web server: http://%s:%u/ (mDNS: %s.local)\n",
                              wifi_svc->getIP(), web_port, hostname);
            }
            _active = true;
            return true;
        }
#endif
        return false;
    }

    void tick() override {
#if defined(ENABLE_WEBSERVER)
        if (_active) _webserver.process();
#endif
    }

private:
#if defined(ENABLE_WEBSERVER)
    WebServerHAL _webserver;
#endif
    bool _active = false;
};
