// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_acoustic.h"
#include <cstdio>
#include <cstring>

// ============================================================================
// Stub implementation — compiles on all targets.
// Real implementation will use AudioHAL microphone + FFT analysis.
// ============================================================================

namespace hal_acoustic {

static bool _initialized = false;
static AcousticConfig _config;
static AcousticState _state = {};
static event_callback_t _event_cb = nullptr;
static float _min_confidence = 0.5f;
static uint32_t _event_count = 0;
static uint32_t _event_counts[8] = {};  // one per SoundClass

bool init(const AcousticConfig& config) {
    if (_initialized) return true;
    _config = config;
    memset(&_state, 0, sizeof(_state));
    _state.classification = SoundClass::SILENCE;
    _initialized = true;
    return true;
}

void shutdown() {
    _initialized = false;
}

void tick() {
    if (!_initialized) return;
    // Stub: no-op — real impl captures samples + runs FFT + VAD
}

int get_samples(int16_t* out, int max_samples) {
    (void)out;
    (void)max_samples;
    return 0;  // Stub: no samples available
}

int get_spectrum(float* bins, int max_bins) {
    if (!bins || max_bins <= 0) return 0;
    // Stub: return zeroed spectrum
    int count = max_bins;
    if (count > (int)(_config.fft_size / 2)) count = _config.fft_size / 2;
    memset(bins, 0, count * sizeof(float));
    return count;
}

bool is_speaking() {
    return _state.speaking;
}

AcousticState get_state() {
    return _state;
}

float get_level() {
    return _state.rms_level;
}

int get_summary_json(char* buf, size_t buf_size) {
    if (!_initialized) return snprintf(buf, buf_size, "{}");
    return snprintf(buf, buf_size,
        "{\"active\":true,\"rms\":%.3f,\"peak\":%.3f,"
        "\"peak_hz\":%.0f,\"speaking\":%s,\"class\":\"%s\"}",
        _state.rms_level, _state.peak_level,
        _state.peak_freq_hz,
        _state.speaking ? "true" : "false",
        _state.classification == SoundClass::SILENCE ? "silence" :
        _state.classification == SoundClass::SPEECH ? "speech" :
        _state.classification == SoundClass::MUSIC ? "music" :
        _state.classification == SoundClass::VEHICLE ? "vehicle" :
        _state.classification == SoundClass::ANIMAL ? "animal" :
        _state.classification == SoundClass::MECHANICAL ? "mechanical" :
        _state.classification == SoundClass::IMPACT ? "impact" : "unknown");
}

bool is_active() {
    return _initialized;
}

SoundClass classify(float* out_confidence) {
    if (!_initialized) {
        if (out_confidence) *out_confidence = 0.0f;
        return SoundClass::SILENCE;
    }

    // Rule-based classification from acoustic state
    // Real implementation would use FFT features
    SoundClass cls = _state.classification;
    float confidence = 0.0f;

    if (_state.rms_level < 0.01f) {
        cls = SoundClass::SILENCE;
        confidence = 0.95f;
    } else if (_state.speaking) {
        cls = SoundClass::SPEECH;
        confidence = 0.7f;
    } else if (_state.peak_level > 0.8f && _state.peak_freq_hz > 1000) {
        cls = SoundClass::IMPACT;
        confidence = 0.6f;
    } else if (_state.peak_freq_hz < 300 && _state.rms_level > 0.2f) {
        cls = SoundClass::VEHICLE;
        confidence = 0.5f;
    } else {
        cls = SoundClass::UNKNOWN;
        confidence = 0.3f;
    }

    _state.classification = cls;

    if (confidence >= _min_confidence && cls != SoundClass::SILENCE) {
        _event_count++;
        _event_counts[static_cast<uint8_t>(cls)]++;

        // Publish event via callback
        if (_event_cb) {
            char json[256];
            const char* cls_str =
                cls == SoundClass::SPEECH ? "speech" :
                cls == SoundClass::MUSIC ? "music" :
                cls == SoundClass::VEHICLE ? "vehicle" :
                cls == SoundClass::ANIMAL ? "animal" :
                cls == SoundClass::MECHANICAL ? "mechanical" :
                cls == SoundClass::IMPACT ? "impact" : "unknown";

            snprintf(json, sizeof(json),
                "{\"event_type\":\"%s\",\"confidence\":%.2f,"
                "\"rms\":%.3f,\"peak_hz\":%.0f,\"peak_db\":%.1f}",
                cls_str, confidence,
                _state.rms_level, _state.peak_freq_hz, _state.peak_level);

            _event_cb(json, cls, confidence);
        }
    }

    if (out_confidence) *out_confidence = confidence;
    return cls;
}

uint32_t get_event_count() {
    return _event_count;
}

uint32_t get_event_count_for(SoundClass cls) {
    uint8_t idx = static_cast<uint8_t>(cls);
    return idx < 8 ? _event_counts[idx] : 0;
}

void set_event_callback(event_callback_t cb) {
    _event_cb = cb;
}

void set_min_confidence(float threshold) {
    _min_confidence = threshold;
}

}  // namespace hal_acoustic
