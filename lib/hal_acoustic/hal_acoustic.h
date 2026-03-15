// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Acoustic Sensor HAL — microphone capture, spectrum analysis, voice activity,
// and audio feature extraction for ML classification.
//
// Provides a high-level interface for acoustic sensing:
//   - Raw PCM sample capture from I2S microphone
//   - FFT spectrum analysis (magnitude bins)
//   - Voice Activity Detection (VAD)
//   - MFCC extraction (13 coefficients) for SC-side classification
//   - Spectral centroid, zero-crossing rate, spectral rolloff
//   - Sound classification hooks (gunshot, vehicle, animal, glass break)
//
// Audio features (not raw audio) are published to MQTT for SC-side
// ML classification, minimizing bandwidth while preserving discriminative info.
//
// Usage:
//   hal_acoustic::AcousticConfig cfg;
//   cfg.sample_rate = 16000;
//   cfg.fft_size = 256;
//   cfg.mfcc_enabled = true;
//   hal_acoustic::init(cfg);
//   // in loop:
//   hal_acoustic::tick();
//   if (hal_acoustic::is_speaking()) { ... }
//   float mfcc[13];
//   hal_acoustic::get_mfcc(mfcc, 13);

#include <cstdint>
#include <cstddef>

namespace hal_acoustic {

// Number of MFCC coefficients to extract
constexpr int MFCC_COEFFICIENTS = 13;

// Number of mel filter banks
constexpr int MEL_FILTERS = 26;

struct AcousticConfig {
    uint32_t sample_rate = 16000;       // Sample rate in Hz
    uint16_t fft_size = 256;            // FFT window size (power of 2)
    uint16_t capture_ms = 50;           // Capture window length in ms
    float    vad_threshold = 0.05f;     // Voice activity RMS threshold (0.0-1.0)
    float    vad_hangover_ms = 500;     // Keep speaking=true for this long after drop
    bool     auto_capture = true;       // Automatically capture in tick()
    bool     mfcc_enabled = true;       // Compute MFCCs on each capture
    uint16_t mfcc_count = MFCC_COEFFICIENTS; // Number of MFCC coefficients
};

// Sound classification categories
enum class SoundClass : uint8_t {
    SILENCE = 0,
    SPEECH,
    MUSIC,
    VEHICLE,
    ANIMAL,
    MECHANICAL,
    IMPACT,         // gunshot, glass break, explosion
    UNKNOWN
};

// Audio feature vector for MQTT publishing to SC
struct AudioFeatureVector {
    float    mfcc[MFCC_COEFFICIENTS];   // Mel-frequency cepstral coefficients
    float    spectral_centroid;          // Center of mass of spectrum (Hz)
    float    spectral_bandwidth;        // Spread around centroid (Hz)
    float    spectral_rolloff;          // Frequency below which 85% energy (Hz)
    float    spectral_flatness;         // Tonality measure (0.0-1.0)
    float    zero_crossing_rate;        // ZCR per sample (0.0-1.0)
    float    rms_energy;                // RMS energy (0.0-1.0)
    float    peak_amplitude;            // Peak amplitude (0.0-1.0)
    uint32_t duration_ms;               // Segment duration
    uint32_t sample_rate;               // Sample rate used
    bool     valid;                     // Whether features were computed
};

// Snapshot of current acoustic state
struct AcousticState {
    float    rms_level;                 // Current RMS level (0.0-1.0)
    float    peak_level;                // Peak level since last reset
    float    peak_freq_hz;             // Dominant frequency in Hz
    bool     speaking;                  // Voice activity detected
    uint32_t speaking_duration_ms;      // How long speaking has been true
    SoundClass classification;          // Current sound classification
    uint32_t last_capture_ms;           // millis() of last capture
};

// Initialize acoustic sensor. Call after AudioHAL is initialized.
// Returns true if microphone is available and ready.
bool init(const AcousticConfig& config = {});

// Shut down acoustic processing.
void shutdown();

// Call from main loop. Captures samples and runs analysis if auto_capture.
void tick();

// Get raw PCM samples from the last capture window.
// Copies up to max_samples into out[]. Returns actual count copied.
int get_samples(int16_t* out, int max_samples);

// Get FFT magnitude spectrum from the last capture.
// Copies up to max_bins magnitude values (0.0-1.0 normalized) into bins[].
// Returns actual number of bins written.
int get_spectrum(float* bins, int max_bins);

// Is voice activity currently detected?
bool is_speaking();

// Get full acoustic state snapshot.
AcousticState get_state();

// Get current RMS level (0.0-1.0). Quick check without full state.
float get_level();

// --- MFCC and Feature Extraction ---

// Get the latest MFCC coefficients. Copies up to max_count values.
// Returns number of coefficients written.
int get_mfcc(float* out, int max_count);

// Get the full audio feature vector from the last capture.
// This is the main data to publish to MQTT for SC-side ML classification.
AudioFeatureVector get_features();

// Get spectral centroid (Hz) from the last FFT.
float get_spectral_centroid();

// Get zero-crossing rate from the last capture (crossings per sample).
float get_zero_crossing_rate();

// Get spectral rolloff frequency (Hz) — frequency below which 85% energy.
float get_spectral_rolloff();

// Get spectral flatness (0.0-1.0) — geometric/arithmetic mean ratio.
float get_spectral_flatness();

// Get JSON summary for heartbeat/status reporting.
// Returns bytes written (excluding null terminator).
int get_summary_json(char* buf, size_t buf_size);

// Get JSON audio feature vector for MQTT publishing.
// Compact format with MFCC + spectral features for SC classification.
// Returns bytes written (excluding null terminator).
int get_features_json(char* buf, size_t buf_size);

// Is the acoustic sensor initialized and active?
bool is_active();

// --- Event classification (sound type detection) ---

// Classify current audio buffer into a SoundClass.
// Returns the classification and confidence (0.0-1.0).
SoundClass classify(float* out_confidence = nullptr);

// Get count of classified events since init.
uint32_t get_event_count();

// Get count of events by class.
uint32_t get_event_count_for(SoundClass cls);

// Set MQTT publish callback for acoustic events.
// Callback receives: event JSON string, event type, confidence.
typedef void (*event_callback_t)(const char* json, SoundClass cls, float confidence);
void set_event_callback(event_callback_t cb);

// Set callback for publishing audio feature vectors.
// Called after each MFCC extraction with the feature JSON string.
typedef void (*features_callback_t)(const char* json);
void set_features_callback(features_callback_t cb);

// Set minimum confidence threshold for event publishing (default 0.5).
void set_min_confidence(float threshold);

// --- NTP-synced TDoA timestamp support ---

// Timestamp info for TDoA computation. When NTP is synced, timestamps
// have microsecond precision for accurate time-of-arrival differences.
struct TDoATimestamp {
    uint64_t epoch_us;         // Microseconds since epoch (NTP-synced)
    float    sync_quality;     // 0.0 = no sync, 1.0 = <1ms jitter
    bool     ntp_synced;       // Whether NTP was synced when captured
    int32_t  estimated_drift_ms; // Estimated drift since last NTP sync
};

// Get a high-precision NTP-synced timestamp for TDoA computation.
// Returns epoch microseconds and sync quality. Call this at the instant
// an acoustic event is detected for accurate TDoA across nodes.
TDoATimestamp get_tdoa_timestamp();

// Get JSON for TDoA acoustic event publishing to MQTT.
// Includes NTP-synced microsecond timestamp and sync quality indicator.
// Format: {"sensor_id":"...", "arrival_time_ms":..., "signal_strength":...,
//          "event_type":"...", "confidence":..., "ntp_sync_quality":...,
//          "ntp_synced":true}
int get_tdoa_event_json(char* buf, size_t buf_size,
                        const char* sensor_id, const char* event_type,
                        float confidence, float signal_strength_db);

}  // namespace hal_acoustic
