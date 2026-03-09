// Tritium-OS Serial Capture — ring buffer capture of Serial output with
// callback notification for WebSocket terminal streaming.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <stddef.h>

namespace serial_capture {

// Initialize serial output capture
// Hooks into Serial to copy output to a ring buffer
void init();

// Get captured lines (returns number of lines available)
// Each line is null-terminated in the buffer
int getLines(char* buf, size_t buf_size, int max_lines = 50);

// Register callback for new serial output
typedef void (*SerialLineCallback)(const char* line, void* user_data);
void onLine(SerialLineCallback cb, void* user_data = nullptr);

// Write captured lines directly as JSON into buf: {"count":N,"lines":["...",...]}\0
// Returns bytes written (excluding null), or 0 on error.
// Reads from the ring buffer under mutex — no intermediate copy needed.
int getLinesJson(char* buf, size_t buf_size, int max_lines = 50);

// Inject a command as if typed on serial (for WS terminal)
void injectCommand(const char* cmd);

}  // namespace serial_capture
