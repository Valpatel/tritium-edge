// Tritium-OS MQTT OTA — remote firmware push from SC dashboard
// Subscribes to tritium/{device_id}/cmd/ota for firmware URL commands
// Reports progress on tritium/{device_id}/ota/progress
// Auto-rollback after 3 failed boots
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <cstdint>
#include <cstddef>

namespace ota_mqtt {

// Initialize MQTT OTA bridge.
// device_id: used for topic construction (tritium/{device_id}/cmd/ota)
// Must be called after MQTT bridge is active.
bool init(const char* device_id);

// Call in loop() — processes pending OTA commands
void tick();

// Check if an MQTT-triggered OTA is in progress
bool is_active();

// Auto-rollback: track boot attempts in NVS.
// Call early in setup() BEFORE markValid().
// If boot_count >= max_failures, rolls back to previous partition.
// Returns true if rollback was triggered (device will reboot).
bool checkAutoRollback(uint8_t max_failures = 3);

// Mark boot successful — resets the boot failure counter.
// Call after device has been running stably (e.g., 30s after boot).
void markBootSuccessful();

}  // namespace ota_mqtt
