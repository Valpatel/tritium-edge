// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

// TinyML Inference HAL — STUB implementation
//
// This stub provides the full API surface for model loading and inference
// but returns synthetic results. When TFLite Micro is integrated, only
// load_model() and run_inference() need real implementations.
//
// The stub allows:
// - SC to push models to edge devices via MQTT/OTA
// - Edge firmware to enumerate and "load" models from SD
// - Inference pipeline to be wired up end-to-end
// - Real TFLite integration to be dropped in later

#include "hal_tinyml.h"
#include <cstring>
#include <cmath>

#if __has_include(<Arduino.h>)
#include <Arduino.h>
#define TINYML_LOG(fmt, ...) Serial.printf("[TinyML] " fmt "\n", ##__VA_ARGS__)
#else
#include <cstdio>
#define TINYML_LOG(fmt, ...) printf("[TinyML] " fmt "\n", ##__VA_ARGS__)
#endif

namespace hal_tinyml {

// --- Internal state ---

static bool s_initialized = false;
static TinyMLConfig s_config;
static ModelInfo s_model;
static bool s_model_loaded = false;
static InferenceResult s_last_result;

// Stub: available model names (simulated SD card contents)
static constexpr int STUB_MODEL_COUNT = 3;
static const char* STUB_MODEL_NAMES[STUB_MODEL_COUNT] = {
    "ble_classifier",
    "correlation",
    "anomaly_detector",
};

// --- Lifecycle ---

bool init(const TinyMLConfig& config) {
    if (s_initialized) return true;

    s_config = config;
    s_initialized = true;
    s_model_loaded = false;
    memset(&s_model, 0, sizeof(s_model));
    memset(&s_last_result, 0, sizeof(s_last_result));

    TINYML_LOG("Initialized (stub mode, model_dir=%s, arena=%zuB)",
               config.model_dir, config.tensor_arena);
    return true;
}

void deinit() {
    if (!s_initialized) return;
    unload_model();
    s_initialized = false;
    TINYML_LOG("Deinitialized");
}

bool is_initialized() {
    return s_initialized;
}

// --- Model Management ---

bool load_model(const char* name) {
    if (!s_initialized) {
        TINYML_LOG("Not initialized");
        return false;
    }
    if (!name || strlen(name) == 0) {
        TINYML_LOG("Empty model name");
        return false;
    }

    // Unload any existing model
    unload_model();

    // Stub: pretend to load from SD card
    strncpy(s_model.name, name, MAX_MODEL_NAME - 1);
    s_model.name[MAX_MODEL_NAME - 1] = '\0';
    s_model.format = ModelFormat::TFLITE_MICRO;
    s_model.loaded = true;
    s_model.last_inference_ms = 0.0f;

    // Set stub sizes based on model name
    if (strcmp(name, "ble_classifier") == 0) {
        s_model.input_size = 13;   // BLE feature vector
        s_model.output_size = 19;  // Device types
        s_model.size_bytes = 24576;
    } else if (strcmp(name, "correlation") == 0) {
        s_model.input_size = 6;    // Correlation features
        s_model.output_size = 2;   // Correlated / not correlated
        s_model.size_bytes = 8192;
    } else if (strcmp(name, "anomaly_detector") == 0) {
        s_model.input_size = 16;   // RF environment features
        s_model.output_size = 1;   // Anomaly score
        s_model.size_bytes = 16384;
    } else {
        // Generic model
        s_model.input_size = MAX_INPUT_SIZE;
        s_model.output_size = MAX_OUTPUT_SIZE;
        s_model.size_bytes = 32768;
    }

    s_model_loaded = true;
    TINYML_LOG("Loaded model '%s' (stub, %zu bytes, in=%d, out=%d)",
               name, s_model.size_bytes, s_model.input_size, s_model.output_size);
    return true;
}

void unload_model() {
    if (!s_model_loaded) return;
    TINYML_LOG("Unloaded model '%s'", s_model.name);
    memset(&s_model, 0, sizeof(s_model));
    s_model_loaded = false;
}

const ModelInfo* get_model_info() {
    if (!s_model_loaded) return nullptr;
    return &s_model;
}

bool model_exists(const char* name) {
    if (!name) return false;
    // Stub: check against known model names
    for (int i = 0; i < STUB_MODEL_COUNT; i++) {
        if (strcmp(name, STUB_MODEL_NAMES[i]) == 0) return true;
    }
    return false;
}

// --- Inference ---

bool run_inference(const float* input, int input_size, InferenceResult& result) {
    if (!s_initialized || !s_model_loaded) {
        result.success = false;
        return false;
    }
    if (!input || input_size <= 0) {
        result.success = false;
        return false;
    }

    // Stub inference: generate deterministic outputs from inputs
    // Real implementation will use TFLite Micro interpreter here
    result.success = true;
    result.output_count = s_model.output_size;

    // Generate stub outputs using a simple hash of inputs
    float sum = 0.0f;
    for (int i = 0; i < input_size && i < MAX_INPUT_SIZE; i++) {
        sum += input[i] * (i + 1);
    }

    // Distribute probability across outputs using softmax-like
    float total = 0.0f;
    for (int i = 0; i < result.output_count && i < MAX_OUTPUT_SIZE; i++) {
        // Deterministic pseudo-random based on input sum and index
        float raw = sinf(sum * 0.1f + i * 1.5f) * 0.5f + 0.5f;
        result.outputs[i] = raw;
        total += raw;
    }

    // Normalize to sum to 1.0 (like softmax output)
    if (total > 0.0f) {
        for (int i = 0; i < result.output_count; i++) {
            result.outputs[i] /= total;
        }
    }

    // Find predicted class and confidence
    result.predicted_class = argmax(result.outputs, result.output_count);
    result.confidence = (result.predicted_class >= 0)
        ? result.outputs[result.predicted_class]
        : 0.0f;

    // Stub timing (real implementation would measure actual inference time)
    result.inference_ms = 2.5f;  // Simulated 2.5ms inference
    s_model.last_inference_ms = result.inference_ms;

    if (s_config.log_timing) {
        TINYML_LOG("Inference on '%s': class=%d conf=%.3f time=%.1fms",
                   s_model.name, result.predicted_class,
                   result.confidence, result.inference_ms);
    }

    s_last_result = result;
    return true;
}

int predict_class(const float* input, int input_size) {
    InferenceResult result;
    if (!run_inference(input, input_size, result)) return -1;
    return result.predicted_class;
}

float get_confidence() {
    return s_last_result.confidence;
}

// --- Utilities ---

int argmax(const float* arr, int size) {
    if (!arr || size <= 0) return -1;
    int best = 0;
    for (int i = 1; i < size; i++) {
        if (arr[i] > arr[best]) best = i;
    }
    return best;
}

int count_available_models() {
    if (!s_initialized) return 0;
    // Stub: return count of known model names
    return STUB_MODEL_COUNT;
}

const char* get_available_model_name(int index) {
    if (index < 0 || index >= STUB_MODEL_COUNT) return nullptr;
    return STUB_MODEL_NAMES[index];
}

}  // namespace hal_tinyml
