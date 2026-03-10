#pragma once
// MQTT Client HAL
//
// Lightweight MQTT 3.1.1 client for ESP32-S3.
// Supports publish, subscribe, last-will, and auto-reconnect.
// Built on top of WiFi manager — connects only when WiFi is up.
//
// Usage:
//   #include "hal_mqtt.h"
//   MqttHAL mqtt;
//   mqtt.init("192.168.1.100", 1883, "esp32-board");
//   mqtt.subscribe("cmd/#", [](const char* topic, const char* payload, size_t len) {
//       Serial.printf("Got: %s = %s\n", topic, payload);
//   });
//   mqtt.publish("status/online", "1", true);
//   // In loop:
//   mqtt.process();

#include <cstdint>
#include <cstddef>

// Connection state
enum class MqttState : uint8_t {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
};

// Message callback
typedef void (*MqttMessageCallback)(const char* topic, const uint8_t* payload, size_t length);

// Subscription entry
static constexpr size_t MQTT_MAX_SUBS = 16;
static constexpr size_t MQTT_MAX_TOPIC_LEN = 128;

struct MqttSubscription {
    char topic[MQTT_MAX_TOPIC_LEN];
    MqttMessageCallback callback;
    uint8_t qos;
    bool active;
};

// Connection callback
typedef void (*MqttConnectCallback)(bool connected);

class MqttHAL {
public:
    // Initialize MQTT client
    bool init(const char* broker, uint16_t port = 1883,
              const char* clientId = nullptr);

    // Connection management
    bool connect();
    void disconnect();
    MqttState getState() const { return _state; }
    bool isConnected() const { return _state == MqttState::CONNECTED; }

    // Broker configuration
    void setBroker(const char* host, uint16_t port = 1883);
    void setCredentials(const char* username, const char* password);
    void setLastWill(const char* topic, const char* payload,
                     uint8_t qos = 0, bool retain = true);
    void setKeepAlive(uint16_t seconds) { _keepAlive = seconds; }
    void setAutoReconnect(bool enabled) { _autoReconnect = enabled; }

    // Publish
    bool publish(const char* topic, const char* payload,
                 bool retain = false, uint8_t qos = 0);
    bool publish(const char* topic, const uint8_t* payload,
                 size_t length, bool retain = false, uint8_t qos = 0);

    // Subscribe
    bool subscribe(const char* topic, MqttMessageCallback callback,
                   uint8_t qos = 0);
    bool unsubscribe(const char* topic);

    // Callbacks
    void onConnect(MqttConnectCallback cb) { _connectCb = cb; }

    // Process (call in loop)
    void process();

    // Stats
    uint32_t getMessagesSent() const { return _msgSent; }
    uint32_t getMessagesReceived() const { return _msgRecv; }
    uint32_t getReconnectCount() const { return _reconnects; }

private:
    MqttState _state = MqttState::DISCONNECTED;
    bool _initialized = false;
    bool _autoReconnect = true;

    char _broker[64] = {};
    uint16_t _port = 1883;
    char _clientId[32] = {};
    char _username[32] = {};
    char _password[64] = {};
    uint16_t _keepAlive = 60;

    // Last will
    char _lwTopic[MQTT_MAX_TOPIC_LEN] = {};
    char _lwPayload[64] = {};
    uint8_t _lwQos = 0;
    bool _lwRetain = true;
    bool _hasLastWill = false;

    // Subscriptions
    MqttSubscription _subs[MQTT_MAX_SUBS] = {};
    int _numSubs = 0;

    // Callbacks
    MqttConnectCallback _connectCb = nullptr;

    // Stats
    uint32_t _msgSent = 0;
    uint32_t _msgRecv = 0;
    uint32_t _reconnects = 0;

    // Reconnect timing
    uint32_t _lastConnectAttempt = 0;
    uint32_t _reconnectInterval = 5000; // ms

    // Internal
    void* _client = nullptr;  // esp_mqtt_client_handle_t (opaque to avoid header dep)
    void resubscribeAll();

public:
    // Called by static bridge — not for user code
    void handleMessage(const char* topic, const uint8_t* payload, size_t length);
};
