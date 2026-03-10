#include "hal_gis.h"
#include "debug_log.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>

static constexpr const char* TAG = "gis";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// OSM coordinate math — slippy map tile <-> lat/lon conversion
// https://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
// ============================================================================

uint32_t GisHAL::lonToTileX(double lon, uint8_t zoom) {
    double n = (double)(1 << zoom);
    return (uint32_t)((lon + 180.0) / 360.0 * n);
}

uint32_t GisHAL::latToTileY(double lat, uint8_t zoom) {
    double n = (double)(1 << zoom);
    double lat_rad = lat * M_PI / 180.0;
    return (uint32_t)((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n);
}

double GisHAL::tileXToLon(uint32_t x, uint8_t zoom) {
    double n = (double)(1 << zoom);
    return (double)x / n * 360.0 - 180.0;
}

double GisHAL::tileYToLat(uint32_t y, uint8_t zoom) {
    double n = (double)(1 << zoom);
    double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * (double)y / n)));
    return lat_rad * 180.0 / M_PI;
}

// ============================================================================
// Path building (shared)
// ============================================================================

void GisHAL::buildTilePath(char* buf, size_t bufSize, const char* layer,
                           uint8_t z, uint32_t x, uint32_t y) const {
    snprintf(buf, bufSize, "%s%s/%s/%u/%u/%u.png",
             _config.sd_mount_point, _config.tile_base_path,
             layer, z, x, y);
}

// ============================================================================
// SIMULATOR — platform-specific methods
// ============================================================================
#ifdef SIMULATOR

#include <sys/stat.h>
#include <dirent.h>
#include <chrono>

