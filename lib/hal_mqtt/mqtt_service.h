#pragma once
// MQTT Service adapter — wraps MqttHAL as a ServiceInterface.
// Connects edge nodes to Tritium-SC via MQTT broker.
// Priority 70 (after WiFi, scanners, heartbeat).
//
// Topic structure:
//   tritium/{device_id}/status       → online/offline (retained, last will)
//   tritium/{device_id}/heartbeat    → periodic JSON (heap, uptime, services)
//   tritium/{device_id}/sighting     → BLE/WiFi sighting summaries
//   tritium/{device_id}/chat         → mesh chat messages bridged to MQTT
//   tritium/{device_id}/cmd          ← commands from SC (subscribe)
//   tritium/{device_id}/config       ← config updates from SC (subscribe)

#include "service.h"
#include <cstring>
#include <cstdio>
#ifndef SIMULATOR
#include <esp_mac.h>
#include <esp_heap_caps.h>
#endif

#if __has_include("hal_mqtt.h")
#include "hal_mqtt.h"
#define MQTT_HAL_AVAILABLE 1
#else
#define MQTT_HAL_AVAILABLE 0
#endif

#if __has_include("os_settings.h")
#include "os_settings.h"
#endif

#if defined(ENABLE_BLE_SCANNER) && __has_include("hal_ble_scanner.h")
#include "hal_ble_scanner.h"
#define MQTT_BLE_STATS 1
#else
#define MQTT_BLE_STATS 0
#endif

#if defined(ENABLE_SIGHTING_LOGGER) && __has_include("hal_sighting_logger.h")
#include "hal_sighting_logger.h"
#define MQTT_LOGGER_STATS 1
#else
#define MQTT_LOGGER_STATS 0
#endif

class MqttService : public ServiceInterface {
public:
    const char* name() const override { return "mqtt"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD; }
    int initPriority() const override { return 70; }

