// Tritium-OS WebSocket Bridge — real-time event streaming, serial terminal,
// and bidirectional command execution over WebSocket.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "esp_http_server.h"
#include <cstddef>

namespace ws_bridge {

// Initialize WebSocket on the given server at /ws
void init(httpd_handle_t server);

// Broadcast a JSON message to all connected WebSocket clients
void broadcast(const char* json);

// Send system status update to all clients
void sendStatus();

// Send event notification
void sendEvent(const char* event_name, const char* data_json = nullptr);

// Send toast notification to all clients
void sendToast(const char* message, const char* level = "info");

// Send serial output line to terminal clients
void sendSerialLine(const char* line);

// Process periodic tasks: status broadcasts, cleanup, drain injected commands
// Call from main tick loop (replaces cleanup + handleMessage).
void tick();

// Get connected client count
int clientCount();

// Cleanup disconnected clients (call periodically)
void cleanup();

}  // namespace ws_bridge
