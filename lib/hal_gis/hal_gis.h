#pragma once
// Offline GIS / Map Tile HAL — serves OSM slippy-map tiles from SD card.
//
// Tiles are stored in standard OSM directory layout:
//   /sdcard/gis/{layer}/{z}/{x}/{y}.png
//
// A manifest file at /sdcard/gis/manifest.json describes available layers,
// bounding boxes, and zoom ranges. MBTiles (SQLite) packages can be imported
// to flat-file layout for serving without SQLite overhead at runtime.
//
// Usage:
//   #include "hal_gis.h"
//   GisHAL gis;
//   GisConfig cfg;
//   cfg.sd_mount_point = "/sdcard";
//   gis.init(cfg);
//   auto layers = gis.getLayers();
//   size_t len;
//   uint8_t* tile = gis.getTile("streets", 14, 8192, 5461, len);
//   free(tile);

#include <cstdint>
#include <cstddef>

// OSM slippy-map tile coordinates
struct GisTile {
    uint8_t  zoom;          // zoom level (0-20)
    uint32_t x;             // tile X coordinate
    uint32_t y;             // tile Y coordinate
    char     path[128];     // absolute path on SD card
    uint32_t size_bytes;    // file size
};

// Geographic bounding box (WGS84)
struct GisBounds {
    double lat_min;
    double lon_min;
    double lat_max;
    double lon_max;
};

// Layer types
enum class GisLayerType : uint8_t {
    STREETS,
    SATELLITE,
    TERRAIN,
    CUSTOM
};

// A map layer (one set of tiles covering some area/zoom range)
struct GisLayer {
    char         name[32];      // e.g. "streets", "satellite"
    GisLayerType type;
    uint32_t     tile_count;    // total tiles in this layer
    uint8_t      zoom_min;
    uint8_t      zoom_max;
    GisBounds    bounds;        // geographic extent
};

// Configuration
struct GisConfig {
    const char* sd_mount_point;     // e.g. "/sdcard"
    const char* tile_base_path;     // e.g. "/gis" (relative to mount point)
    uint8_t     max_cache_tiles;    // LRU tile cache size in PSRAM (default 16)
};

// Callback for streaming tile data during export
using GisTileCallback = void (*)(const GisTile& tile, const uint8_t* data, size_t len);

// Callback for MBTiles import progress (tiles_done, tiles_total)
using GisProgressCallback = void (*)(uint32_t done, uint32_t total);

class GisHAL {
public:
    // Initialize — scans SD card for manifest.json, builds layer index
    bool init(const GisConfig& config);
    void deinit();
    bool isReady() const { return _ready; }

    // Layer discovery
    int getLayerCount() const;
    bool getLayer(int index, GisLayer& out) const;
    bool getLayerByName(const char* name, GisLayer& out) const;

    // Tile access — caller must free() returned buffer.
    // Returns nullptr if tile not found. Sets outLen to byte count.
    uint8_t* getTile(const char* layer, uint8_t z, uint32_t x, uint32_t y,
                     size_t& outLen);

    // Check if a specific location has tile coverage at a given zoom
    bool hasCoverage(const char* layer, double lat, double lon, uint8_t zoom) const;

    // Statistics
    uint32_t getTileCount() const;
    uint64_t getStorageUsed() const;

    // MBTiles import — extracts tiles from SQLite DB to flat file structure.
    // Requires the sqlite3 component (optional, compile-gated).
    // Returns number of tiles extracted, or -1 on error.
    int importMBTiles(const char* mbtiles_path, const char* layer_name,
                      GisProgressCallback progress = nullptr);

    // Export / stream tiles for a region to a callback (e.g. ESP-NOW, MQTT)
    int exportRegion(const char* layer, const GisBounds& bounds,
                     uint8_t zoom_min, uint8_t zoom_max,
                     GisTileCallback callback);

    // Write or update the manifest.json on SD card
    bool writeManifest();

    // Test harness
    struct TestResult {
        bool init_ok;
        int  layer_count;
        uint32_t tile_count;
        uint64_t storage_bytes;
        bool tile_read_ok;
        uint32_t tile_read_ms;      // time to read one tile
        bool coverage_check_ok;
        uint32_t test_duration_ms;
    };
    TestResult runTest();

private:
    bool _ready = false;
    GisConfig _config = {};

    // Layer storage (max 8 layers — streets, satellite, terrain, custom * 2)
    static constexpr int MAX_LAYERS = 8;
    GisLayer _layers[MAX_LAYERS] = {};
    int _layerCount = 0;

    // LRU tile cache in PSRAM
    struct CacheEntry {
        char     layer[32];
        uint8_t  zoom;
        uint32_t x;
        uint32_t y;
        uint8_t* data;
        size_t   len;
        uint32_t last_access;   // millis() or monotonic counter
    };
    static constexpr int MAX_CACHE = 32;
    CacheEntry* _cache = nullptr;
    int _cacheSize = 0;
    uint32_t _accessCounter = 0;

    // Internal helpers
    bool loadManifest();
    bool scanLayerDir(const char* layer_name);
    void buildTilePath(char* buf, size_t bufSize, const char* layer,
                       uint8_t z, uint32_t x, uint32_t y) const;
    uint8_t* readTileFromSD(const char* path, size_t& outLen);
    uint8_t* cacheLookup(const char* layer, uint8_t z, uint32_t x, uint32_t y,
                         size_t& outLen);
    void cacheInsert(const char* layer, uint8_t z, uint32_t x, uint32_t y,
                     const uint8_t* data, size_t len);

    // Coordinate math — OSM slippy map convention
    static uint32_t lonToTileX(double lon, uint8_t zoom);
    static uint32_t latToTileY(double lat, uint8_t zoom);
    static double tileXToLon(uint32_t x, uint8_t zoom);
    static double tileYToLat(uint32_t y, uint8_t zoom);
};
