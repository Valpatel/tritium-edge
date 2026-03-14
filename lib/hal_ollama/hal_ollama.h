// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// HAL Ollama — HTTP client for querying a local Ollama LLM instance.
//
// Enables on-device intelligence when an ESP32 board has WiFi access to a
// local Ollama server. The board sends simple classification prompts (e.g.,
// "Is this device pattern suspicious?") and receives short text responses.
//
// This is a stub for future integration. The actual HTTP request is
// performed using ESP-IDF's esp_http_client (or Arduino HTTPClient on
// Arduino framework).
//
// Configuration:
//   - endpoint: URL of the Ollama server (default: http://192.168.1.100:11434)
//   - model: Model name (default: "qwen2.5:0.5b" — smallest available)
//   - timeout_ms: Request timeout (default: 10000ms)
//
// Usage:
//   hal_ollama::OllamaConfig cfg;
//   cfg.endpoint = "http://192.168.1.100:11434";
//   cfg.model = "qwen2.5:0.5b";
//   hal_ollama::init(cfg);
//
//   char response[256];
//   if (hal_ollama::classify("BLE device appeared with random MAC", response, sizeof(response))) {
//       // response contains LLM classification
//   }

#include <cstdint>
#include <cstddef>

namespace hal_ollama {

static constexpr int MAX_PROMPT_LEN = 512;
static constexpr int MAX_RESPONSE_LEN = 1024;
static constexpr int DEFAULT_TIMEOUT_MS = 10000;

struct OllamaConfig {
    const char* endpoint = "http://192.168.1.100:11434";
    const char* model = "qwen2.5:0.5b";
    uint32_t timeout_ms = DEFAULT_TIMEOUT_MS;
    float temperature = 0.3f;
    int max_tokens = 100;
};

// Initialize the Ollama HAL with server configuration.
// Returns true if configuration is valid (does NOT test connectivity).
bool init(const OllamaConfig& config = {});

// Check if the Ollama HAL is initialized.
bool is_initialized();

// Test connectivity to the Ollama server.
// Sends a simple health check request. Returns true if server responds.
bool test_connection();

// Send a classification prompt to the Ollama server.
// Writes the response text to the output buffer.
// Returns true on success, false on timeout or error.
bool classify(const char* prompt, char* response, size_t response_size);

// Send a general prompt to the Ollama server.
// More flexible than classify — allows custom system prompts.
bool generate(
    const char* prompt,
    const char* system_prompt,
    char* response,
    size_t response_size
);

// Get the last error message (empty string if no error).
const char* get_last_error();

// Get stats: request count, success count, average latency
struct OllamaStats {
    uint32_t requests;
    uint32_t successes;
    uint32_t failures;
    uint32_t avg_latency_ms;
};
OllamaStats get_stats();

}  // namespace hal_ollama
