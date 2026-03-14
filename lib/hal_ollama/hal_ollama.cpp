// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_ollama.h"
#include "debug_log.h"

#include <cstring>
#include <cstdio>

static constexpr const char* TAG = "ollama";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace hal_ollama {

bool init(const OllamaConfig&) { return false; }
bool is_initialized() { return false; }
bool test_connection() { return false; }
bool classify(const char*, char* response, size_t size) {
    if (size > 0) response[0] = '\0';
    return false;
}
bool generate(const char*, const char*, char* response, size_t size) {
    if (size > 0) response[0] = '\0';
    return false;
}
const char* get_last_error() { return "simulator stub"; }
OllamaStats get_stats() { return {}; }

}  // namespace hal_ollama

// ============================================================================
// ESP32 — HTTP client to Ollama REST API
// ============================================================================
#else

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>

namespace hal_ollama {

static OllamaConfig s_config;
static bool s_initialized = false;
static char s_last_error[128] = "";
static OllamaStats s_stats = {};

bool init(const OllamaConfig& config) {
    s_config = config;
    s_initialized = true;
    s_stats = {};
    s_last_error[0] = '\0';
    DEBUG_LOG(TAG, "Initialized: endpoint=%s model=%s",
              s_config.endpoint, s_config.model);
    return true;
}

bool is_initialized() {
    return s_initialized;
}

bool test_connection() {
    if (!s_initialized) {
        snprintf(s_last_error, sizeof(s_last_error), "not initialized");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        snprintf(s_last_error, sizeof(s_last_error), "WiFi not connected");
        return false;
    }

    HTTPClient http;
    char url[256];
    snprintf(url, sizeof(url), "%s/api/tags", s_config.endpoint);

    http.begin(url);
    http.setTimeout(s_config.timeout_ms);
    int code = http.GET();
    http.end();

    if (code == 200) {
        s_last_error[0] = '\0';
        return true;
    }

    snprintf(s_last_error, sizeof(s_last_error), "HTTP %d", code);
    return false;
}

bool classify(const char* prompt, char* response, size_t response_size) {
    // Classification uses a fixed system prompt
    const char* sys = "You are a device classifier for an IoT security system. "
                      "Respond with a single word or short phrase classification.";
    return generate(prompt, sys, response, response_size);
}

bool generate(
    const char* prompt,
    const char* system_prompt,
    char* response,
    size_t response_size
) {
    if (response_size > 0) response[0] = '\0';

    if (!s_initialized) {
        snprintf(s_last_error, sizeof(s_last_error), "not initialized");
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        snprintf(s_last_error, sizeof(s_last_error), "WiFi not connected");
        return false;
    }

    s_stats.requests++;
    uint32_t start_ms = millis();

    HTTPClient http;
    char url[256];
    snprintf(url, sizeof(url), "%s/api/generate", s_config.endpoint);

    http.begin(url);
    http.setTimeout(s_config.timeout_ms);
    http.addHeader("Content-Type", "application/json");

    // Build JSON payload — minimal, no ArduinoJson dependency
    // We truncate the prompt if too long to fit in our buffer
    char payload[MAX_PROMPT_LEN + 512];
    int written = snprintf(payload, sizeof(payload),
        "{\"model\":\"%s\",\"prompt\":\"%s\","
        "\"system\":\"%s\","
        "\"stream\":false,"
        "\"options\":{\"temperature\":%.1f,\"num_predict\":%d}}",
        s_config.model,
        prompt,
        system_prompt ? system_prompt : "",
        s_config.temperature,
        s_config.max_tokens);

    if (written < 0 || (size_t)written >= sizeof(payload)) {
        snprintf(s_last_error, sizeof(s_last_error), "payload too large");
        s_stats.failures++;
        http.end();
        return false;
    }

    int code = http.POST(payload);
    uint32_t elapsed = millis() - start_ms;

    if (code != 200) {
        snprintf(s_last_error, sizeof(s_last_error), "HTTP %d", code);
        s_stats.failures++;
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    // Extract "response" field from JSON (simple parser, no ArduinoJson)
    // Format: {"model":"...","response":"THE TEXT WE WANT","...}
    int idx = body.indexOf("\"response\":\"");
    if (idx < 0) {
        snprintf(s_last_error, sizeof(s_last_error), "no response field");
        s_stats.failures++;
        return false;
    }

    idx += 12;  // skip past "response":"
    int end = body.indexOf("\"", idx);
    if (end < 0) end = body.length();

    int len = end - idx;
    if (len >= (int)response_size) len = response_size - 1;

    body.substring(idx, idx + len).toCharArray(response, response_size);
    response[len] = '\0';

    s_stats.successes++;
    // Running average latency
    s_stats.avg_latency_ms = (s_stats.avg_latency_ms * (s_stats.successes - 1) + elapsed) / s_stats.successes;

    s_last_error[0] = '\0';
    DEBUG_LOG(TAG, "Response (%lums): %.40s...", elapsed, response);
    return true;
}

const char* get_last_error() {
    return s_last_error;
}

OllamaStats get_stats() {
    return s_stats;
}

}  // namespace hal_ollama

#endif
