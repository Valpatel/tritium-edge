// ═══════════════════════════════════════════════════════════════════════════
// AUTO-GENERATED — do not edit manually.
// Source: dashboard.html
// Regenerate with: xxd -i dashboard.html > dashboard_html.h
//   or use the raw-literal approach below for readability.
//
// This file embeds the Tritium-OS web dashboard as a PROGMEM string for
// serving directly from ESP32 flash without LittleFS.
// ═══════════════════════════════════════════════════════════════════════════
#pragma once

// pgmspace.h not needed — PROGMEM is no-op on ESP32 (flash is memory-mapped)

// The dashboard is served as a single self-contained HTML file with inline
// CSS and JavaScript.  To update it, edit dashboard.html and re-run the
// embedding script (or replace the raw literal below).
//
// Current size: ~41 KB — fits comfortably in ESP32 flash.
//
// Usage in hal_webserver.cpp:
//   #include "web/dashboard_html.h"
//   server->send(200, "text/html", FPSTR(DASHBOARD_HTML_V2));

static const char DASHBOARD_HTML_V2[] PROGMEM =
#include "dashboard.html.inc"
;

// NOTE: dashboard.html.inc should be generated from dashboard.html by
// wrapping the content in a C raw string literal:
//
//   echo 'R"rawliteral(' > dashboard.html.inc
//   cat dashboard.html >> dashboard.html.inc
//   echo ')rawliteral"' >> dashboard.html.inc
//
// Alternatively, if your build system supports it, use the simpler
// approach of embedding via PROGMEM with a raw literal directly:

#ifndef DASHBOARD_HTML_INLINE
#define DASHBOARD_HTML_INLINE

// For builds that prefer a single-header approach without the .inc file,
// uncomment the block below and paste the HTML content inside the raw
// literal.  The .inc approach above is preferred for maintainability.

/*
static const char DASHBOARD_HTML_EMBEDDED[] PROGMEM = R"rawliteral(
  ... paste dashboard.html contents here ...
)rawliteral";
*/

#endif // DASHBOARD_HTML_INLINE
