#pragma once
// Voice Command Detection HAL
//
// Provides on-device voice activity detection and simple keyword spotting
// using energy analysis and spectral features. Works with Arduino framework.
//
// For complex recognition, supports streaming audio to a WiFi endpoint.
//
// Usage:
//   #include "hal_voice.h"
//   VoiceHAL voice;
//   voice.init(&audioHAL);
//   voice.addCommand("yes", VOICE_CMD_YES);
//   voice.addCommand("no", VOICE_CMD_NO);
//   // In loop:
//   voice.process();
//   if (voice.hasCommand()) { auto cmd = voice.getCommand(); ... }

#include <cstdint>
#include <cstddef>

class AudioHAL;

// Voice activity state
enum class VoiceState : uint8_t {
    IDLE,           // No audio activity
    LISTENING,      // VAD triggered, collecting audio
    PROCESSING,     // Analyzing captured audio
    COMMAND_READY   // Command detected, waiting for retrieval
};

// Built-in command IDs (user can add custom ones starting at 100)
enum VoiceCommand : uint8_t {
    VOICE_CMD_NONE = 0,
    VOICE_CMD_YES = 1,
    VOICE_CMD_NO = 2,
    VOICE_CMD_STOP = 3,
    VOICE_CMD_GO = 4,
    VOICE_CMD_UP = 5,
    VOICE_CMD_DOWN = 6,
    VOICE_CMD_LEFT = 7,
    VOICE_CMD_RIGHT = 8,
    VOICE_CMD_CUSTOM_START = 100
};

// Detection result
struct VoiceDetection {
    uint8_t commandId;
    float confidence;     // 0.0 - 1.0
    uint32_t timestamp;   // millis() when detected
    const char* label;    // Human-readable name
};

// VAD (Voice Activity Detection) configuration
struct VADConfig {
    float energyThreshold;      // RMS threshold to trigger (0.0-1.0), default 0.02
    uint32_t minSpeechMs;       // Minimum speech duration (ms), default 200
    uint32_t maxSpeechMs;       // Maximum speech duration (ms), default 3000
    uint32_t silenceTimeoutMs;  // Silence before end-of-speech (ms), default 500
    uint32_t cooldownMs;        // Cooldown between detections (ms), default 1000
};

// Spectral feature vector for keyword matching
static constexpr size_t VOICE_NUM_FEATURES = 13;  // MFCC-like feature count
static constexpr size_t VOICE_MAX_FRAMES = 32;     // Max time frames per utterance
static constexpr size_t VOICE_MAX_COMMANDS = 16;

struct VoiceTemplate {
    uint8_t commandId;
    char label[16];
    float features[VOICE_MAX_FRAMES][VOICE_NUM_FEATURES];
    uint8_t numFrames;
    uint8_t numSamples;  // Number of training samples averaged
};

// Callback types
typedef void (*VoiceCommandCallback)(const VoiceDetection& detection);
typedef void (*VoiceStateCallback)(VoiceState state);

class VoiceHAL {
public:
    bool init(AudioHAL* audio);

    // VAD configuration
    void setVADConfig(const VADConfig& config);
    VADConfig getVADConfig() const { return _vadConfig; }

    // Command registration
    bool addCommand(const char* label, uint8_t commandId);
    bool trainCommand(uint8_t commandId);  // Record a training sample
    bool removeCommand(uint8_t commandId);
    int getCommandCount() const { return _numCommands; }

    // Processing (call in loop)
    void process();

    // State
    VoiceState getState() const { return _state; }
    bool hasCommand() const { return _state == VoiceState::COMMAND_READY; }
    VoiceDetection getCommand();

    // Callbacks
    void onCommand(VoiceCommandCallback cb) { _commandCb = cb; }
    void onStateChange(VoiceStateCallback cb) { _stateCb = cb; }

    // Audio analysis utilities
    float getInstantLevel() const { return _instantLevel; }
    bool isVoiceActive() const { return _voiceActive; }

    // Enable/disable
    void setEnabled(bool enabled) { _enabled = enabled; }
    bool isEnabled() const { return _enabled; }

private:
    AudioHAL* _audio = nullptr;
    bool _enabled = true;

    // VAD state
    VADConfig _vadConfig;
    VoiceState _state = VoiceState::IDLE;
    bool _voiceActive = false;
    float _instantLevel = 0.0f;
    uint32_t _speechStartMs = 0;
    uint32_t _lastSpeechMs = 0;
    uint32_t _lastDetectionMs = 0;

    // Audio capture buffer
    static constexpr size_t CAPTURE_BUF_SIZE = 16000 * 3;  // 3 seconds at 16kHz
    int16_t* _captureBuf = nullptr;
    size_t _capturePos = 0;

    // Command templates
    VoiceTemplate _commands[VOICE_MAX_COMMANDS];
    int _numCommands = 0;

    // Detection result
    VoiceDetection _lastDetection = {};

    // Callbacks
    VoiceCommandCallback _commandCb = nullptr;
    VoiceStateCallback _stateCb = nullptr;

    // Internal methods
    void setState(VoiceState newState);
    void processIdle();
    void processListening();
    void processAudio();
    void extractFeatures(const int16_t* samples, size_t numSamples,
                         float features[][VOICE_NUM_FEATURES], uint8_t& numFrames);
    float matchTemplate(const float features[][VOICE_NUM_FEATURES], uint8_t numFrames,
                        const VoiceTemplate& tmpl);
    void computeMelFilterbank(const float* spectrum, size_t specLen,
                              float* melBins, size_t numBins);
};