static uint32_t sim_millis() {
    auto now = std::chrono::steady_clock::now();
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

bool GisHAL::init(const GisConfig& config) {
    _config = config;
    if (!_config.sd_mount_point) _config.sd_mount_point = "./sdcard";
    if (!_config.tile_base_path) _config.tile_base_path = "/gis";
    if (_config.max_cache_tiles == 0) _config.max_cache_tiles = 16;

    _cacheSize = (_config.max_cache_tiles > MAX_CACHE)
                     ? MAX_CACHE : _config.max_cache_tiles;
    _cache = (CacheEntry*)calloc(_cacheSize, sizeof(CacheEntry));
    if (!_cache) {
        DBG_ERROR(TAG, "Failed to allocate tile cache (%d entries)", _cacheSize);
        return false;
    }

    if (!loadManifest()) {
        DBG_WARN(TAG, "No manifest.json found — no layers available");
    }

    _ready = true;
    DBG_INFO(TAG, "GIS init: %d layers, cache=%d tiles", _layerCount, _cacheSize);
    return true;
}

void GisHAL::deinit() {
    if (_cache) {
        for (int i = 0; i < _cacheSize; i++) {
            free(_cache[i].data);
        }
        free(_cache);
        _cache = nullptr;
    }
    _layerCount = 0;
    _ready = false;
}

bool GisHAL::loadManifest() {
    char path[256];
    snprintf(path, sizeof(path), "%s%s/manifest.json",
             _config.sd_mount_point, _config.tile_base_path);

    FILE* f = fopen(path, "rb");
    if (!f) {
        DBG_WARN(TAG, "Cannot open manifest: %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 8192) {
        DBG_ERROR(TAG, "Manifest invalid size: %ld", sz);
        fclose(f);
        return false;
    }

    char* json = (char*)malloc(sz + 1);
    if (!json) { fclose(f); return false; }
    fread(json, 1, sz, f);
    json[sz] = '\0';
    fclose(f);

    // Minimal JSON parsing — looks for layer objects in {"layers":[...]}
    // Format: {"layers":[{"name":"streets","type":"streets","zoom_min":10,
    //          "zoom_max":16,"bounds":[lat_min,lon_min,lat_max,lon_max]},...]}
    //
    // Full JSON parser would be nice but we avoid pulling in ArduinoJson
    // to keep the HAL dependency-free. This handles the manifest format we
    // generate in writeManifest().
    _layerCount = 0;
    const char* p = json;

    while (_layerCount < MAX_LAYERS) {
        const char* nameKey = strstr(p, "\"name\":");
        if (!nameKey) break;
        const char* nameStart = strchr(nameKey + 7, '"');
        if (!nameStart) break;
        nameStart++;
        const char* nameEnd = strchr(nameStart, '"');
        if (!nameEnd) break;

        GisLayer& layer = _layers[_layerCount];
        size_t nameLen = (size_t)(nameEnd - nameStart);
        if (nameLen >= sizeof(layer.name)) nameLen = sizeof(layer.name) - 1;
        memcpy(layer.name, nameStart, nameLen);
        layer.name[nameLen] = '\0';

        // Parse type
        layer.type = GisLayerType::CUSTOM;
        const char* typeKey = strstr(nameEnd, "\"type\":");
        if (typeKey && typeKey < nameEnd + 200) {
            const char* ts = strchr(typeKey + 7, '"');
            if (ts) {
                ts++;
                if (strncmp(ts, "streets", 7) == 0) layer.type = GisLayerType::STREETS;
                else if (strncmp(ts, "satellite", 9) == 0) layer.type = GisLayerType::SATELLITE;
                else if (strncmp(ts, "terrain", 7) == 0) layer.type = GisLayerType::TERRAIN;
            }
        }

        // Parse zoom_min, zoom_max
        layer.zoom_min = 0;
        layer.zoom_max = 20;
        const char* zminKey = strstr(nameEnd, "\"zoom_min\":");
        if (zminKey && zminKey < nameEnd + 300) {
            layer.zoom_min = (uint8_t)atoi(zminKey + 11);
        }
        const char* zmaxKey = strstr(nameEnd, "\"zoom_max\":");
        if (zmaxKey && zmaxKey < nameEnd + 300) {
            layer.zoom_max = (uint8_t)atoi(zmaxKey + 11);
        }

        // Parse bounds array [lat_min, lon_min, lat_max, lon_max]
        layer.bounds = {0, 0, 0, 0};
        const char* boundsKey = strstr(nameEnd, "\"bounds\":");
        if (boundsKey && boundsKey < nameEnd + 400) {
            const char* arr = strchr(boundsKey + 9, '[');
            if (arr) {
                arr++;
                layer.bounds.lat_min = strtod(arr, nullptr);
                const char* c1 = strchr(arr, ',');
                if (c1) {
                    layer.bounds.lon_min = strtod(c1 + 1, nullptr);
                    const char* c2 = strchr(c1 + 1, ',');
                    if (c2) {
                        layer.bounds.lat_max = strtod(c2 + 1, nullptr);
                        const char* c3 = strchr(c2 + 1, ',');
                        if (c3) {
                            layer.bounds.lon_max = strtod(c3 + 1, nullptr);
                        }
                    }
                }
            }
        }

        // Count tiles by scanning directory (sets tile_count)
        scanLayerDir(layer.name);

        _layerCount++;
        p = nameEnd + 1;
    }

    free(json);
    DBG_INFO(TAG, "Loaded manifest: %d layers", _layerCount);
    return _layerCount > 0;
}

bool GisHAL::scanLayerDir(const char* layer_name) {
    char dirPath[256];
    snprintf(dirPath, sizeof(dirPath), "%s%s/%s",
             _config.sd_mount_point, _config.tile_base_path, layer_name);

    GisLayer* layer = nullptr;
    for (int i = 0; i < _layerCount + 1 && i < MAX_LAYERS; i++) {
        if (strcmp(_layers[i].name, layer_name) == 0) {
            layer = &_layers[i];
            break;
        }
    }
    if (!layer) return false;

    uint32_t count = 0;
    DIR* zDir = opendir(dirPath);
    if (!zDir) {
        DBG_WARN(TAG, "Cannot open layer dir: %s", dirPath);
        layer->tile_count = 0;
        return false;
    }

    struct dirent* zEntry;
    while ((zEntry = readdir(zDir)) != nullptr) {
        if (zEntry->d_name[0] == '.') continue;
        if (zEntry->d_type != DT_DIR) continue;

        char xDirPath[256];
        snprintf(xDirPath, sizeof(xDirPath), "%s/%s", dirPath, zEntry->d_name);
        DIR* xDir = opendir(xDirPath);
        if (!xDir) continue;

        struct dirent* xEntry;
        while ((xEntry = readdir(xDir)) != nullptr) {
            if (xEntry->d_name[0] == '.') continue;
            if (xEntry->d_type != DT_DIR) continue;

            char yDirPath[256];
            snprintf(yDirPath, sizeof(yDirPath), "%s/%s", xDirPath, xEntry->d_name);
            DIR* yDir = opendir(yDirPath);
            if (!yDir) continue;

            struct dirent* yEntry;
            while ((yEntry = readdir(yDir)) != nullptr) {
                if (yEntry->d_name[0] == '.') continue;
                const char* ext = strrchr(yEntry->d_name, '.');
                if (ext && (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0)) {
                    count++;
                }
            }
            closedir(yDir);
        }
        closedir(xDir);
    }
    closedir(zDir);

    layer->tile_count = count;
    DBG_INFO(TAG, "Layer '%s': %u tiles", layer_name, count);
    return true;
}

uint8_t* GisHAL::readTileFromSD(const char* path, size_t& outLen) {
    outLen = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); return nullptr; }

    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) { fclose(f); return nullptr; }

    outLen = fread(buf, 1, sz, f);
    fclose(f);
    return buf;
}

