// Tritium-OS Remote Control — PROGMEM-embedded HTML page for browser-based
// remote screen viewing and touch injection.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// AUTO-GENERATED — do not edit manually.
// Source: remote.html
// To update: edit remote.html, then regenerate remote.html.inc
// by wrapping the content in a C raw string literal.

#pragma once

#include <pgmspace.h>

// The remote control page is served as a single self-contained HTML file with
// inline CSS and JavaScript.  No external dependencies (fonts load from CDN
// but gracefully fall back to system fonts).
//
// Usage in hal_webserver.cpp:
//   #include "web/remote_html.h"
//   _server->on("/remote", HTTP_GET, [this]() {
//       _server->send_P(200, "text/html", REMOTE_HTML, REMOTE_HTML_LEN);
//   });

static const char REMOTE_HTML[] PROGMEM =
#include "remote.html.inc"
;

static const size_t REMOTE_HTML_LEN = sizeof(REMOTE_HTML) - 1;
