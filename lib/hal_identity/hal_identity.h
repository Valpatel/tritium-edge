// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once
// Device Identity Service
// Generates and persists a unique device UUID in NVS on first boot.
// Provides a permanent device identifier that survives IP changes,
// reflashing, and WiFi reconnects. Included in every MQTT message.
//
// Usage:
//   #include "hal_identity.h"
//   hal_identity::init();
//   const char* uuid = hal_identity::get_uuid();  // "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
//   const char* short_id = hal_identity::get_short_id();  // "xxxx-xxxx"

#include <cstdint>
#include <cstddef>

namespace hal_identity {

/// Initialize the identity service.
/// Reads UUID from NVS. If not found, generates a new UUIDv4 and stores it.
/// Returns true on success.
bool init();

/// Get the full UUID string (36 chars + null: "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx").
/// Returns empty string if not initialized.
const char* get_uuid();

/// Get a short ID derived from the UUID (9 chars + null: "xxxx-xxxx").
/// Suitable for display on small screens and log messages.
const char* get_short_id();

/// Get the device name. Falls back to "tritium-{short_id}" if not set.
const char* get_device_name();

/// Set a custom device name (persisted to NVS, max 63 chars).
bool set_device_name(const char* name);

/// Force regenerate the UUID (factory reset scenario).
/// WARNING: This changes the device's permanent identity.
bool regenerate();

/// Check if the identity has been initialized.
bool is_initialized();

/// Write the UUID into a JSON field: "device_uuid":"..." (no trailing comma).
/// Returns number of bytes written, or 0 on error.
int to_json_field(char* buf, size_t size);

}  // namespace hal_identity
