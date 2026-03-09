// Tritium-OS File Manager — REST API + Web UI for LittleFS and SD card
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#if __has_include("ESPAsyncWebServer.h")
#include <ESPAsyncWebServer.h>
#define HAS_ASYNC_WEBSERVER 1
#else
#define HAS_ASYNC_WEBSERVER 0
struct AsyncWebServer;  // forward declare stub
#endif

namespace file_manager {

// Register all file manager routes on the given server
void registerRoutes(AsyncWebServer* server);

}  // namespace file_manager
