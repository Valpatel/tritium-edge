// Tritium-OS OTA Routes — REST API for firmware update management
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "esp_http_server.h"

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
void registerRoutes(httpd_handle_t server);

}  // namespace ota_routes