bool GisHAL::hasCoverage(const char* layer, double lat, double lon,
                         uint8_t zoom) const {
    if (!_ready) return false;

    GisLayer gl;
    if (!getLayerByName(layer, gl)) return false;
    if (zoom < gl.zoom_min || zoom > gl.zoom_max) return false;
    if (lat < gl.bounds.lat_min || lat > gl.bounds.lat_max) return false;
    if (lon < gl.bounds.lon_min || lon > gl.bounds.lon_max) return false;

    uint32_t tx = lonToTileX(lon, zoom);
    uint32_t ty = latToTileY(lat, zoom);

    char path[256];
    buildTilePath(path, sizeof(path), layer, zoom, tx, ty);

    struct stat st;
    return stat(path, &st) == 0;
}

uint64_t GisHAL::getStorageUsed() const {
    uint64_t total = 0;
    for (int i = 0; i < _layerCount; i++) {
        char dirPath[256];
        snprintf(dirPath, sizeof(dirPath), "%s%s/%s",
                 _config.sd_mount_point, _config.tile_base_path, _layers[i].name);

        DIR* zDir = opendir(dirPath);
        if (!zDir) continue;

        struct dirent* zEntry;
        while ((zEntry = readdir(zDir)) != nullptr) {
            if (zEntry->d_name[0] == '.' || zEntry->d_type != DT_DIR) continue;
            char xDirPath[256];
            snprintf(xDirPath, sizeof(xDirPath), "%s/%s", dirPath, zEntry->d_name);
            DIR* xDir = opendir(xDirPath);
            if (!xDir) continue;

            struct dirent* xEntry;
            while ((xEntry = readdir(xDir)) != nullptr) {
                if (xEntry->d_name[0] == '.' || xEntry->d_type != DT_DIR) continue;
                char yDirPath[256];
                snprintf(yDirPath, sizeof(yDirPath), "%s/%s", xDirPath, xEntry->d_name);
                DIR* yDir = opendir(yDirPath);
                if (!yDir) continue;

                struct dirent* yEntry;
                while ((yEntry = readdir(yDir)) != nullptr) {
                    if (yEntry->d_name[0] == '.') continue;
                    char filePath[256];
                    snprintf(filePath, sizeof(filePath), "%s/%s",
                             yDirPath, yEntry->d_name);
                    struct stat st;
                    if (stat(filePath, &st) == 0) {
                        total += st.st_size;
                    }
                }
                closedir(yDir);
            }
            closedir(xDir);
        }
        closedir(zDir);
    }
    return total;
}

