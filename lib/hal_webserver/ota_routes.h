// Tritium-OS OTA Routes — REST API for firmware update management
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

namespace ota_routes {

// Register all OTA management routes on the given server:
//   GET    /api/ota/status     -> OtaStatus JSON
//   POST   /api/ota/upload     -> multipart firmware upload with progress
//   POST   /api/ota/url        -> {"url": "http://..."} pull update
//   POST   /api/ota/rollback   -> rollback to previous partition
//   POST   /api/ota/reboot     -> reboot device
//   GET    /api/ota/history    -> update history array
//   POST   /api/ota/mesh-push  -> distribute to mesh peers
//   GET    /ota                -> OTA web UI page
void registerRoutes(AsyncWebServer* server);

}  // namespace ota_routes
