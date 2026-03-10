// Tritium Sighting Logger — BLE + WiFi MAC/RSSI logging to SQLite on SD card
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "hal_sighting_logger.h"
#include "debug_log.h"

#include <cstring>
#include <cstdio>

static constexpr const char* TAG = "sighting_log";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace hal_sighting_logger {

bool init(const LoggerConfig& config) { (void)config; return false; }
void shutdown() {}
void log_ble(const char* mac, const char* name, int8_t rssi, bool is_known) {
    (void)mac; (void)name; (void)rssi; (void)is_known;
}
void log_wifi(const char* ssid, const uint8_t bssid[6], int8_t rssi,
              uint8_t channel, uint8_t auth_type) {
    (void)ssid; (void)bssid; (void)rssi; (void)channel; (void)auth_type;
}
int flush() { return 0; }
void tick() {}
bool is_active() { return false; }
Stats get_stats() { return Stats{}; }
void enable() {}
void disable() {}

}  // namespace hal_sighting_logger

// ============================================================================
// ESP32 — real SQLite sighting logger
// ============================================================================
#else

#include "sqlite3.h"
#include "tritium_compat.h"
#include "esp_heap_caps.h"

#include <sys/stat.h>
#include <ctime>

// Optional NVS settings integration
#if __has_include("os_settings.h")
#include "os_settings.h"
#define HAS_SETTINGS 1
static constexpr const char* SETTINGS_DOMAIN = "tracking";
static constexpr const char* SETTINGS_KEY    = "enabled";
#else
#define HAS_SETTINGS 0
#endif

namespace hal_sighting_logger {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

// Pending entry for the ring buffer
struct PendingEntry {
    char     type;          // 'B' = BLE, 'W' = WiFi
    char     mac[18];       // "XX:XX:XX:XX:XX:XX\0"
    char     name[33];      // BLE name or WiFi SSID
    int8_t   rssi;
    uint8_t  channel;       // WiFi channel (0 for BLE)
    uint8_t  auth;          // WiFi auth type (0 for BLE)
    uint8_t  is_known;      // BLE known flag
    uint32_t timestamp;     // Unix epoch seconds, or millis if no NTP
};

static sqlite3*         _db = nullptr;
static bool             _enabled = false;
static bool             _inited = false;
static LoggerConfig     _config;

static PendingEntry*    _ring = nullptr;
static uint16_t         _ring_head = 0;
static uint16_t         _ring_count = 0;

static SemaphoreHandle_t _mutex = nullptr;
static uint32_t         _last_flush_ms = 0;

// Stats counters
static uint32_t         _ble_logged = 0;
static uint32_t         _wifi_logged = 0;
static uint32_t         _total_rows = 0;

// ---------------------------------------------------------------------------
// Schema SQL
// ---------------------------------------------------------------------------
static const char* SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS ble_sightings ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "mac TEXT NOT NULL,"
        "name TEXT DEFAULT '',"
        "rssi INTEGER NOT NULL,"
        "is_known INTEGER DEFAULT 0,"
        "timestamp TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS wifi_sightings ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "bssid TEXT NOT NULL,"
        "ssid TEXT DEFAULT '',"
        "rssi INTEGER NOT NULL,"
        "channel INTEGER DEFAULT 0,"
        "auth TEXT DEFAULT '',"
        "timestamp TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_ble_ts ON ble_sightings(timestamp);"
    "CREATE INDEX IF NOT EXISTS idx_wifi_ts ON wifi_sightings(timestamp);"
    "CREATE INDEX IF NOT EXISTS idx_ble_mac ON ble_sightings(mac);"
    "CREATE INDEX IF NOT EXISTS idx_wifi_bssid ON wifi_sightings(bssid);";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Format timestamp as ISO8601 or boot-relative if no NTP
static void format_timestamp(uint32_t ts, char* buf, size_t len) {
    if (ts >= 1700000000) {
        // Real wall-clock time from NTP
        time_t t = (time_t)ts;
        struct tm tm_buf;
        gmtime_r(&t, &tm_buf);
        snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                 tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    } else {
        // No NTP yet — use millis-based marker
        snprintf(buf, len, "boot+%lu", (unsigned long)millis());
    }
}

static uint32_t get_current_timestamp() {
    time_t now;
    time(&now);
    return (uint32_t)now;
}

static int exec_sql(const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(_db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        DBG_ERROR(TAG, "SQL error: %s (rc=%d)", err ? err : "?", rc);
        sqlite3_free(err);
    }
    return rc;
}

static uint32_t get_db_file_size() {
    struct stat st;
    if (stat(_config.db_path, &st) == 0) {
        return (uint32_t)st.st_size;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Format BSSID bytes as "XX:XX:XX:XX:XX:XX"
// ---------------------------------------------------------------------------
static void format_bssid(const uint8_t bssid[6], char* out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2],
             bssid[3], bssid[4], bssid[5]);
}

// ---------------------------------------------------------------------------
// Auth type to string
// ---------------------------------------------------------------------------
static const char* auth_type_str(uint8_t auth) {
    switch (auth) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA_PSK";
        case 3: return "WPA2_PSK";
        case 4: return "WPA_WPA2_PSK";
        case 5: return "WPA2_ENTERPRISE";
        case 6: return "WPA3_PSK";
        case 7: return "WPA2_WPA3_PSK";
        default: return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool init(const LoggerConfig& config) {
    if (_inited) {
        DBG_WARN(TAG, "Already initialized");
        return true;
    }

    _config = config;

    // Check SD card is mounted
    struct stat sd_stat;
    if (stat("/sdcard", &sd_stat) != 0) {
        DBG_ERROR(TAG, "SD card not mounted at /sdcard");
        return false;
    }

    // Open SQLite database
    int rc = sqlite3_open_v2(_config.db_path, &_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        DBG_ERROR(TAG, "Failed to open DB: %s (rc=%d)",
                sqlite3_errmsg(_db), rc);
        if (_db) { sqlite3_close(_db); _db = nullptr; }
        return false;
    }

    // Pragmas for performance
    exec_sql("PRAGMA journal_mode=WAL;");
    exec_sql("PRAGMA synchronous=NORMAL;");
    exec_sql("PRAGMA cache_size=-64;");  // 64KB

    // Create tables and indices
    if (exec_sql(SCHEMA_SQL) != SQLITE_OK) {
        DBG_ERROR(TAG, "Failed to create schema");
        sqlite3_close(_db);
        _db = nullptr;
        return false;
    }

    // Allocate ring buffer
    _ring = (PendingEntry*)heap_caps_calloc(
        _config.max_batch, sizeof(PendingEntry), MALLOC_CAP_DEFAULT);
    if (!_ring) {
        DBG_ERROR(TAG, "Failed to allocate ring buffer (%u entries)",
                _config.max_batch);
        sqlite3_close(_db);
        _db = nullptr;
        return false;
    }
    _ring_head = 0;
    _ring_count = 0;

    // Create mutex
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) {
        DBG_ERROR(TAG, "Failed to create mutex");
        free(_ring);
        _ring = nullptr;
        sqlite3_close(_db);
        _db = nullptr;
        return false;
    }

    // Load enabled state from NVS if available
#if HAS_SETTINGS
    _enabled = TritiumSettings::instance().getBool(
        SETTINGS_DOMAIN, SETTINGS_KEY, false);
    DBG_INFO(TAG, "Loaded enabled=%d from NVS", _enabled);
#else
    _enabled = true;
#endif

    _last_flush_ms = millis();
    _inited = true;

    DBG_INFO(TAG, "Initialized: db=%s batch=%u flush=%lums enabled=%d",
             _config.db_path, _config.max_batch,
             (unsigned long)_config.flush_interval_ms, _enabled);
    return true;
}

void shutdown() {
    if (!_inited) return;

    // Flush remaining entries
    if (_ring_count > 0) {
        flush();
    }

    _inited = false;
    _enabled = false;

    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
    if (_ring) {
        free(_ring);
        _ring = nullptr;
    }
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }

