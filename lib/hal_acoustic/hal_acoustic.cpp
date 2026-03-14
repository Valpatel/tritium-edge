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

}  // namespace hal_acoustic
