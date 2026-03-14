// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Meshtastic Serial Bridge HAL
//
// Provides a high-level interface for communicating with an external
// Meshtastic radio connected via serial (UART).  The Meshtastic device
// must be configured with serial.enabled=true and serial.mode=TEXTMSG.
//
// This is a standalone HAL that wraps the low-level serial bridge from
// hal_lora into a cleaner, service-friendly interface with callbacks.
//
// Usage:
//   #include "hal_meshtastic.h"
//   hal_meshtastic::MeshtasticConfig cfg;
//   cfg.uart_num = 1;
//   cfg.rx_pin = 18;
//   cfg.tx_pin = 17;
//   hal_meshtastic::init(cfg);
//   hal_meshtastic::on_message([](const char* text, uint32_t from) {
//       Serial.printf("Mesh msg from %08X: %s\n", from, text);
//   });
//   // in loop():
//   hal_meshtastic::tick();

#include <cstdint>
#include <cstddef>

namespace hal_meshtastic {

// Maximum text message length
static constexpr size_t MAX_MSG_LEN = 237;
// Maximum nodes we track
static constexpr int MAX_NODES = 32;

struct MeshtasticConfig {
    int uart_num = 1;            // UART peripheral (0, 1, or 2)
    int rx_pin = -1;             // RX GPIO pin (-1 = not connected)
    int tx_pin = -1;             // TX GPIO pin (-1 = not connected)
    uint32_t baud = 115200;      // Baud rate
    bool auto_poll = true;       // Auto-poll for messages in tick()
};

struct MeshtasticNode {
    uint32_t node_id;
    char short_name[8];
    char long_name[40];
    int8_t rssi;
    float snr;
    bool has_gps;
    double latitude;
    double longitude;
    float altitude;
    uint32_t last_heard_ms;      // millis() of last contact
};

// Callback for incoming text messages
// text: the message content (null-terminated)
// from_node: sender node ID (0xFFFFFFFF = broadcast)
typedef void (*MessageCallback)(const char* text, uint32_t from_node);

// Initialize the Meshtastic serial bridge.
// Returns true if the serial port was opened successfully.
bool init(const MeshtasticConfig& config = {});

// Shut down the serial bridge and release resources.
void shutdown();

// Call from loop(). Polls serial for incoming messages, invokes callbacks.
void tick();

// Check if the bridge is initialized and the serial link appears active.
bool is_connected();

// Check if init() has been called successfully.
bool is_initialized();

// ── Sending ──────────────────────────────────────────────────────────────

// Send a text message to the mesh.
// dest: destination node ID (0xFFFFFFFF = broadcast, the default).
// Returns true if the message was queued for sending.
bool send_text(const char* text, uint32_t dest = 0xFFFFFFFF);

// ── Receiving ────────────────────────────────────────────────────────────

// Register a callback for incoming messages.
// Only one callback is supported; calling again replaces the previous one.
void on_message(MessageCallback callback);

// Get the last received message text, or nullptr if none.
const char* get_last_message();

// Get the millis() timestamp of the last received message.
uint32_t get_last_message_time();

// ── Node tracking ────────────────────────────────────────────────────────

// Get the list of known mesh nodes.
// Writes up to max_count nodes into the output array.
// Returns the number of nodes written.
int get_nodes(MeshtasticNode* out, int max_count);

// Get node count.
int get_node_count();

// Serialize node list to JSON array.
// Returns bytes written (excluding null terminator), or 0 on error.
int get_nodes_json(char* buf, size_t size);

// ── Stats ────────────────────────────────────────────────────────────────

uint32_t get_messages_sent();
uint32_t get_messages_received();
uint32_t get_uptime_ms();

// ── Test harness ─────────────────────────────────────────────────────────

struct TestResult {
    bool init_ok;
    bool send_ok;
    int node_count;
    uint32_t messages_sent;
    uint32_t messages_received;
    const char* status;
    uint32_t test_duration_ms;
};
TestResult runTest();

}  // namespace hal_meshtastic
