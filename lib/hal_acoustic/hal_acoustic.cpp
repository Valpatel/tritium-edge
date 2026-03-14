// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_acoustic.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// ============================================================================
// Stub + MFCC feature extraction implementation.
// Audio capture is stubbed (no real I2S in this build), but the feature
// extraction math is real — when real audio samples are available, the
// same code computes actual MFCCs, spectral centroid, ZCR, etc.
// ============================================================================

namespace hal_acoustic {

static bool _initialized = false;
static AcousticConfig _config;
static AcousticState _state = {};
static event_callback_t _event_cb = nullptr;
static features_callback_t _features_cb = nullptr;
static float _min_confidence = 0.5f;
static uint32_t _event_count = 0;
static uint32_t _event_counts[8] = {};  // one per SoundClass

// Feature extraction state
static AudioFeatureVector _features = {};
static float _mfcc_coeffs[MFCC_COEFFICIENTS] = {};
static float _spectral_centroid = 0.0f;
static float _zero_crossing_rate = 0.0f;
static float _spectral_rolloff = 0.0f;
static float _spectral_flatness = 0.0f;

// Internal sample buffer for feature extraction
static constexpr int MAX_SAMPLES = 1024;
static int16_t _sample_buf[MAX_SAMPLES] = {};
static int _sample_count = 0;

// Internal FFT magnitude buffer
static constexpr int MAX_FFT_BINS = 512;
static float _fft_bins[MAX_FFT_BINS] = {};
static int _fft_bin_count = 0;


// --- Mel frequency conversion ---

static float hz_to_mel(float hz) {
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

static float mel_to_hz(float mel) {
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}


// --- Compute MFCCs from FFT magnitude spectrum ---
// Simplified MFCC: mel filter bank -> log energy -> DCT-II
// This runs on ESP32 without external deps.

static void compute_mfcc(const float* fft_mag, int n_bins,
                         float sample_rate, float* out_mfcc, int n_mfcc) {
    if (n_bins <= 0 || n_mfcc <= 0) {
        memset(out_mfcc, 0, n_mfcc * sizeof(float));
        return;
    }

    // Mel filter bank energies
    float mel_energies[MEL_FILTERS] = {};
    float freq_per_bin = sample_rate / (2.0f * n_bins);
    float mel_low = hz_to_mel(0.0f);
    float mel_high = hz_to_mel(sample_rate / 2.0f);
    float mel_step = (mel_high - mel_low) / (MEL_FILTERS + 1);

    for (int m = 0; m < MEL_FILTERS; m++) {
        float mel_center = mel_low + (m + 1) * mel_step;
        float mel_left = mel_center - mel_step;
        float mel_right = mel_center + mel_step;

        float hz_center = mel_to_hz(mel_center);
        float hz_left = mel_to_hz(mel_left);
        float hz_right = mel_to_hz(mel_right);

        float energy = 0.0f;
        for (int k = 0; k < n_bins; k++) {
            float freq = k * freq_per_bin;
            float weight = 0.0f;

            if (freq >= hz_left && freq <= hz_center) {
                float denom = hz_center - hz_left;
                weight = (denom > 0.001f) ? (freq - hz_left) / denom : 0.0f;
            } else if (freq > hz_center && freq <= hz_right) {
                float denom = hz_right - hz_center;
                weight = (denom > 0.001f) ? (hz_right - freq) / denom : 0.0f;
            }

            energy += fft_mag[k] * fft_mag[k] * weight;
        }

        // Log energy (add small value to avoid log(0))
        mel_energies[m] = logf(energy + 1e-10f);
    }

    // DCT-II to get cepstral coefficients
    for (int i = 0; i < n_mfcc && i < MEL_FILTERS; i++) {
        float sum = 0.0f;
        for (int j = 0; j < MEL_FILTERS; j++) {
            sum += mel_energies[j] * cosf(
                (float)M_PI * (float)i * ((float)j + 0.5f) / (float)MEL_FILTERS
            );
        }
        out_mfcc[i] = sum;
    }
}


// --- Compute spectral centroid from FFT magnitudes ---

static float compute_spectral_centroid(const float* fft_mag, int n_bins,
                                        float sample_rate) {
    if (n_bins <= 0) return 0.0f;
    float freq_per_bin = sample_rate / (2.0f * n_bins);

    float weighted_sum = 0.0f;
    float total_mag = 0.0f;
    for (int k = 0; k < n_bins; k++) {
        float freq = k * freq_per_bin;
        weighted_sum += freq * fft_mag[k];
        total_mag += fft_mag[k];
    }

    return (total_mag > 1e-10f) ? weighted_sum / total_mag : 0.0f;
}


// --- Compute spectral bandwidth ---

static float compute_spectral_bandwidth(const float* fft_mag, int n_bins,
                                         float sample_rate, float centroid) {
    if (n_bins <= 0) return 0.0f;
    float freq_per_bin = sample_rate / (2.0f * n_bins);

    float weighted_sum = 0.0f;
    float total_mag = 0.0f;
    for (int k = 0; k < n_bins; k++) {
        float freq = k * freq_per_bin;
        float diff = freq - centroid;
        weighted_sum += diff * diff * fft_mag[k];
        total_mag += fft_mag[k];
    }

    return (total_mag > 1e-10f) ? sqrtf(weighted_sum / total_mag) : 0.0f;
}


// --- Compute zero-crossing rate from PCM samples ---

static float compute_zcr(const int16_t* samples, int count) {
    if (count < 2) return 0.0f;
    int crossings = 0;
    for (int i = 1; i < count; i++) {
        if ((samples[i] >= 0 && samples[i - 1] < 0) ||
            (samples[i] < 0 && samples[i - 1] >= 0)) {
            crossings++;
        }
    }
    return (float)crossings / (float)(count - 1);
}


// --- Compute RMS energy from PCM samples ---

static float compute_rms(const int16_t* samples, int count) {
    if (count <= 0) return 0.0f;
    float sum_sq = 0.0f;
    for (int i = 0; i < count; i++) {
        float s = (float)samples[i] / 32768.0f;
        sum_sq += s * s;
    }
    return sqrtf(sum_sq / (float)count);
}


// --- Compute spectral rolloff (frequency below which 85% of energy) ---

static float compute_spectral_rolloff(const float* fft_mag, int n_bins,
                                       float sample_rate, float rolloff_pct = 0.85f) {
    if (n_bins <= 0) return 0.0f;
    float freq_per_bin = sample_rate / (2.0f * n_bins);

    float total_energy = 0.0f;
    for (int k = 0; k < n_bins; k++) {
        total_energy += fft_mag[k] * fft_mag[k];
    }

    float threshold = total_energy * rolloff_pct;
    float cumulative = 0.0f;
    for (int k = 0; k < n_bins; k++) {
        cumulative += fft_mag[k] * fft_mag[k];
        if (cumulative >= threshold) {
            return k * freq_per_bin;
        }
    }
    return n_bins * freq_per_bin;
}


// --- Compute spectral flatness (geometric/arithmetic mean ratio) ---

static float compute_spectral_flatness(const float* fft_mag, int n_bins) {
    if (n_bins <= 0) return 0.0f;

    float log_sum = 0.0f;
    float arith_sum = 0.0f;
    int valid = 0;
    for (int k = 0; k < n_bins; k++) {
        float mag = fft_mag[k];
        if (mag > 1e-10f) {
            log_sum += logf(mag);
            arith_sum += mag;
            valid++;
        }
    }

    if (valid == 0 || arith_sum <= 1e-10f) return 0.0f;

    float geo_mean = expf(log_sum / (float)valid);
    float arith_mean = arith_sum / (float)valid;

    return geo_mean / arith_mean;
}


// --- Extract all features from current buffers ---

static void extract_features() {
    float sr = (float)_config.sample_rate;
    int n_bins = _fft_bin_count;

    // MFCC
    if (_config.mfcc_enabled && n_bins > 0) {
        compute_mfcc(_fft_bins, n_bins, sr, _mfcc_coeffs, _config.mfcc_count);
    }

    // Spectral features
    _spectral_centroid = compute_spectral_centroid(_fft_bins, n_bins, sr);
    float bandwidth = compute_spectral_bandwidth(_fft_bins, n_bins, sr, _spectral_centroid);
    _spectral_rolloff = compute_spectral_rolloff(_fft_bins, n_bins, sr);
    _spectral_flatness = compute_spectral_flatness(_fft_bins, n_bins);

    // Time-domain features
    _zero_crossing_rate = compute_zcr(_sample_buf, _sample_count);
    float rms = compute_rms(_sample_buf, _sample_count);

    // Peak amplitude
    float peak = 0.0f;
    for (int i = 0; i < _sample_count; i++) {
        float s = fabsf((float)_sample_buf[i] / 32768.0f);
        if (s > peak) peak = s;
    }

    // Build feature vector
    memcpy(_features.mfcc, _mfcc_coeffs, sizeof(_features.mfcc));
    _features.spectral_centroid = _spectral_centroid;
    _features.spectral_bandwidth = bandwidth;
    _features.spectral_rolloff = _spectral_rolloff;
    _features.spectral_flatness = _spectral_flatness;
    _features.zero_crossing_rate = _zero_crossing_rate;
    _features.rms_energy = rms;
    _features.peak_amplitude = peak;
    _features.duration_ms = _config.capture_ms;
    _features.sample_rate = _config.sample_rate;
    _features.valid = true;

    // Update state
    _state.rms_level = rms;
    _state.peak_level = peak;
    _state.peak_freq_hz = _spectral_centroid;

    // Publish features via callback
    if (_features_cb && _features.valid) {
        char json[512];
        int n = get_features_json(json, sizeof(json));
        if (n > 0) {
            _features_cb(json);
        }
    }
}


// ============================================================================
// Public API implementation
// ============================================================================

bool init(const AcousticConfig& config) {
    if (_initialized) return true;
    _config = config;
    memset(&_state, 0, sizeof(_state));
    memset(&_features, 0, sizeof(_features));
    memset(_mfcc_coeffs, 0, sizeof(_mfcc_coeffs));
    _state.classification = SoundClass::SILENCE;
    _sample_count = 0;
    _fft_bin_count = 0;
    _initialized = true;
    return true;
}

void shutdown() {
    _initialized = false;
}

void tick() {
    if (!_initialized) return;
    // Stub: no real I2S capture — feature extraction runs on whatever
    // samples are in the buffer (filled externally or via test injection).
    // In real impl: capture I2S samples -> run FFT -> extract features.

    if (_config.auto_capture && _sample_count > 0 && _fft_bin_count > 0) {
        extract_features();
    }
}

int get_samples(int16_t* out, int max_samples) {
    if (!out || max_samples <= 0) return 0;
    int count = (_sample_count < max_samples) ? _sample_count : max_samples;
    memcpy(out, _sample_buf, count * sizeof(int16_t));
    return count;
}

int get_spectrum(float* bins, int max_bins) {
    if (!bins || max_bins <= 0) return 0;
    int count = max_bins;
    if (count > _fft_bin_count) count = _fft_bin_count;
    if (count > (int)(_config.fft_size / 2)) count = _config.fft_size / 2;
    if (count <= 0) {
        // Return zeroed spectrum
        count = (max_bins < (int)(_config.fft_size / 2)) ? max_bins : _config.fft_size / 2;
        memset(bins, 0, count * sizeof(float));
        return count;
    }
    memcpy(bins, _fft_bins, count * sizeof(float));
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

int get_mfcc(float* out, int max_count) {
    if (!out || max_count <= 0) return 0;
    int count = (max_count < MFCC_COEFFICIENTS) ? max_count : MFCC_COEFFICIENTS;
    memcpy(out, _mfcc_coeffs, count * sizeof(float));
    return count;
}

AudioFeatureVector get_features() {
    return _features;
}

float get_spectral_centroid() {
    return _spectral_centroid;
}

float get_zero_crossing_rate() {
    return _zero_crossing_rate;
}

float get_spectral_rolloff() {
    return _spectral_rolloff;
}

float get_spectral_flatness() {
    return _spectral_flatness;
}

int get_summary_json(char* buf, size_t buf_size) {
    if (!_initialized) return snprintf(buf, buf_size, "{}");
    return snprintf(buf, buf_size,
        "{\"active\":true,\"rms\":%.3f,\"peak\":%.3f,"
        "\"peak_hz\":%.0f,\"speaking\":%s,\"class\":\"%s\","
        "\"mfcc_enabled\":%s,\"zcr\":%.4f,\"centroid\":%.0f}",
        _state.rms_level, _state.peak_level,
        _state.peak_freq_hz,
        _state.speaking ? "true" : "false",
        _state.classification == SoundClass::SILENCE ? "silence" :
        _state.classification == SoundClass::SPEECH ? "speech" :
        _state.classification == SoundClass::MUSIC ? "music" :
        _state.classification == SoundClass::VEHICLE ? "vehicle" :
        _state.classification == SoundClass::ANIMAL ? "animal" :
        _state.classification == SoundClass::MECHANICAL ? "mechanical" :
        _state.classification == SoundClass::IMPACT ? "impact" : "unknown",
        _config.mfcc_enabled ? "true" : "false",
        _zero_crossing_rate, _spectral_centroid);
}

int get_features_json(char* buf, size_t buf_size) {
    if (!_initialized || !_features.valid) {
        return snprintf(buf, buf_size, "{}");
    }

    // Compact MFCC array
    char mfcc_str[256] = {};
    int pos = 0;
    pos += snprintf(mfcc_str + pos, sizeof(mfcc_str) - pos, "[");
    for (int i = 0; i < MFCC_COEFFICIENTS; i++) {
        if (i > 0) pos += snprintf(mfcc_str + pos, sizeof(mfcc_str) - pos, ",");
        pos += snprintf(mfcc_str + pos, sizeof(mfcc_str) - pos, "%.2f", _features.mfcc[i]);
    }
    pos += snprintf(mfcc_str + pos, sizeof(mfcc_str) - pos, "]");

    return snprintf(buf, buf_size,
        "{\"mfcc\":%s,\"sc\":%.0f,\"sb\":%.0f,\"sr\":%.0f,"
        "\"sf\":%.4f,\"zcr\":%.4f,\"rms\":%.4f,\"pa\":%.4f,"
        "\"dur\":%u,\"hz\":%u}",
        mfcc_str,
        _features.spectral_centroid,
        _features.spectral_bandwidth,
        _features.spectral_rolloff,
        _features.spectral_flatness,
        _features.zero_crossing_rate,
        _features.rms_energy,
        _features.peak_amplitude,
        _features.duration_ms,
        _features.sample_rate);
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

void set_features_callback(features_callback_t cb) {
    _features_cb = cb;
}

void set_min_confidence(float threshold) {
    _min_confidence = threshold;
}

}  // namespace hal_acoustic
