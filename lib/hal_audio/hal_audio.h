#pragma once
// Audio HAL for ES8311 codec + I2S
//
// Usage:
//   #include "hal_audio.h"
//   AudioHAL audio;
//   audio.initLgfx(0, 0x18);  // ESP32 via lgfx::i2c (shared bus)
//   audio.init(Wire);          // ESP32 via Arduino Wire
//   audio.init();              // simulator

#include <cstdint>
#include <cstddef>

#ifndef SIMULATOR
class TwoWire;
#endif

// Audio sample format
struct AudioBuffer {
    int16_t* data;
    size_t samples;
    uint32_t sampleRate;
    uint8_t channels;
};

class AudioHAL {
public:
#ifdef SIMULATOR
    bool init();
#else
    bool init(TwoWire &wire);
    bool initLgfx(uint8_t i2c_port, uint8_t addr = 0x18);
#endif

    // Playback
    bool setVolume(uint8_t volume);  // 0-100
    uint8_t getVolume() const { return _volume; }
    bool playTone(uint16_t freqHz, uint32_t durationMs);
    bool playBuffer(const int16_t* data, size_t samples);
    bool setSpeakerEnabled(bool enabled);

    // Recording
    bool startRecording(int16_t *buffer, size_t samples);
    size_t readMic(int16_t *buffer, size_t maxSamples);
    bool setMicGain(uint8_t gain);  // 0-100

    // Audio analysis
    float getMicLevel();  // Returns RMS level 0.0-1.0
    bool getSpectrum(float* bins, size_t numBins, const int16_t* samples, size_t numSamples);

    // State
    bool available() const { return _initialized; }
    bool hasMic() const { return _has_mic; }
    bool hasCodec() const { return _has_codec; }
    bool hasSpeaker() const { return _has_speaker; }
    uint32_t getSampleRate() const { return _sample_rate; }

private:
    bool _initialized = false;
    bool _has_codec = false;
    bool _has_mic = false;
    bool _has_speaker = false;
    uint8_t _volume = 70;
    uint32_t _sample_rate = 16000;

#ifndef SIMULATOR
    // I2C mode
    TwoWire *_wire = nullptr;
    bool _use_lgfx = false;
    uint8_t _lgfx_port = 0;
    uint8_t _codec_addr = 0;
    int _i2s_port = 0;

    bool initES8311();
    bool initI2S();
    void writeReg(uint8_t reg, uint8_t val);
    uint8_t readReg(uint8_t reg);
#endif
};