    bool init() override {
#if MQTT_HAL_AVAILABLE
        // Read broker config from NVS settings
        const char* broker = "192.168.1.100";
        uint16_t port = 1883;

#if __has_include("os_settings.h")
        auto& settings = TritiumSettings::instance();
        const char* b = settings.getString(SettingsDomain::WIFI, "mqtt_broker", "");
        if (b && b[0]) broker = b;

        port = (uint16_t)settings.getInt(SettingsDomain::WIFI, "mqtt_port", 1883);
#endif

        // Build client ID from MAC
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        snprintf(_clientId, sizeof(_clientId), "tritium-%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
        snprintf(_topicPrefix, sizeof(_topicPrefix), "tritium/%s", _clientId);

        if (!_mqtt.init(broker, port, _clientId)) {
            Serial.printf("[mqtt] Init failed (broker=%s:%u)\n", broker, port);
            return false;
        }

        // Set last will (offline status)
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/status", _topicPrefix);
        _mqtt.setLastWill(topic, "offline", 1, true);
        _mqtt.setAutoReconnect(true);

        // Connection callback — publish online status and subscribe
        _mqtt.onConnect([](bool connected) {
            // Static access through singleton pattern isn't great but
            // the callback signature doesn't support user data
            Serial.printf("[mqtt] %s\n", connected ? "Connected" : "Disconnected");
        });

        // Subscribe to command topic
        snprintf(topic, sizeof(topic), "%s/cmd", _topicPrefix);
        _mqtt.subscribe(topic, [](const char* topic, const uint8_t* payload, size_t len) {
            // Handle commands from SC
            char cmd[256];
            size_t clen = (len < sizeof(cmd) - 1) ? len : sizeof(cmd) - 1;
            memcpy(cmd, payload, clen);
            cmd[clen] = '\0';
            Serial.printf("[mqtt] CMD: %s\n", cmd);

            // Dispatch to service registry
            char* space = strchr(cmd, ' ');
            const char* args = "";
            if (space) { *space = '\0'; args = space + 1; }
            ServiceRegistry::dispatchCommand(cmd, args);
        }, 1);

        _mqtt.connect();
        Serial.printf("[mqtt] Connecting to %s:%u as %s\n", broker, port, _clientId);
        _active = true;
        return true;
#else
        return false;
#endif
    }

    void tick() override {
#if MQTT_HAL_AVAILABLE
        if (!_active) return;
        _mqtt.process();

        // Periodic heartbeat (every 30s)
        uint32_t now = millis();
        if (_mqtt.isConnected() && (now - _lastHeartbeat) >= 30000) {
            _lastHeartbeat = now;
            publishHeartbeat();
        }

        // Publish online status on first connect
        if (_mqtt.isConnected() && !_published_online) {
            _published_online = true;
            char topic[128];
            snprintf(topic, sizeof(topic), "%s/status", _topicPrefix);
            _mqtt.publish(topic, "online", true, 1);
        }
        if (!_mqtt.isConnected()) {
            _published_online = false;
        }
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if MQTT_HAL_AVAILABLE
        if (strcmp(cmd, "MQTT_STATUS") == 0) {
            Serial.printf("[mqtt] state=%s sent=%lu recv=%lu reconnects=%lu\n",
                _mqtt.isConnected() ? "connected" : "disconnected",
                (unsigned long)_mqtt.getMessagesSent(),
                (unsigned long)_mqtt.getMessagesReceived(),
                (unsigned long)_mqtt.getReconnectCount());
            return true;
        }
        if (strcmp(cmd, "MQTT_PUB") == 0 && args) {
            // MQTT_PUB topic payload
            char topic[128];
            const char* space = strchr(args, ' ');
            if (!space) return true;
            size_t tlen = space - args;
            if (tlen >= sizeof(topic)) tlen = sizeof(topic) - 1;
            memcpy(topic, args, tlen);
            topic[tlen] = '\0';
            _mqtt.publish(topic, space + 1);
            Serial.printf("[mqtt] Published to %s\n", topic);
            return true;
        }
        if (strcmp(cmd, "MQTT_BROKER") == 0 && args) {
#if __has_include("os_settings.h")
            auto& settings = TritiumSettings::instance();
            settings.setString(SettingsDomain::WIFI, "mqtt_broker", args);
            Serial.printf("[mqtt] Broker set to %s (reconnect to apply)\n", args);
#endif
            return true;
        }
#endif
        return false;
    }

    // Public: allow other services to publish to MQTT
    bool publish(const char* subtopic, const char* payload, bool retain = false) {
#if MQTT_HAL_AVAILABLE
        if (!_active || !_mqtt.isConnected()) return false;
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/%s", _topicPrefix, subtopic);
        return _mqtt.publish(topic, payload, retain);
#else
        return false;
#endif
    }

#if MQTT_HAL_AVAILABLE
    MqttHAL& mqtt() { return _mqtt; }
#endif

private:
    bool _active = false;
    bool _published_online = false;
    uint32_t _lastHeartbeat = 0;
    char _clientId[32] = {};
    char _topicPrefix[64] = {};

#if MQTT_HAL_AVAILABLE
    MqttHAL _mqtt;

    void publishHeartbeat() {
        char buf[512];
        uint32_t uptime = millis() / 1000;
        size_t heap = esp_get_free_heap_size();
        size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        int pos = snprintf(buf, sizeof(buf),
            "{\"uptime\":%lu,\"heap\":%u,\"psram\":%u,\"services\":%d",
            (unsigned long)uptime,
            (unsigned)(heap / 1024),
            (unsigned)(psram / 1024),
            ServiceRegistry::count());

#if MQTT_BLE_STATS
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",\"ble_devices\":%d", hal_ble_scanner::get_visible_count());
#endif

#if MQTT_LOGGER_STATS
        if (hal_sighting_logger::is_active()) {
            auto s = hal_sighting_logger::get_stats();
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                ",\"sightings\":{\"ble\":%lu,\"wifi\":%lu}",
                (unsigned long)s.ble_logged, (unsigned long)s.wifi_logged);
        }
#endif

        snprintf(buf + pos, sizeof(buf) - pos, "}");

        char topic[128];
        snprintf(topic, sizeof(topic), "%s/heartbeat", _topicPrefix);
        _mqtt.publish(topic, buf);
    }
#endif
};
