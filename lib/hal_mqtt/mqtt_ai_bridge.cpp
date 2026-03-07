#include "mqtt_ai_bridge.h"

#ifdef SIMULATOR

bool MqttAIBridge::init(MqttHAL*, AudioHAL*, const char*) { return false; }
void MqttAIBridge::setConfig(const AIBridgeConfig&) {}
bool MqttAIBridge::startVoiceQuery() { return false; }
void MqttAIBridge::stopVoiceQuery() {}
bool MqttAIBridge::sendTextPrompt(const char*) { return false; }
bool MqttAIBridge::sendSensorData(const char*, const char*) { return false; }
AIResponse MqttAIBridge::getResponse() { return {}; }
void MqttAIBridge::process() {}
void MqttAIBridge::publishAudioChunk() {}
void MqttAIBridge::onTextResponse(const uint8_t*, size_t) {}
void MqttAIBridge::onAudioResponse(const uint8_t*, size_t) {}
void MqttAIBridge::onServerCommand(const char*, const uint8_t*, size_t) {}
const char* MqttAIBridge::buildTopic(const char*, const char*) { return ""; }

#else

#include <Arduino.h>
#include "hal_mqtt.h"
#include "hal_audio.h"
#include <cstring>
#include <cstdio>

// Static pointer for MQTT callbacks to find us
static MqttAIBridge* _bridgeInstance = nullptr;

static void bridgeTextCb(const char* topic, const uint8_t* payload, size_t len) {
    if (_bridgeInstance) _bridgeInstance->onTextResponse(payload, len);
}

static void bridgeAudioCb(const char* topic, const uint8_t* payload, size_t len) {
    if (_bridgeInstance) _bridgeInstance->onAudioResponse(payload, len);
}

static void bridgeCommandCb(const char* topic, const uint8_t* payload, size_t len) {
    if (_bridgeInstance) _bridgeInstance->onServerCommand(topic, payload, len);
}

bool MqttAIBridge::init(MqttHAL* mqtt, AudioHAL* audio, const char* clientId) {
    if (!mqtt || !audio) return false;

    _mqtt = mqtt;
    _audio = audio;
    strncpy(_clientId, clientId ? clientId : "esp32", sizeof(_clientId) - 1);

    // Default config
    _config.serverTopicPrefix = "ai";
    _config.deviceTopicPrefix = "esp32";
    _config.audioChunkMs = 100;
    _config.maxStreamMs = 5000;
    _config.autoPlayResponse = true;

    _bridgeInstance = this;

    // Subscribe to AI server responses
    char topic[128];

    snprintf(topic, sizeof(topic), "ai/%s/text/response", _clientId);
    _mqtt->subscribe(topic, bridgeTextCb);

    snprintf(topic, sizeof(topic), "ai/%s/audio/response", _clientId);
    _mqtt->subscribe(topic, bridgeAudioCb);

    snprintf(topic, sizeof(topic), "ai/%s/command/#", _clientId);
    _mqtt->subscribe(topic, bridgeCommandCb);

    _initialized = true;
    Serial.printf("[AI Bridge] Init: client=%s, subscribed to ai/%s/#\n",
                  _clientId, _clientId);
    return true;
}

void MqttAIBridge::setConfig(const AIBridgeConfig& config) {
    _config = config;
}

const char* MqttAIBridge::buildTopic(const char* prefix, const char* suffix) {
    snprintf(_topicBuf, sizeof(_topicBuf), "%s/%s/%s", prefix, _clientId, suffix);
    return _topicBuf;
}

bool MqttAIBridge::startVoiceQuery() {
    if (!_initialized || !_mqtt->isConnected()) return false;
    if (_state == AIBridgeState::STREAMING_AUDIO) return false;

    _state = AIBridgeState::STREAMING_AUDIO;
    _streamStartMs = millis();
    _lastChunkMs = 0;
    _responseReady = false;
    memset(&_response, 0, sizeof(_response));

    // Notify server that audio stream is starting
    const char* topic = buildTopic("esp32", "audio/start");
    char meta[128];
    snprintf(meta, sizeof(meta),
             "{\"rate\":16000,\"bits\":16,\"channels\":1,\"format\":\"pcm\"}");
    _mqtt->publish(topic, meta);

    Serial.println("[AI Bridge] Voice query started, streaming audio...");
    return true;
}

