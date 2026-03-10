// Tritium-OS File Manager — REST API + Web UI for LittleFS and SD card
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "esp_http_server.h"

namespace file_manager {

// Register all file manager routes on the given server
void registerRoutes(httpd_handle_t server);

}  // namespace file_manager
