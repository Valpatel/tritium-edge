#pragma once
// MQTT AI Bridge
//
// Connects ESP32 voice/sensor pipeline to a local AI server (e.g. GB10)
// via MQTT. Supports:
//   - Streaming audio chunks for STT
//   - Sending text prompts for LLM inference
//   - Receiving text responses for display
//   - Receiving audio responses for TTS playback
//   - Bidirectional sensor/command exchange
//
// MQTT Topic Structure:
//   esp32/{clientId}/audio/stream   → raw PCM chunks to server
//   esp32/{clientId}/text/prompt    → text prompt to server
//   esp32/{clientId}/sensor/{type}  → sensor data to server
//   ai/{clientId}/text/response     → text response from server
//   ai/{clientId}/audio/response    → audio response from server
//   ai/{clientId}/command/{action}  → commands from server to ESP32

#include <cstdint>
#include <cstddef>

class MqttHAL;
class AudioHAL;

// AI bridge state
enum class AIBridgeState : uint8_t {
    IDLE,               // Waiting for wake word / trigger
    STREAMING_AUDIO,    // Sending mic audio to server
    WAITING_RESPONSE,   // Audio sent, waiting for LLM response
    PLAYING_RESPONSE,   // Playing back audio/text response
    ERROR
};

// Response from AI server
struct AIResponse {
    char text[512];         // Text response (for display)
    bool hasText;
    bool hasAudio;          // Audio response available
    uint32_t timestamp;
};

// Command from AI server
typedef void (*AICommandCallback)(const char* action, const char* payload, size_t len);

// Configuration
struct AIBridgeConfig {
    const char* serverTopicPrefix;  // e.g. "ai" — server publishes here
    const char* deviceTopicPrefix;  // e.g. "esp32" — device publishes here
    uint16_t audioChunkMs;          // Audio chunk duration (default 100ms)
    uint16_t maxStreamMs;           // Max audio stream duration (default 5000ms)
    bool autoPlayResponse;          // Auto-play audio responses
};

class MqttAIBridge {
public:
    bool init(MqttHAL* mqtt, AudioHAL* audio, const char* clientId);
    void setConfig(const AIBridgeConfig& config);

    // Trigger voice interaction (call after wake word detected)
    bool startVoiceQuery();
    void stopVoiceQuery();

    // Send text prompt directly (skip voice)
    bool sendTextPrompt(const char* prompt);

    // Send sensor data
    bool sendSensorData(const char* sensorType, const char* jsonPayload);

    // State
    AIBridgeState getState() const { return _state; }
    bool hasResponse() const { return _responseReady; }
    AIResponse getResponse();

    // Callbacks
    void onCommand(AICommandCallback cb) { _commandCb = cb; }

    // Process (call in loop)
    void process();

private:
    MqttHAL* _mqtt = nullptr;
    AudioHAL* _audio = nullptr;
    AIBridgeState _state = AIBridgeState::IDLE;
    bool _initialized = false;

    char _clientId[32] = {};
    AIBridgeConfig _config = {};

    // Streaming state
    uint32_t _streamStartMs = 0;
    uint32_t _lastChunkMs = 0;

    // Response
    AIResponse _response = {};
    bool _responseReady = false;

    // Callbacks
    AICommandCallback _commandCb = nullptr;

    // Topic buffers
    char _topicBuf[128] = {};

    // Internal helpers
    void publishAudioChunk();
    const char* buildTopic(const char* prefix, const char* suffix);

public:
    // Called by static MQTT callbacks — not for user code
    void onTextResponse(const uint8_t* payload, size_t length);
    void onAudioResponse(const uint8_t* payload, size_t length);
    void onServerCommand(const char* topic, const uint8_t* payload, size_t length);
};
