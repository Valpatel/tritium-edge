// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Acoustic Sensor HAL — microphone capture, spectrum analysis, voice activity
//
// Provides a high-level interface for acoustic sensing:
//   - Raw PCM sample capture from I2S microphone
//   - FFT spectrum analysis (magnitude bins)
//   - Voice Activity Detection (VAD)
//   - Sound classification hooks (future: gunshot, vehicle, animal, glass break)
//
// This HAL wraps the lower-level AudioHAL codec driver and adds
// analysis capabilities for the Tritium sensor fusion pipeline.
//
// Usage:
//   hal_acoustic::AcousticConfig cfg;
//   cfg.sample_rate = 16000;
//   cfg.fft_size = 256;
//   hal_acoustic::init(cfg);
//   // in loop:
//   hal_acoustic::tick();
//   if (hal_acoustic::is_speaking()) { ... }
//   float bins[128];
//   hal_acoustic::get_spectrum(bins, 128);

#include <cstdint>
#include <cstddef>

namespace hal_acoustic {

struct AcousticConfig {
    uint32_t sample_rate = 16000;       // Sample rate in Hz
    uint16_t fft_size = 256;            // FFT window size (power of 2)
    uint16_t capture_ms = 50;           // Capture window length in ms
    float    vad_threshold = 0.05f;     // Voice activity RMS threshold (0.0-1.0)
    float    vad_hangover_ms = 500;     // Keep speaking=true for this long after drop
    bool     auto_capture = true;       // Automatically capture in tick()
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

// Get JSON summary for heartbeat/status reporting.
// Returns bytes written (excluding null terminator).
int get_summary_json(char* buf, size_t buf_size);

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

// Set minimum confidence threshold for event publishing (default 0.5).
void set_min_confidence(float threshold);

}  // namespace hal_acoustic
