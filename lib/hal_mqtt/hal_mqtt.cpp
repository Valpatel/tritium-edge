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

#include "tritium_compat.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include <mqtt_client.h>
#include <cstring>

// We store a global pointer so the ESP-IDF MQTT event handler can find us
static MqttHAL* _globalMqtt = nullptr;

// ESP-IDF MQTT event handler
static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                                int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    MqttHAL* self = (MqttHAL*)handler_args;
    if (!self) return;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        self->handleMessage(nullptr, nullptr, 0);  // Signal connected (handled below)
        break;
    case MQTT_EVENT_DISCONNECTED:
        break;
    case MQTT_EVENT_DATA:
        if (event->topic && event->topic_len > 0) {
            // Null-terminate the topic for our callback
            char topic_buf[MQTT_MAX_TOPIC_LEN];
            size_t tlen = (size_t)event->topic_len < sizeof(topic_buf) - 1
                          ? (size_t)event->topic_len : sizeof(topic_buf) - 1;
            memcpy(topic_buf, event->topic, tlen);
            topic_buf[tlen] = '\0';
            self->handleMessage(topic_buf, (const uint8_t*)event->data,
                                (size_t)event->data_len);
        }
        break;
    default:
        break;
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
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(_clientId, sizeof(_clientId), "esp32-%02X%02X%02X",
                 mac[3], mac[4], mac[5]);
    }

    // Client is created in connect() since ESP-IDF MQTT config is set at init time
    _globalMqtt = this;
    _initialized = true;

    Serial.printf("[MQTT] Init: broker=%s:%d client=%s\n", _broker, _port, _clientId);
    return true;
}

void MqttHAL::setBroker(const char* host, uint16_t port) {
    strncpy(_broker, host, sizeof(_broker) - 1);
    _port = port;
    // If already connected, need to reconnect for new broker to take effect
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
    if (!_initialized) return false;

    // Check WiFi first
    { wifi_ap_record_t _ap;
      if (esp_wifi_sta_get_ap_info(&_ap) != ESP_OK) {
        _state = MqttState::DISCONNECTED;
        return false;
      }
    }

    _state = MqttState::CONNECTING;
    _lastConnectAttempt = millis();

    // Destroy old client if any
    if (_client) {
        esp_mqtt_client_stop((esp_mqtt_client_handle_t)_client);
        esp_mqtt_client_destroy((esp_mqtt_client_handle_t)_client);
        _client = nullptr;
    }

    // Build broker URI
    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", _broker, _port);

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = uri;
    mqtt_cfg.credentials.client_id = _clientId;
    mqtt_cfg.session.keepalive = _keepAlive;
    mqtt_cfg.buffer.size = 1024;

    if (_username[0]) {
        mqtt_cfg.credentials.username = _username;
        mqtt_cfg.credentials.authentication.password = _password;
    }

    if (_hasLastWill) {
        mqtt_cfg.session.last_will.topic = _lwTopic;
        mqtt_cfg.session.last_will.msg = _lwPayload;
        mqtt_cfg.session.last_will.qos = _lwQos;
        mqtt_cfg.session.last_will.retain = _lwRetain ? 1 : 0;
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        _state = MqttState::ERROR;
        Serial.printf("[MQTT] Client init failed\n");
        if (_connectCb) _connectCb(false);
        return false;
    }

    esp_mqtt_client_register_event(client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, this);

    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        _state = MqttState::ERROR;
        Serial.printf("[MQTT] Client start failed: 0x%x\n", err);
        esp_mqtt_client_destroy(client);
        if (_connectCb) _connectCb(false);
        return false;
    }

    _client = client;
    _state = MqttState::CONNECTED;
    _reconnects++;
    Serial.printf("[MQTT] Connected to %s:%d as %s\n", _broker, _port, _clientId);
    resubscribeAll();
    if (_connectCb) _connectCb(true);

    return true;
}

void MqttHAL::disconnect() {
    if (_client) {
        esp_mqtt_client_stop((esp_mqtt_client_handle_t)_client);
        esp_mqtt_client_destroy((esp_mqtt_client_handle_t)_client);
        _client = nullptr;
    }
    _state = MqttState::DISCONNECTED;
    Serial.printf("[MQTT] Disconnected\n");
}

bool MqttHAL::publish(const char* topic, const char* payload,
                       bool retain, uint8_t qos) {
    return publish(topic, (const uint8_t*)payload, strlen(payload), retain, qos);
}

bool MqttHAL::publish(const char* topic, const uint8_t* payload,
                       size_t length, bool retain, uint8_t qos) {
    if (!_initialized || !_client || _state != MqttState::CONNECTED) return false;

    int msg_id = esp_mqtt_client_publish((esp_mqtt_client_handle_t)_client,
                                          topic, (const char*)payload, (int)length,
                                          qos, retain ? 1 : 0);
    if (msg_id >= 0) {
        _msgSent++;
        return true;
    }
    return false;
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
        esp_mqtt_client_subscribe((esp_mqtt_client_handle_t)_client, topic, qos);
    }

    return true;
}

bool MqttHAL::unsubscribe(const char* topic) {
    for (int i = 0; i < MQTT_MAX_SUBS; i++) {
        if (_subs[i].active && strcmp(_subs[i].topic, topic) == 0) {
            _subs[i].active = false;
            _numSubs--;
            if (_state == MqttState::CONNECTED && _client) {
                esp_mqtt_client_unsubscribe((esp_mqtt_client_handle_t)_client, topic);
            }
            return true;
        }
    }
    return false;
}

void MqttHAL::process() {
    if (!_initialized || !_client) return;

    // ESP-IDF MQTT client runs in its own task — events are delivered via
    // the event handler callback. No explicit loop() call needed.
    // We just check for auto-reconnect here.
    if (_state != MqttState::CONNECTED) {
        if (_state == MqttState::DISCONNECTED) {
            // Auto-reconnect
            wifi_ap_record_t _ap;
            if (_autoReconnect && esp_wifi_sta_get_ap_info(&_ap) == ESP_OK) {
                uint32_t now = millis();
                if (now - _lastConnectAttempt >= _reconnectInterval) {
                    connect();
                }
            }
        }
        return;
    }
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
    // Null topic = connection event signal (from event handler)
    if (!topic) return;

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

    for (int i = 0; i < MQTT_MAX_SUBS; i++) {
        if (_subs[i].active) {
            esp_mqtt_client_subscribe((esp_mqtt_client_handle_t)_client,
                                      _subs[i].topic, _subs[i].qos);
        }
    }
}

#endif // SIMULATOR
