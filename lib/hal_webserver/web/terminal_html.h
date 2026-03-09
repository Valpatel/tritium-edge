// Tritium-OS Web Terminal — PROGMEM-embedded HTML page for browser-based
// serial console over WebSocket.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// AUTO-GENERATED — do not edit manually.
// Source: terminal.html
// To update: edit terminal.html, then regenerate terminal.html.inc
// by wrapping the content in a C raw string literal.

#pragma once

#include <pgmspace.h>

// The terminal page is served as a single self-contained HTML file with
// inline CSS and JavaScript.  No external dependencies.
//
// Usage in ws_bridge.cpp:
//   #include "web/terminal_html.h"
//   server->on("/terminal", HTTP_GET, [](AsyncWebServerRequest* req) {
//       req->send_P(200, "text/html", TERMINAL_HTML, TERMINAL_HTML_LEN);
//   });

static const char TERMINAL_HTML[] PROGMEM =
#include "terminal.html.inc"
;

static const size_t TERMINAL_HTML_LEN = sizeof(TERMINAL_HTML) - 1;