bool GisHAL::writeManifest() {
    if (!_ready || _layerCount == 0) return false;

    char path[256];
    snprintf(path, sizeof(path), "%s%s/manifest.json",
             _config.sd_mount_point, _config.tile_base_path);

    FILE* f = fopen(path, "w");
    if (!f) {
        DBG_ERROR(TAG, "Cannot write manifest: %s", path);
        return false;
    }

    fprintf(f, "{\n  \"layers\": [\n");
    for (int i = 0; i < _layerCount; i++) {
        const GisLayer& l = _layers[i];
        const char* typeStr = "custom";
        switch (l.type) {
            case GisLayerType::STREETS:   typeStr = "streets"; break;
            case GisLayerType::SATELLITE: typeStr = "satellite"; break;
            case GisLayerType::TERRAIN:   typeStr = "terrain"; break;
            default: break;
        }
        fprintf(f, "    {\n");
        fprintf(f, "      \"name\": \"%s\",\n", l.name);
        fprintf(f, "      \"type\": \"%s\",\n", typeStr);
        fprintf(f, "      \"zoom_min\": %u,\n", l.zoom_min);
        fprintf(f, "      \"zoom_max\": %u,\n", l.zoom_max);
        fprintf(f, "      \"tile_count\": %u,\n", l.tile_count);
        fprintf(f, "      \"bounds\": [%.6f, %.6f, %.6f, %.6f]\n",
                l.bounds.lat_min, l.bounds.lon_min,
                l.bounds.lat_max, l.bounds.lon_max);
        fprintf(f, "    }%s\n", (i < _layerCount - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);

    DBG_INFO(TAG, "Wrote manifest: %d layers", _layerCount);
    return true;
}

GisHAL::TestResult GisHAL::runTest() {
    TestResult r = {};
    uint32_t startMs = sim_millis();

    r.init_ok = _ready;
    r.layer_count = _layerCount;
    r.tile_count = getTileCount();
    r.storage_bytes = getStorageUsed();

    if (_layerCount > 0) {
        GisLayer& l = _layers[0];
        uint32_t tx = lonToTileX(
            (l.bounds.lon_min + l.bounds.lon_max) / 2.0, l.zoom_min);
        uint32_t ty = latToTileY(
            (l.bounds.lat_min + l.bounds.lat_max) / 2.0, l.zoom_min);

        uint32_t t0 = sim_millis();
        size_t len = 0;
        uint8_t* data = getTile(l.name, l.zoom_min, tx, ty, len);
        r.tile_read_ms = sim_millis() - t0;
        r.tile_read_ok = (data != nullptr && len > 0);
        free(data);

        double centerLat = (l.bounds.lat_min + l.bounds.lat_max) / 2.0;
        double centerLon = (l.bounds.lon_min + l.bounds.lon_max) / 2.0;
        r.coverage_check_ok = hasCoverage(l.name, centerLat, centerLon, l.zoom_min);
    }

    r.test_duration_ms = sim_millis() - startMs;

    DBG_INFO(TAG, "--- GIS Test Complete ---");
    DBG_INFO(TAG, "Layers: %d, Tiles: %u, Storage: %llu bytes",
             r.layer_count, r.tile_count, (unsigned long long)r.storage_bytes);
    DBG_INFO(TAG, "Tile read: %s (%u ms), Coverage: %s",
             r.tile_read_ok ? "OK" : "FAIL", r.tile_read_ms,
             r.coverage_check_ok ? "OK" : "FAIL");

    return r;
}

// ============================================================================
// ESP32 — platform-specific methods
// ============================================================================
#else

#include "tritium_compat.h"
// SD card via VFS — use POSIX file ops on /sdcard/
// Filesystem via VFS — use POSIX file ops
#include <dirent.h>
#include <sys/stat.h>

#ifndef HAS_SDCARD
#define HAS_SDCARD 0
#endif

bool GisHAL::init(const GisConfig& config) {
    _config = config;
    if (!_config.sd_mount_point) _config.sd_mount_point = "/sdcard";
    if (!_config.tile_base_path) _config.tile_base_path = "/gis";
    if (_config.max_cache_tiles == 0) _config.max_cache_tiles = 16;

#if !HAS_SDCARD
    DBG_WARN(TAG, "No SD card support on this board");
    return false;
#endif

    // Allocate LRU cache in PSRAM
    _cacheSize = (_config.max_cache_tiles > MAX_CACHE)
                     ? MAX_CACHE : _config.max_cache_tiles;
    _cache = (CacheEntry*)heap_caps_calloc(_cacheSize, sizeof(CacheEntry),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_cache) {
        // Fall back to regular malloc
        _cache = (CacheEntry*)calloc(_cacheSize, sizeof(CacheEntry));
    }
    if (!_cache) {
        DBG_ERROR(TAG, "Failed to allocate tile cache");
        return false;
    }

    if (!loadManifest()) {
        DBG_WARN(TAG, "No manifest.json found — no layers available");
    }

    _ready = true;
    DBG_INFO(TAG, "GIS init: %d layers, cache=%d tiles", _layerCount, _cacheSize);
    return true;
}

void GisHAL::deinit() {
    if (_cache) {
        for (int i = 0; i < _cacheSize; i++) {
            free(_cache[i].data);
        }
        free(_cache);
        _cache = nullptr;
    }
    _layerCount = 0;
    _ready = false;
}

bool GisHAL::loadManifest() {
#if !HAS_SDCARD
    return false;
#else
    char path[256];
    snprintf(path, sizeof(path), "%s/gis/manifest.json", _config.sd_mount_point);

    FILE* f = fopen(path, "rb");
    if (!f) {
        DBG_WARN(TAG, "Cannot open manifest: %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz == 0 || sz > 8192) {
        DBG_ERROR(TAG, "Manifest invalid size: %zu", sz);
        fclose(f);
        return false;
    }

    char* json = (char*)malloc(sz + 1);
    if (!json) { fclose(f); return false; }
    fread(json, 1, sz, f);
    json[sz] = '\0';
    fclose(f);

    // Same minimal JSON parsing as simulator version
    _layerCount = 0;
    const char* p = json;

    while (_layerCount < MAX_LAYERS) {
        const char* nameKey = strstr(p, "\"name\":");
        if (!nameKey) break;
        const char* nameStart = strchr(nameKey + 7, '"');
        if (!nameStart) break;
        nameStart++;
        const char* nameEnd = strchr(nameStart, '"');
        if (!nameEnd) break;

        GisLayer& layer = _layers[_layerCount];
        size_t nameLen = (size_t)(nameEnd - nameStart);
        if (nameLen >= sizeof(layer.name)) nameLen = sizeof(layer.name) - 1;
        memcpy(layer.name, nameStart, nameLen);
        layer.name[nameLen] = '\0';

        layer.type = GisLayerType::CUSTOM;
        const char* typeKey = strstr(nameEnd, "\"type\":");
        if (typeKey && typeKey < nameEnd + 200) {
            const char* ts = strchr(typeKey + 7, '"');
            if (ts) {
                ts++;
                if (strncmp(ts, "streets", 7) == 0) layer.type = GisLayerType::STREETS;
                else if (strncmp(ts, "satellite", 9) == 0) layer.type = GisLayerType::SATELLITE;
                else if (strncmp(ts, "terrain", 7) == 0) layer.type = GisLayerType::TERRAIN;
            }
        }

        layer.zoom_min = 0;
        layer.zoom_max = 20;
        const char* zminKey = strstr(nameEnd, "\"zoom_min\":");
        if (zminKey && zminKey < nameEnd + 300) {
            layer.zoom_min = (uint8_t)atoi(zminKey + 11);
        }
        const char* zmaxKey = strstr(nameEnd, "\"zoom_max\":");
        if (zmaxKey && zmaxKey < nameEnd + 300) {
            layer.zoom_max = (uint8_t)atoi(zmaxKey + 11);
        }

        layer.bounds = {0, 0, 0, 0};
        const char* boundsKey = strstr(nameEnd, "\"bounds\":");
        if (boundsKey && boundsKey < nameEnd + 400) {
            const char* arr = strchr(boundsKey + 9, '[');
            if (arr) {
                arr++;
                layer.bounds.lat_min = strtod(arr, nullptr);
                const char* c1 = strchr(arr, ',');
                if (c1) {
                    layer.bounds.lon_min = strtod(c1 + 1, nullptr);
                    const char* c2 = strchr(c1 + 1, ',');
                    if (c2) {
                        layer.bounds.lat_max = strtod(c2 + 1, nullptr);
                        const char* c3 = strchr(c2 + 1, ',');
                        if (c3) {
                            layer.bounds.lon_max = strtod(c3 + 1, nullptr);
                        }
                    }
                }
            }
        }

        scanLayerDir(layer.name);
        _layerCount++;
        p = nameEnd + 1;
    }

    free(json);
    DBG_INFO(TAG, "Loaded manifest: %d layers", _layerCount);
    return _layerCount > 0;
#endif
}

bool GisHAL::scanLayerDir(const char* layer_name) {
#if !HAS_SDCARD
    return false;
#else
    char dirPath[256];
    snprintf(dirPath, sizeof(dirPath), "%s%s/%s",
             _config.sd_mount_point, _config.tile_base_path, layer_name);

    GisLayer* layer = nullptr;
    for (int i = 0; i < _layerCount + 1 && i < MAX_LAYERS; i++) {
        if (strcmp(_layers[i].name, layer_name) == 0) {
            layer = &_layers[i];
            break;
        }
    }
    if (!layer) return false;

    uint32_t count = 0;

    DIR* zRoot = opendir(dirPath);
    if (!zRoot) {
        layer->tile_count = 0;
        return false;
    }

    struct dirent* zEntry;
    while ((zEntry = readdir(zRoot)) != nullptr) {
        if (zEntry->d_type == DT_DIR && zEntry->d_name[0] != '.') {
            char xPath[384];
            snprintf(xPath, sizeof(xPath), "%s/%s", dirPath, zEntry->d_name);
            DIR* xDir = opendir(xPath);
            if (!xDir) continue;
            struct dirent* xEntry;
            while ((xEntry = readdir(xDir)) != nullptr) {
                if (xEntry->d_type == DT_DIR && xEntry->d_name[0] != '.') {
                    char yPath[512];
                    snprintf(yPath, sizeof(yPath), "%s/%s", xPath, xEntry->d_name);
                    DIR* yDir = opendir(yPath);
                    if (!yDir) continue;
                    struct dirent* yEntry;
                    while ((yEntry = readdir(yDir)) != nullptr) {
                        if (yEntry->d_type != DT_DIR) {
                            const char* ext = strrchr(yEntry->d_name, '.');
                            if (ext && (strcmp(ext, ".png") == 0 ||
                                        strcmp(ext, ".jpg") == 0)) {
                                count++;
                            }
                        }
                    }
                    closedir(yDir);
                }
            }
            closedir(xDir);
        }
    }
    closedir(zRoot);

    layer->tile_count = count;
    DBG_INFO(TAG, "Layer '%s': %u tiles", layer_name, count);
    return true;
#endif
}

uint8_t* GisHAL::readTileFromSD(const char* path, size_t& outLen) {
    outLen = 0;
#if !HAS_SDCARD
    return nullptr;
#else
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;

    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz == 0) { fclose(f); return nullptr; }

    // Allocate in PSRAM for large tiles
    uint8_t* buf = nullptr;
    if (sz > 4096) {
        buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        buf = (uint8_t*)malloc(sz);
    }
    if (!buf) { fclose(f); return nullptr; }

    outLen = fread(buf, 1, sz, f);
    fclose(f);
    return buf;
#endif
}

bool GisHAL::hasCoverage(const char* layer, double lat, double lon,
                         uint8_t zoom) const {
#if !HAS_SDCARD
    return false;
#else
    if (!_ready) return false;

    GisLayer gl;
    if (!getLayerByName(layer, gl)) return false;
    if (zoom < gl.zoom_min || zoom > gl.zoom_max) return false;
    if (lat < gl.bounds.lat_min || lat > gl.bounds.lat_max) return false;
    if (lon < gl.bounds.lon_min || lon > gl.bounds.lon_max) return false;

    uint32_t tx = lonToTileX(lon, zoom);
    uint32_t ty = latToTileY(lat, zoom);

    char path[256];
    buildTilePath(path, sizeof(path), layer, zoom, tx, ty);
    struct stat _st;
    return (stat(path, &_st) == 0);
#endif
}

uint64_t GisHAL::getStorageUsed() const {
    // On ESP32, walking the tree is expensive. Return tile_count * avg tile
    // size estimate (15 KB for PNG street tiles) as a heuristic.
    // A more accurate value could be cached in manifest.json.
    uint64_t total = 0;
    for (int i = 0; i < _layerCount; i++) {
        total += (uint64_t)_layers[i].tile_count * 15000;
    }
    return total;
}

bool GisHAL::writeManifest() {
#if !HAS_SDCARD
    return false;
#else
    if (!_ready || _layerCount == 0) return false;

    char path[256];
    snprintf(path, sizeof(path), "%s%s/manifest.json",
             _config.sd_mount_point, _config.tile_base_path);

    FILE* f = fopen(path, "wb");
    if (!f) {
        DBG_ERROR(TAG, "Cannot write manifest: %s", path);
        return false;
    }

    // Build JSON in a buffer to avoid many small fwrite() calls
    char buf[2048];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\n  \"layers\": [\n");

    for (int i = 0; i < _layerCount; i++) {
        const GisLayer& l = _layers[i];
        const char* typeStr = "custom";
        switch (l.type) {
            case GisLayerType::STREETS:   typeStr = "streets"; break;
            case GisLayerType::SATELLITE: typeStr = "satellite"; break;
            case GisLayerType::TERRAIN:   typeStr = "terrain"; break;
            default: break;
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "    {\n"
            "      \"name\": \"%s\",\n"
            "      \"type\": \"%s\",\n"
            "      \"zoom_min\": %u,\n"
            "      \"zoom_max\": %u,\n"
            "      \"tile_count\": %u,\n"
            "      \"bounds\": [%.6f, %.6f, %.6f, %.6f]\n"
            "    }%s\n",
            l.name, typeStr, l.zoom_min, l.zoom_max, l.tile_count,
            l.bounds.lat_min, l.bounds.lon_min,
            l.bounds.lat_max, l.bounds.lon_max,
            (i < _layerCount - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "  ]\n}\n");

    fwrite(buf, 1, pos, f);
    fclose(f);

    DBG_INFO(TAG, "Wrote manifest: %d layers", _layerCount);
    return true;
#endif
}

GisHAL::TestResult GisHAL::runTest() {
    TestResult r = {};
    uint32_t startMs = millis();

    r.init_ok = _ready;
    r.layer_count = _layerCount;
    r.tile_count = getTileCount();
    r.storage_bytes = getStorageUsed();

    if (_layerCount > 0) {
        GisLayer& l = _layers[0];
        uint32_t tx = lonToTileX(
            (l.bounds.lon_min + l.bounds.lon_max) / 2.0, l.zoom_min);
        uint32_t ty = latToTileY(
            (l.bounds.lat_min + l.bounds.lat_max) / 2.0, l.zoom_min);

        uint32_t t0 = millis();
        size_t len = 0;
        uint8_t* data = getTile(l.name, l.zoom_min, tx, ty, len);
        r.tile_read_ms = millis() - t0;
        r.tile_read_ok = (data != nullptr && len > 0);
        free(data);

        double centerLat = (l.bounds.lat_min + l.bounds.lat_max) / 2.0;
        double centerLon = (l.bounds.lon_min + l.bounds.lon_max) / 2.0;
        r.coverage_check_ok = hasCoverage(l.name, centerLat, centerLon, l.zoom_min);
    }

    r.test_duration_ms = millis() - startMs;

    DBG_INFO(TAG, "--- GIS Test Complete ---");
    DBG_INFO(TAG, "Layers: %d, Tiles: %u, Storage: %llu bytes",
             r.layer_count, r.tile_count, (unsigned long long)r.storage_bytes);
    DBG_INFO(TAG, "Tile read: %s (%u ms), Coverage: %s",
             r.tile_read_ok ? "OK" : "FAIL", r.tile_read_ms,
             r.coverage_check_ok ? "OK" : "FAIL");

    return r;
}

#endif // SIMULATOR

// ============================================================================
// Shared methods — identical on simulator and ESP32
// ============================================================================

uint8_t* GisHAL::cacheLookup(const char* layer, uint8_t z, uint32_t x,
                              uint32_t y, size_t& outLen) {
    if (!_cache) return nullptr;
    for (int i = 0; i < _cacheSize; i++) {
        CacheEntry& e = _cache[i];
        if (e.data && e.zoom == z && e.x == x && e.y == y &&
            strcmp(e.layer, layer) == 0) {
            e.last_access = ++_accessCounter;
            // Return a copy so caller can free() it independently
            uint8_t* copy = (uint8_t*)malloc(e.len);
            if (!copy) return nullptr;
            memcpy(copy, e.data, e.len);
            outLen = e.len;
            return copy;
        }
    }
    return nullptr;
}

void GisHAL::cacheInsert(const char* layer, uint8_t z, uint32_t x, uint32_t y,
                          const uint8_t* data, size_t len) {
    if (!_cache || !data || len == 0) return;

    // Find empty slot or LRU slot
    int slot = 0;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < _cacheSize; i++) {
        if (!_cache[i].data) {
            slot = i;
            break;
        }
        if (_cache[i].last_access < oldest) {
            oldest = _cache[i].last_access;
            slot = i;
        }
    }

    // Evict old entry
    free(_cache[slot].data);

    // Store copy
    _cache[slot].data = (uint8_t*)malloc(len);
    if (!_cache[slot].data) return;
    memcpy(_cache[slot].data, data, len);
    _cache[slot].len = len;
    _cache[slot].zoom = z;
    _cache[slot].x = x;
    _cache[slot].y = y;
    _cache[slot].last_access = ++_accessCounter;
    strncpy(_cache[slot].layer, layer, sizeof(_cache[slot].layer) - 1);
    _cache[slot].layer[sizeof(_cache[slot].layer) - 1] = '\0';
}

int GisHAL::getLayerCount() const {
    return _layerCount;
}

bool GisHAL::getLayer(int index, GisLayer& out) const {
    if (index < 0 || index >= _layerCount) return false;
    out = _layers[index];
    return true;
}

bool GisHAL::getLayerByName(const char* name, GisLayer& out) const {
    for (int i = 0; i < _layerCount; i++) {
        if (strcmp(_layers[i].name, name) == 0) {
            out = _layers[i];
            return true;
        }
    }
    return false;
}

uint8_t* GisHAL::getTile(const char* layer, uint8_t z, uint32_t x, uint32_t y,
                          size_t& outLen) {
    outLen = 0;
    if (!_ready) return nullptr;

    // Check cache first
    uint8_t* cached = cacheLookup(layer, z, x, y, outLen);
    if (cached) {
        DBG_DEBUG(TAG, "Cache hit: %s/%u/%u/%u", layer, z, x, y);
        return cached;
    }

    // Read from SD
    char path[256];
    buildTilePath(path, sizeof(path), layer, z, x, y);

    uint8_t* data = readTileFromSD(path, outLen);
    if (!data) {
        // Try .jpg fallback
        size_t pathLen = strlen(path);
        if (pathLen > 4) {
            path[pathLen - 4] = '\0';
            strcat(path, ".jpg");
            data = readTileFromSD(path, outLen);
        }
    }

    if (data) {
        cacheInsert(layer, z, x, y, data, outLen);
        DBG_DEBUG(TAG, "Loaded tile: %s/%u/%u/%u (%zu bytes)",
                  layer, z, x, y, outLen);
    }

    return data;
}

uint32_t GisHAL::getTileCount() const {
    uint32_t total = 0;
    for (int i = 0; i < _layerCount; i++) {
        total += _layers[i].tile_count;
    }
    return total;
}

int GisHAL::importMBTiles(const char* mbtiles_path, const char* layer_name,
                          GisProgressCallback progress) {
    // MBTiles import requires SQLite — on ESP32 this would use the sqlite3
    // component. The MBTiles format stores tiles in a SQLite DB:
    //   CREATE TABLE tiles (zoom_level int, tile_column int, tile_row int,
    //                       tile_data blob);
    //   CREATE TABLE metadata (name text, value text);
    // TMS y-axis is flipped vs OSM: osm_y = (1 << zoom) - 1 - tms_y
    //
    // Recommended workflow: run extraction on a desktop machine, copy flat
    // files to SD card, then let the ESP32 use them directly.
    DBG_WARN(TAG, "MBTiles import not yet implemented (needs SQLite component)");
    (void)mbtiles_path;
    (void)layer_name;
    (void)progress;
    return -1;
}

int GisHAL::exportRegion(const char* layer, const GisBounds& bounds,
                         uint8_t zoom_min, uint8_t zoom_max,
                         GisTileCallback callback) {
    if (!_ready || !callback) return -1;

    int exported = 0;

    for (uint8_t z = zoom_min; z <= zoom_max; z++) {
        uint32_t xMin = lonToTileX(bounds.lon_min, z);
        uint32_t xMax = lonToTileX(bounds.lon_max, z);
        uint32_t yMin = latToTileY(bounds.lat_max, z);  // lat_max -> smaller y
        uint32_t yMax = latToTileY(bounds.lat_min, z);

        for (uint32_t x = xMin; x <= xMax; x++) {
            for (uint32_t y = yMin; y <= yMax; y++) {
                size_t len = 0;
                uint8_t* data = getTile(layer, z, x, y, len);
                if (data) {
                    GisTile tile;
                    tile.zoom = z;
                    tile.x = x;
                    tile.y = y;
                    tile.size_bytes = (uint32_t)len;
                    buildTilePath(tile.path, sizeof(tile.path), layer, z, x, y);
                    callback(tile, data, len);
                    free(data);
                    exported++;
                }
            }
        }
    }

    DBG_INFO(TAG, "Exported %d tiles from '%s'", exported, layer);
    return exported;
}