    _ring_head = 0;
    _ring_count = 0;

    DBG_INFO(TAG, "Shutdown complete");
}

void log_ble(const char* mac, const char* name, int8_t rssi, bool is_known) {
    if (!_inited || !_enabled) return;

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    if (_ring_count >= _config.max_batch) {
        // Ring full — drop oldest by advancing head
        _ring_head = (_ring_head + 1) % _config.max_batch;
        _ring_count--;
    }

    uint16_t idx = (_ring_head + _ring_count) % _config.max_batch;
    PendingEntry& e = _ring[idx];
    e.type = 'B';
    strncpy(e.mac, mac ? mac : "", sizeof(e.mac) - 1);
    e.mac[sizeof(e.mac) - 1] = '\0';
    strncpy(e.name, name ? name : "", sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    e.rssi = rssi;
    e.channel = 0;
    e.auth = 0;
    e.is_known = is_known ? 1 : 0;
    e.timestamp = get_current_timestamp();
    _ring_count++;

    xSemaphoreGive(_mutex);
}

void log_wifi(const char* ssid, const uint8_t bssid[6], int8_t rssi,
              uint8_t channel, uint8_t auth_type) {
    if (!_inited || !_enabled) return;

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    if (_ring_count >= _config.max_batch) {
        _ring_head = (_ring_head + 1) % _config.max_batch;
        _ring_count--;
    }

    uint16_t idx = (_ring_head + _ring_count) % _config.max_batch;
    PendingEntry& e = _ring[idx];
    e.type = 'W';
    format_bssid(bssid, e.mac);
    strncpy(e.name, ssid ? ssid : "", sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    e.rssi = rssi;
    e.channel = channel;
    e.auth = auth_type;
    e.is_known = 0;
    e.timestamp = get_current_timestamp();
    _ring_count++;

    xSemaphoreGive(_mutex);
}

int flush() {
    if (!_inited || !_db) return 0;

    // Copy pending entries under mutex
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    uint16_t count = _ring_count;
    if (count == 0) {
        xSemaphoreGive(_mutex);
        return 0;
    }

    // Stack-allocate a copy (max 64 entries * ~76 bytes = ~5KB)
    PendingEntry* batch = (PendingEntry*)malloc(count * sizeof(PendingEntry));
    if (!batch) {
        xSemaphoreGive(_mutex);
        DBG_ERROR(TAG, "Failed to allocate flush batch");
        return 0;
    }

    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (_ring_head + i) % _config.max_batch;
        batch[i] = _ring[idx];
    }
    _ring_head = 0;
    _ring_count = 0;

    xSemaphoreGive(_mutex);

    // Insert into DB within a transaction
    exec_sql("BEGIN TRANSACTION;");

    static const char* BLE_INSERT =
        "INSERT INTO ble_sightings (mac, name, rssi, is_known, timestamp) "
        "VALUES (?, ?, ?, ?, ?);";
    static const char* WIFI_INSERT =
        "INSERT INTO wifi_sightings (bssid, ssid, rssi, channel, auth, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* ble_stmt = nullptr;
    sqlite3_stmt* wifi_stmt = nullptr;
    sqlite3_prepare_v2(_db, BLE_INSERT, -1, &ble_stmt, nullptr);
    sqlite3_prepare_v2(_db, WIFI_INSERT, -1, &wifi_stmt, nullptr);

    int inserted = 0;
    char ts_buf[32];

    for (uint16_t i = 0; i < count; i++) {
        PendingEntry& e = batch[i];
        format_timestamp(e.timestamp, ts_buf, sizeof(ts_buf));

        if (e.type == 'B' && ble_stmt) {
            sqlite3_bind_text(ble_stmt, 1, e.mac, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ble_stmt, 2, e.name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ble_stmt, 3, e.rssi);
            sqlite3_bind_int(ble_stmt, 4, e.is_known);
            sqlite3_bind_text(ble_stmt, 5, ts_buf, -1, SQLITE_TRANSIENT);

            if (sqlite3_step(ble_stmt) == SQLITE_DONE) {
                inserted++;
                _ble_logged++;
            }
            sqlite3_reset(ble_stmt);

        } else if (e.type == 'W' && wifi_stmt) {
            sqlite3_bind_text(wifi_stmt, 1, e.mac, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(wifi_stmt, 2, e.name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(wifi_stmt, 3, e.rssi);
            sqlite3_bind_int(wifi_stmt, 4, e.channel);
            sqlite3_bind_text(wifi_stmt, 5, auth_type_str(e.auth), -1, SQLITE_STATIC);
            sqlite3_bind_text(wifi_stmt, 6, ts_buf, -1, SQLITE_TRANSIENT);

            if (sqlite3_step(wifi_stmt) == SQLITE_DONE) {
                inserted++;
                _wifi_logged++;
            }
            sqlite3_reset(wifi_stmt);
        }
    }

    if (ble_stmt) sqlite3_finalize(ble_stmt);
    if (wifi_stmt) sqlite3_finalize(wifi_stmt);

    exec_sql("COMMIT;");

    free(batch);

    _total_rows += (uint32_t)inserted;
    if (inserted > 0) {
        DBG_INFO(TAG, "Flushed %d sightings (BLE=%lu WiFi=%lu total=%lu)",
                 inserted, (unsigned long)_ble_logged,
                 (unsigned long)_wifi_logged, (unsigned long)_total_rows);
    }

    return inserted;
}

void tick() {
    if (!_inited || !_enabled) return;

    uint32_t now = millis();
    if (_ring_count > 0 && (now - _last_flush_ms) >= _config.flush_interval_ms) {
        flush();
        _last_flush_ms = now;
    }
}

bool is_active() {
    return _inited && _enabled && _db != nullptr;
}

Stats get_stats() {
    Stats s = {};
    s.ble_logged = _ble_logged;
    s.wifi_logged = _wifi_logged;
    s.total_rows = _total_rows;
    s.db_size_bytes = _db ? get_db_file_size() : 0;
    s.db_open = (_db != nullptr);
    return s;
}

void enable() {
    _enabled = true;
#if HAS_SETTINGS
    TritiumSettings::instance().setBool(SETTINGS_DOMAIN, SETTINGS_KEY, true);
#endif
    DBG_INFO(TAG, "Enabled");
}

void disable() {
    _enabled = false;
    // Flush anything pending before disabling
    if (_inited && _ring_count > 0) {
        flush();
    }
#if HAS_SETTINGS
    TritiumSettings::instance().setBool(SETTINGS_DOMAIN, SETTINGS_KEY, false);
#endif
    DBG_INFO(TAG, "Disabled");
}

}  // namespace hal_sighting_logger

#endif  // SIMULATOR
