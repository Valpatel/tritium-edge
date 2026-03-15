// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
//
// hal_diag_dump.h — Full diagnostic dump on MQTT command.
//
// When the device receives `tritium/{device_id}/cmd/dump`, it publishes
// a comprehensive diagnostic snapshot to `tritium/{device_id}/diagnostics`:
// heap, tasks, stack watermarks, NVS contents, WiFi state, BLE state,
// and all HAL statuses. For remote troubleshooting.
//
// Usage:
//   #include "hal_diag_dump.h"
//   // In your command callback:
//   if (strcmp(cmd, "dump") == 0) {
//       hal_diag_dump::publish(mqtt_publish_fn);
//   }

#pragma once

#include <cstdint>
#include <cstddef>

namespace hal_diag_dump {

/// Function type for MQTT publish callback.
/// Parameters: topic, payload, retain, qos
using PublishFn = bool (*)(const char* topic, const char* payload, bool retain, int qos);

/// Collect full diagnostic snapshot and publish via MQTT.
/// @param publish    MQTT publish function
/// @param device_id  Device ID for topic construction
/// @return true if published successfully
bool publish_diagnostic_dump(PublishFn publish, const char* device_id);

/// Collect diagnostic snapshot into a JSON buffer.
/// @param buf      Output buffer
/// @param buf_size Buffer size
/// @return Number of bytes written (excluding null terminator)
int collect_diagnostic_json(char* buf, size_t buf_size);

}  // namespace hal_diag_dump
