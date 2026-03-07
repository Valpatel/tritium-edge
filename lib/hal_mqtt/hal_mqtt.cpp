#include "hal_mqtt.h"

#ifdef SIMULATOR

bool MqttHAL::init(const char*, uint16_t, const char*) { return false; }
bool MqttHAL::connect() { return false; }
void MqttHAL::disconnect() {}
void MqttHAL::setBroker(const char*, uint16_t) {}
void MqttHAL::setCredentials(const char*, const char*) {}
void MqttHAL::setLastWill(const char*, const char*, uint8_t, bool) {}
bool MqttHAL::publish(const char*, const char*, bool, uint8_t) { return false; }
bool MqttHAL::publish(const char*, const uint8_t*, size_t, bool, uint8_t) { return false; }
bool MqttHAL::subscribe(const char*, MqttMessageCallback, uint8_t) { return false; }
bool MqttHAL::unsubscribe(const char*) { return false; }
void MqttHAL::process() {}
void MqttHAL::handleMessage(const char*, const uint8_t*, size_t) {}
void MqttHAL::resubscribeAll() {}

#else // ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <cstring>

// We store a global pointer so the PubSubClient callback can find us
static MqttHAL* _globalMqtt = nullptr;
static WiFiClient _wifiClient;

static void mqttCallbackBridge(char* topic, byte* payload, unsigned int length) {
    if (_globalMqtt) {
        _globalMqtt->handleMessage(topic, payload, length);
    }
}

bool MqttHAL::init(const char* broker, uint16_t port, const char* clientId) {
    if (_initialized) return true;

    strncpy(_broker, broker, sizeof(_broker) - 1);
    _port = port;

    if (clientId && clientId[0]) {
        strncpy(_clientId, clientId, sizeof(_clientId) - 1);
    } else {
        // Generate from MAC
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(_clientId, sizeof(_clientId), "esp32-%02X%02X%02X",
                 mac[3], mac[4], mac[5]);
    }

    PubSubClient* ps = new PubSubClient(_wifiClient);
    ps->setServer(_broker, _port);
    ps->setCallback(mqttCallbackBridge);
    ps->setBufferSize(1024);
    ps->setKeepAlive(_keepAlive);
    _client = ps;
    _globalMqtt = this;
    _initialized = true;

    Serial.printf("[MQTT] Init: broker=%s:%d client=%s\n", _broker, _port, _clientId);
    return true;
}

void MqttHAL::setBroker(const char* host, uint16_t port) {
    strncpy(_broker, host, sizeof(_broker) - 1);
    _port = port;
    if (_client) {
        ((PubSubClient*)_client)->setServer(_broker, _port);
    }
}

void MqttHAL::setCredentials(const char* username, const char* password) {
    if (username) strncpy(_username, username, sizeof(_username) - 1);
    if (password) strncpy(_password, password, sizeof(_password) - 1);
}

void MqttHAL::setLastWill(const char* topic, const char* payload,
                           uint8_t qos, bool retain) {
    strncpy(_lwTopic, topic, sizeof(_lwTopic) - 1);
    strncpy(_lwPayload, payload, sizeof(_lwPayload) - 1);
    _lwQos = qos;
    _lwRetain = retain;
    _hasLastWill = true;
}

bool MqttHAL::connect() {
    if (!_initialized || !_client) return false;

    PubSubClient* ps = (PubSubClient*)_client;

    // Check WiFi first
    if (WiFi.status() != WL_CONNECTED) {
        _state = MqttState::DISCONNECTED;
        return false;
    }

    _state = MqttState::CONNECTING;
    _lastConnectAttempt = millis();

    bool ok = false;
    if (_hasLastWill) {
        if (_username[0]) {
            ok = ps->connect(_clientId, _username, _password,
                             _lwTopic, _lwQos, _lwRetain, _lwPayload);
        } else {
            ok = ps->connect(_clientId, nullptr, nullptr,
                             _lwTopic, _lwQos, _lwRetain, _lwPayload);
        }
    } else {
        if (_username[0]) {
            ok = ps->connect(_clientId, _username, _password);
        } else {
            ok = ps->connect(_clientId);
        }
    }

    if (ok) {
        _state = MqttState::CONNECTED;
        _reconnects++;
        Serial.printf("[MQTT] Connected to %s:%d as %s\n", _broker, _port, _clientId);
        resubscribeAll();
        if (_connectCb) _connectCb(true);
    } else {
        _state = MqttState::ERROR;
        Serial.printf("[MQTT] Connect failed, rc=%d\n", ps->state());
        if (_connectCb) _connectCb(false);
    }

    return ok;
}

