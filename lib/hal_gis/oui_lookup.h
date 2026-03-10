// OUI (Organizationally Unique Identifier) Manufacturer Lookup
//
// Resolves the first 3 bytes of a MAC address to a manufacturer name
// using a SQLite database on SD card (/sdcard/data/oui.db).
//
// Usage:
//   oui_lookup::init();  // opens DB (call after SD card is mounted)
//   const char* mfg = oui_lookup::lookup(mac_bytes);  // returns nullptr if not found
//   oui_lookup::shutdown();
//
// The database is built by tools/build_oui_db.py from the IEEE OUI CSV.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <cstdint>
#include <cstddef>

namespace oui_lookup {

/// Initialize OUI lookup — opens SQLite DB on SD card.
/// Returns true if DB opened successfully.
bool init(const char* db_path = "/sdcard/data/oui.db");

/// Shut down and close the database.
void shutdown();

/// Look up manufacturer name for a MAC address (first 3 bytes used).
/// Returns a pointer to a static buffer with the name, or nullptr if not found.
/// The returned pointer is valid until the next call to lookup().
const char* lookup(const uint8_t mac[6]);

/// Look up by MAC string ("AA:BB:CC:DD:EE:FF" or "AABBCC").
/// Returns manufacturer name or nullptr.
const char* lookup_str(const char* mac_str);

/// Check if the OUI database is available.
bool is_available();

/// Get number of entries in the database.
int get_entry_count();

}  // namespace oui_lookup
