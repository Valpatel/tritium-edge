// MBTiles Reader — reads map tiles from SQLite MBTiles files on SD card.
//
// MBTiles is a standard SQLite format for tiled map data:
//   CREATE TABLE tiles (zoom_level INTEGER, tile_column INTEGER,
//                       tile_row INTEGER, tile_data BLOB);
//   CREATE TABLE metadata (name TEXT, value TEXT);
//
// Note: MBTiles uses TMS y-coordinate (flipped from OSM convention).
// This reader handles the conversion automatically.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <cstdint>
#include <cstddef>

namespace mbtiles {

struct Metadata {
    char name[64];
    char description[128];
    char format[16];        // "png", "jpg", "pbf"
    uint8_t min_zoom;
    uint8_t max_zoom;
    double center_lat;
    double center_lon;
    uint8_t center_zoom;
    int tile_count;
};

/// Open an MBTiles file. Returns true on success.
bool open(const char* path);

/// Close the MBTiles file.
void close();

/// Check if a file is currently open.
bool is_open();

/// Get metadata from the MBTiles file.
bool get_metadata(Metadata& out);

/// Get a tile's raw data (PNG/JPEG blob).
/// Uses OSM convention for y coordinate (converts internally from TMS).
/// Caller must free() the returned buffer.
/// Returns nullptr if tile not found. Sets out_len to byte count.
uint8_t* get_tile(uint8_t zoom, uint32_t x, uint32_t y, size_t& out_len);

/// Get total tile count.
int get_tile_count();

/// Check if a specific tile exists.
bool has_tile(uint8_t zoom, uint32_t x, uint32_t y);

// Coordinate math helpers (OSM slippy map convention)
uint32_t lon_to_tile_x(double lon, uint8_t zoom);
uint32_t lat_to_tile_y(double lat, uint8_t zoom);
double tile_x_to_lon(uint32_t x, uint8_t zoom);
double tile_y_to_lat(uint32_t y, uint8_t zoom);

}  // namespace mbtiles
