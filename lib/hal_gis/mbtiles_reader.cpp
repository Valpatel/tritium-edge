// MBTiles Reader — SQLite-backed map tile access.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "mbtiles_reader.h"
#include <cstdio>
#include <cstring>
#include <cmath>

#ifdef SIMULATOR

namespace mbtiles {
bool open(const char*) { return false; }
void close() {}
bool is_open() { return false; }
bool get_metadata(Metadata&) { return false; }
uint8_t* get_tile(uint8_t, uint32_t, uint32_t, size_t&) { return nullptr; }
int get_tile_count() { return 0; }
bool has_tile(uint8_t, uint32_t, uint32_t) { return false; }
uint32_t lon_to_tile_x(double, uint8_t) { return 0; }
uint32_t lat_to_tile_y(double, uint8_t) { return 0; }
double tile_x_to_lon(uint32_t, uint8_t) { return 0; }
double tile_y_to_lat(uint32_t, uint8_t) { return 0; }
}

#else

#include "sqlite3.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace mbtiles {

static sqlite3* s_db = nullptr;
static sqlite3_stmt* s_tile_stmt = nullptr;

bool open(const char* path) {
    if (s_db) return true;  // Already open

    int rc = sqlite3_open_v2(path, &s_db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        printf("[mbtiles] Failed to open %s: %s\n", path,
               s_db ? sqlite3_errmsg(s_db) : "null");
        if (s_db) { sqlite3_close(s_db); s_db = nullptr; }
        return false;
    }

    // Prepare the tile lookup statement
    // MBTiles uses TMS y-coordinate, so we convert in the query
    rc = sqlite3_prepare_v2(s_db,
        "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?",
        -1, &s_tile_stmt, nullptr);
    if (rc != SQLITE_OK) {
        printf("[mbtiles] Failed to prepare statement: %s\n", sqlite3_errmsg(s_db));
        sqlite3_close(s_db);
        s_db = nullptr;
        return false;
    }

    printf("[mbtiles] Opened %s (%d tiles)\n", path, get_tile_count());
    return true;
}

void close() {
    if (s_tile_stmt) { sqlite3_finalize(s_tile_stmt); s_tile_stmt = nullptr; }
    if (s_db) { sqlite3_close(s_db); s_db = nullptr; }
}

bool is_open() {
    return s_db != nullptr;
}

bool get_metadata(Metadata& out) {
    if (!s_db) return false;
    memset(&out, 0, sizeof(out));

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(s_db, "SELECT name, value FROM metadata", -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(stmt, 0);
        const char* value = (const char*)sqlite3_column_text(stmt, 1);
        if (!name || !value) continue;

        if (strcmp(name, "name") == 0) {
            strncpy(out.name, value, sizeof(out.name) - 1);
        } else if (strcmp(name, "description") == 0) {
            strncpy(out.description, value, sizeof(out.description) - 1);
        } else if (strcmp(name, "format") == 0) {
            strncpy(out.format, value, sizeof(out.format) - 1);
        } else if (strcmp(name, "minzoom") == 0) {
            out.min_zoom = (uint8_t)atoi(value);
        } else if (strcmp(name, "maxzoom") == 0) {
            out.max_zoom = (uint8_t)atoi(value);
        } else if (strcmp(name, "center") == 0) {
            // Format: "lon,lat,zoom"
            sscanf(value, "%lf,%lf,%hhu", &out.center_lon, &out.center_lat, &out.center_zoom);
        }
    }
    sqlite3_finalize(stmt);

    out.tile_count = get_tile_count();
    return true;
}

uint8_t* get_tile(uint8_t zoom, uint32_t x, uint32_t y, size_t& out_len) {
    if (!s_db || !s_tile_stmt) { out_len = 0; return nullptr; }

    // Convert OSM y to TMS y: tms_y = (2^zoom - 1) - osm_y
    uint32_t tms_y = ((1u << zoom) - 1) - y;

    sqlite3_reset(s_tile_stmt);
    sqlite3_bind_int(s_tile_stmt, 1, zoom);
    sqlite3_bind_int(s_tile_stmt, 2, x);
    sqlite3_bind_int(s_tile_stmt, 3, tms_y);

    if (sqlite3_step(s_tile_stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(s_tile_stmt, 0);
        int blob_len = sqlite3_column_bytes(s_tile_stmt, 0);
        if (blob && blob_len > 0) {
            // Copy to caller-owned buffer (PSRAM preferred)
            uint8_t* buf = (uint8_t*)malloc(blob_len);
            if (buf) {
                memcpy(buf, blob, blob_len);
                out_len = blob_len;
                return buf;
            }
        }
    }

    out_len = 0;
    return nullptr;
}

int get_tile_count() {
    if (!s_db) return 0;

    sqlite3_stmt* stmt;
    int count = 0;
    if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM tiles", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

bool has_tile(uint8_t zoom, uint32_t x, uint32_t y) {
    size_t len;
    uint8_t* data = get_tile(zoom, x, y, len);
    if (data) { free(data); return true; }
    return false;
}

// OSM slippy map coordinate math

uint32_t lon_to_tile_x(double lon, uint8_t zoom) {
    return (uint32_t)floor((lon + 180.0) / 360.0 * (1 << zoom));
}

uint32_t lat_to_tile_y(double lat, uint8_t zoom) {
    double lat_rad = lat * M_PI / 180.0;
    return (uint32_t)floor((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * (1 << zoom));
}

double tile_x_to_lon(uint32_t x, uint8_t zoom) {
    return x / (double)(1 << zoom) * 360.0 - 180.0;
}

double tile_y_to_lat(uint32_t y, uint8_t zoom) {
    double n = M_PI - 2.0 * M_PI * y / (double)(1 << zoom);
    return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

}  // namespace mbtiles

#endif  // !SIMULATOR