void MqttHAL::disconnect() {
    if (_client) {
        ((PubSubClient*)_client)->disconnect();
    }
    _state = MqttState::DISCONNECTED;
    Serial.println("[MQTT] Disconnected");
}

bool MqttHAL::publish(const char* topic, const char* payload,
                       bool retain, uint8_t qos) {
    return publish(topic, (const uint8_t*)payload, strlen(payload), retain, qos);
}

bool MqttHAL::publish(const char* topic, const uint8_t* payload,
                       size_t length, bool retain, uint8_t qos) {
    if (!_initialized || !_client || _state != MqttState::CONNECTED) return false;

    PubSubClient* ps = (PubSubClient*)_client;
    bool ok = ps->publish(topic, payload, length, retain);
    if (ok) _msgSent++;
    return ok;
}

bool MqttHAL::subscribe(const char* topic, MqttMessageCallback callback,
                          uint8_t qos) {
    if (!topic || !callback) return false;

    // Find existing or empty slot
    int slot = -1;
    for (int i = 0; i < MQTT_MAX_SUBS; i++) {
        if (_subs[i].active && strcmp(_subs[i].topic, topic) == 0) {
            slot = i;
            break;
        }
        if (!_subs[i].active && slot < 0) {
            slot = i;
        }
    }

    if (slot < 0) return false;

    bool wasActive = _subs[slot].active;
    strncpy(_subs[slot].topic, topic, MQTT_MAX_TOPIC_LEN - 1);
    _subs[slot].callback = callback;
    _subs[slot].qos = qos;
    _subs[slot].active = true;
    if (!wasActive) _numSubs++;

    // Subscribe on broker if connected
    if (_state == MqttState::CONNECTED && _client) {
        ((PubSubClient*)_client)->subscribe(topic, qos);
    }

    return true;
}

bool MqttHAL::unsubscribe(const char* topic) {
    for (int i = 0; i < MQTT_MAX_SUBS; i++) {
        if (_subs[i].active && strcmp(_subs[i].topic, topic) == 0) {
            _subs[i].active = false;
            _numSubs--;
            if (_state == MqttState::CONNECTED && _client) {
                ((PubSubClient*)_client)->unsubscribe(topic);
            }
            return true;
        }
    }
    return false;
}

void MqttHAL::process() {
    if (!_initialized || !_client) return;

    PubSubClient* ps = (PubSubClient*)_client;

    // Check connection
    if (!ps->connected()) {
        if (_state == MqttState::CONNECTED) {
            _state = MqttState::DISCONNECTED;
            Serial.println("[MQTT] Connection lost");
            if (_connectCb) _connectCb(false);
        }

        // Auto-reconnect
        if (_autoReconnect && WiFi.status() == WL_CONNECTED) {
            uint32_t now = millis();
            if (now - _lastConnectAttempt >= _reconnectInterval) {
                connect();
            }
        }
        return;
    }

    // Process incoming messages
    ps->loop();
}

// Simple MQTT topic pattern match (supports + and # wildcards)
static bool topicMatch(const char* pattern, const char* topic) {
    while (*pattern && *topic) {
        if (*pattern == '#') return true; // # matches rest
        if (*pattern == '+') {
            // + matches one level
            while (*topic && *topic != '/') topic++;
            pattern++;
            if (*pattern == '/') pattern++;
            if (*topic == '/') topic++;
            continue;
        }
        if (*pattern != *topic) return false;
        pattern++;
        topic++;
    }
    return (*pattern == '\0' && *topic == '\0') || *pattern == '#';
}

void MqttHAL::handleMessage(const char* topic, const uint8_t* payload, size_t length) {
    _msgRecv++;

    for (int i = 0; i < MQTT_MAX_SUBS; i++) {
        if (_subs[i].active && _subs[i].callback) {
            if (topicMatch(_subs[i].topic, topic)) {
                _subs[i].callback(topic, payload, length);
            }
        }
    }
}

void MqttHAL::resubscribeAll() {
    if (!_client || _state != MqttState::CONNECTED) return;

    PubSubClient* ps = (PubSubClient*)_client;
    for (int i = 0; i < MQTT_MAX_SUBS; i++) {
        if (_subs[i].active) {
            ps->subscribe(_subs[i].topic, _subs[i].qos);
        }
    }
}

#endif // SIMULATOR
