// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// TinyML Inference HAL — loads quantized models from SD card and runs
// inference on sensor data. Stub implementation for now — real inference
// when TFLite Micro is integrated.
//
// Supports TFLite Micro format (.tflite) models stored on the SD card
// at /models/<name>.tflite. Models are loaded into PSRAM for inference.
//
// Usage:
//   hal_tinyml::init({.model_dir = "/sd/models"});
//   if (hal_tinyml::load_model("ble_classifier")) {
//       float input[13] = { ... };
//       float output[19];
//       if (hal_tinyml::run_inference(input, 13, output, 19)) {
//           int best = hal_tinyml::argmax(output, 19);
//       }
//   }

#include <cstdint>
#include <cstddef>

namespace hal_tinyml {

static constexpr size_t MAX_MODEL_SIZE = 512 * 1024;    // 512KB max model
static constexpr size_t MAX_TENSOR_ARENA = 128 * 1024;  // 128KB tensor arena
static constexpr int MAX_INPUT_SIZE  = 256;
static constexpr int MAX_OUTPUT_SIZE = 64;
static constexpr int MAX_MODEL_NAME  = 64;

enum class ModelFormat : uint8_t {
    TFLITE_MICRO = 0,   // TensorFlow Lite Micro (.tflite)
    ONNX_MICRO   = 1,   // ONNX Micro (future)
    CUSTOM       = 2,   // Custom binary format (future)
};

struct TinyMLConfig {
    const char* model_dir      = "/sd/models";  // Directory on SD card
    size_t      tensor_arena   = MAX_TENSOR_ARENA;
    bool        use_psram      = true;           // Allocate in PSRAM
    bool        log_timing     = false;          // Log inference timing
};

struct ModelInfo {
    char        name[MAX_MODEL_NAME];
    ModelFormat format;
    size_t      size_bytes;
    int         input_size;      // Number of input elements
    int         output_size;     // Number of output elements
    bool        loaded;
    float       last_inference_ms;
};

struct InferenceResult {
    bool    success;
    float   outputs[MAX_OUTPUT_SIZE];
    int     output_count;
    float   inference_ms;      // Time taken for inference
    int     predicted_class;   // argmax of outputs (-1 if not applicable)
    float   confidence;        // max output value
};

// --- Lifecycle ---

/// Initialize the TinyML subsystem. Call once at boot.
/// Returns true if initialization succeeded (even if no models are loaded yet).
bool init(const TinyMLConfig& config = {});

/// Shut down and free all resources.
void deinit();

/// Returns true if the subsystem is initialized.
bool is_initialized();

// --- Model Management ---

/// Load a model from SD card by name (without extension).
/// Looks for <model_dir>/<name>.tflite
/// Returns true if the model was loaded (or stubbed) successfully.
bool load_model(const char* name);

/// Unload the currently loaded model and free memory.
void unload_model();

/// Get info about the currently loaded model.
/// Returns nullptr if no model is loaded.
const ModelInfo* get_model_info();

/// Check if a model file exists on the SD card.
bool model_exists(const char* name);

// --- Inference ---

/// Run inference on the loaded model.
/// @param input  Input feature vector (float array).
/// @param input_size  Number of input elements.
/// @param result  Output result struct.
/// @return true if inference succeeded (even if stubbed).
bool run_inference(const float* input, int input_size, InferenceResult& result);

/// Convenience: run inference and return just the predicted class index.
/// Returns -1 on failure.
int predict_class(const float* input, int input_size);

/// Convenience: get the confidence (max output value) from last inference.
float get_confidence();

// --- Utilities ---

/// Compute argmax of a float array.
int argmax(const float* arr, int size);

/// Get the number of models available on SD card.
int count_available_models();

/// Get the name of the n-th available model (for enumeration).
/// Returns nullptr if index is out of range.
const char* get_available_model_name(int index);

}  // namespace hal_tinyml
