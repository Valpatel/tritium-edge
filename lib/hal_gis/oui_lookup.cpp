// OUI Manufacturer Lookup — SQLite-backed MAC prefix resolution.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "oui_lookup.h"
#include <cstdio>
#include <cstring>
#include <cctype>

#ifdef SIMULATOR

// Stub implementation for simulator/desktop builds
namespace oui_lookup {
bool init(const char*) { return false; }
void shutdown() {}
const char* lookup(const uint8_t[6]) { return nullptr; }
const char* lookup_str(const char*) { return nullptr; }
bool is_available() { return false; }
int get_entry_count() { return 0; }
}

#else

#include "sqlite3.h"

namespace oui_lookup {

static sqlite3* s_db = nullptr;
static sqlite3_stmt* s_lookup_stmt = nullptr;
static char s_result_buf[50];  // Manufacturer name (max 48 chars + null)
static int s_entry_count = 0;

// LRU cache — avoid repeated DB hits for the same prefixes
static constexpr int CACHE_SIZE = 16;
struct CacheEntry {
    uint32_t prefix_key;  // 3-byte prefix packed into uint32_t
    char name[50];
    bool valid;
};
static CacheEntry s_cache[CACHE_SIZE];
static int s_cache_idx = 0;  // Round-robin write position

static uint32_t mac_to_key(const uint8_t mac[6]) {
    return ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
}

static const char* cache_lookup(uint32_t key) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].valid && s_cache[i].prefix_key == key) {
            return s_cache[i].name;
        }
    }
    return nullptr;
}

static void cache_insert(uint32_t key, const char* name) {
    CacheEntry& e = s_cache[s_cache_idx];
    e.prefix_key = key;
    strncpy(e.name, name, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    e.valid = true;
    s_cache_idx = (s_cache_idx + 1) % CACHE_SIZE;
}

bool init(const char* db_path) {
    if (s_db) return true;  // Already open

    int rc = sqlite3_open_v2(db_path, &s_db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        printf("[oui] Failed to open %s: %s\n", db_path,
               s_db ? sqlite3_errmsg(s_db) : "null");
        if (s_db) { sqlite3_close(s_db); s_db = nullptr; }
        return false;
    }

    // Prepare the lookup statement
    rc = sqlite3_prepare_v2(s_db,
        "SELECT name FROM oui WHERE prefix=?", -1,
        &s_lookup_stmt, nullptr);
    if (rc != SQLITE_OK) {
        printf("[oui] Failed to prepare statement: %s\n", sqlite3_errmsg(s_db));
        sqlite3_close(s_db);
        s_db = nullptr;
        return false;
    }

    // Get entry count
    sqlite3_stmt* cnt_stmt;
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM oui", -1, &cnt_stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(cnt_stmt) == SQLITE_ROW) {
            s_entry_count = sqlite3_column_int(cnt_stmt, 0);
        }
        sqlite3_finalize(cnt_stmt);
    }

    // Clear cache
    memset(s_cache, 0, sizeof(s_cache));
    s_cache_idx = 0;

    printf("[oui] Loaded %d manufacturer entries from %s\n", s_entry_count, db_path);
    return true;
}

void shutdown() {
    if (s_lookup_stmt) { sqlite3_finalize(s_lookup_stmt); s_lookup_stmt = nullptr; }
    if (s_db) { sqlite3_close(s_db); s_db = nullptr; }
    s_entry_count = 0;
}

const char* lookup(const uint8_t mac[6]) {
    if (!s_db || !s_lookup_stmt) return nullptr;

    // Check cache first
    uint32_t key = mac_to_key(mac);
    const char* cached = cache_lookup(key);
    if (cached) return cached;

    // Build prefix string: "AABBCC"
    char prefix[7];
    snprintf(prefix, sizeof(prefix), "%02X%02X%02X", mac[0], mac[1], mac[2]);

    sqlite3_reset(s_lookup_stmt);
    sqlite3_bind_text(s_lookup_stmt, 1, prefix, 6, SQLITE_STATIC);

    if (sqlite3_step(s_lookup_stmt) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(s_lookup_stmt, 0);
        if (name) {
            strncpy(s_result_buf, name, sizeof(s_result_buf) - 1);
            s_result_buf[sizeof(s_result_buf) - 1] = '\0';
            cache_insert(key, s_result_buf);
            return s_result_buf;
        }
    }

    return nullptr;
}

const char* lookup_str(const char* mac_str) {
    if (!mac_str) return nullptr;

    // Parse "AA:BB:CC:DD:EE:FF" or "AABBCCDDEEFF" or just first 3 bytes
    uint8_t mac[6] = {};
    unsigned a, b, c;

    if (sscanf(mac_str, "%02x:%02x:%02x", &a, &b, &c) == 3 ||
        sscanf(mac_str, "%02x%02x%02x", &a, &b, &c) == 3) {
        mac[0] = (uint8_t)a;
        mac[1] = (uint8_t)b;
        mac[2] = (uint8_t)c;
        return lookup(mac);
    }

    return nullptr;
}

bool is_available() {
    return s_db != nullptr;
}

int get_entry_count() {
    return s_entry_count;
}

}  // namespace oui_lookup

#endif  // !SIMULATOR