void MqttAIBridge::stopVoiceQuery() {
    if (_state != AIBridgeState::STREAMING_AUDIO) return;

    // Notify server that audio stream ended
    const char* topic = buildTopic("esp32", "audio/stop");
    _mqtt->publish(topic, "{}");

    _state = AIBridgeState::WAITING_RESPONSE;
    Serial.printf("[AI Bridge] Audio stream stopped after %lums, waiting for response...\n",
                  (unsigned long)(millis() - _streamStartMs));
}

bool MqttAIBridge::sendTextPrompt(const char* prompt) {
    if (!_initialized || !_mqtt->isConnected() || !prompt) return false;

    const char* topic = buildTopic("esp32", "text/prompt");
    bool ok = _mqtt->publish(topic, prompt);

    if (ok) {
        _state = AIBridgeState::WAITING_RESPONSE;
        _responseReady = false;
        memset(&_response, 0, sizeof(_response));
        Serial.printf("[AI Bridge] Text prompt sent: %.40s...\n", prompt);
    }
    return ok;
}

bool MqttAIBridge::sendSensorData(const char* sensorType, const char* jsonPayload) {
    if (!_initialized || !_mqtt->isConnected()) return false;

    char topic[128];
    snprintf(topic, sizeof(topic), "esp32/%s/sensor/%s", _clientId, sensorType);
    return _mqtt->publish(topic, jsonPayload);
}

AIResponse MqttAIBridge::getResponse() {
    _responseReady = false;
    return _response;
}

void MqttAIBridge::process() {
    if (!_initialized) return;

    uint32_t now = millis();

    switch (_state) {
        case AIBridgeState::STREAMING_AUDIO: {
            // Stream audio chunks at configured interval
            if (now - _lastChunkMs >= _config.audioChunkMs) {
                _lastChunkMs = now;
                publishAudioChunk();
            }

            // Auto-stop after max duration
            if (now - _streamStartMs >= _config.maxStreamMs) {
                stopVoiceQuery();
            }
            break;
        }

        case AIBridgeState::WAITING_RESPONSE: {
            // Timeout after 30 seconds
            if (now - _streamStartMs > 30000) {
                Serial.println("[AI Bridge] Response timeout");
                _state = AIBridgeState::ERROR;
            }
            break;
        }

        case AIBridgeState::PLAYING_RESPONSE: {
            // After displaying/playing, return to idle
            // (caller should transition after consuming response)
            break;
        }

        default:
            break;
    }
}

void MqttAIBridge::publishAudioChunk() {
    if (!_audio || !_audio->hasMic()) return;

    // Read ~100ms of 16kHz audio = 1600 samples = 3200 bytes
    static int16_t chunkBuf[1600];
    size_t read = _audio->readMic(chunkBuf, 1600);

    if (read > 0) {
        const char* topic = buildTopic("esp32", "audio/stream");
        _mqtt->publish(topic, (const uint8_t*)chunkBuf,
                       read * sizeof(int16_t), false, 0);
    }
}

void MqttAIBridge::onTextResponse(const uint8_t* payload, size_t length) {
    if (length >= sizeof(_response.text)) {
        length = sizeof(_response.text) - 1;
    }
    memcpy(_response.text, payload, length);
    _response.text[length] = '\0';
    _response.hasText = true;
    _response.timestamp = millis();
    _responseReady = true;

    _state = AIBridgeState::PLAYING_RESPONSE;
    Serial.printf("[AI Bridge] Text response: %.60s...\n", _response.text);
}

void MqttAIBridge::onAudioResponse(const uint8_t* payload, size_t length) {
    _response.hasAudio = true;
    _response.timestamp = millis();
    _responseReady = true;

    // Play audio through speaker
    if (_config.autoPlayResponse && _audio && _audio->hasSpeaker()) {
        _audio->setSpeakerEnabled(true);
        _audio->playBuffer((const int16_t*)payload, length / sizeof(int16_t));
    }

    _state = AIBridgeState::PLAYING_RESPONSE;
    Serial.printf("[AI Bridge] Audio response: %d bytes\n", (int)length);
}

void MqttAIBridge::onServerCommand(const char* topic, const uint8_t* payload, size_t length) {
    // Extract action from topic: ai/{clientId}/command/{action}
    // Find the last '/' and get the action name
    const char* action = strrchr(topic, '/');
    if (action) action++; else action = topic;

    Serial.printf("[AI Bridge] Command: %s\n", action);

    if (_commandCb) {
        _commandCb(action, (const char*)payload, length);
    }
}

#endif // SIMULATOR
