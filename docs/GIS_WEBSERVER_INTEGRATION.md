# GIS + WebServer Integration — Offline Map Viewer

## Overview

The `hal_gis` library serves OSM slippy-map tiles from the SD card. Combined with
`hal_webserver`, this enables an offline Leaflet.js map viewer accessible from any
browser on the local network — no internet required.

## Web API Endpoints

Add these routes to `hal_webserver` (in `addGisEndpoints()` or similar):

### GET /api/gis/layers

Returns JSON array of available map layers:

```json
{
  "layers": [
    {
      "name": "streets",
      "type": "streets",
      "zoom_min": 10,
      "zoom_max": 16,
      "tile_count": 4200,
      "bounds": [40.700, -74.020, 40.800, -73.930]
    }
  ]
}
```

### GET /api/gis/tile/{layer}/{z}/{x}/{y}.png

Serves a single tile image. Returns 200 with `image/png` or `image/jpeg` content
type, or 404 if the tile is not available. The LRU cache in PSRAM ensures recently
accessed tiles are served quickly without repeated SD card reads.

### GET /api/gis/stats

Returns tile count, storage used, cache hit rate.

### GET /map

Serves the offline map viewer HTML page (see below).

## Leaflet.js Offline Map Viewer

The map viewer is a single HTML page served from LittleFS (or embedded as a
PROGMEM string). It uses Leaflet.js loaded from LittleFS as well — no CDN needed.

### Required files on LittleFS

```
/www/leaflet.js        (180 KB minified)
/www/leaflet.css       (15 KB)
/www/map.html          (the viewer page, ~3 KB)
```

### map.html structure

```html
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel="stylesheet" href="/www/leaflet.css" />
    <script src="/www/leaflet.js"></script>
    <style>
        body { margin: 0; }
        #map { height: 100vh; width: 100vw; }
    </style>
</head>
<body>
    <div id="map"></div>
    <script>
        // Fetch available layers, then init map with first layer
        fetch('/api/gis/layers')
            .then(r => r.json())
            .then(data => {
                const layer = data.layers[0];
                const bounds = [
                    [layer.bounds[0], layer.bounds[1]],
                    [layer.bounds[2], layer.bounds[3]]
                ];
                const map = L.map('map').fitBounds(bounds);
                L.tileLayer('/api/gis/tile/' + layer.name + '/{z}/{x}/{y}.png', {
                    minZoom: layer.zoom_min,
                    maxZoom: layer.zoom_max,
                    attribution: 'Offline tiles via Tritium'
                }).addTo(map);

                // Layer switcher if multiple layers exist
                if (data.layers.length > 1) {
                    const baseMaps = {};
                    data.layers.forEach(l => {
                        baseMaps[l.name] = L.tileLayer(
                            '/api/gis/tile/' + l.name + '/{z}/{x}/{y}.png',
                            { minZoom: l.zoom_min, maxZoom: l.zoom_max }
                        );
                    });
                    L.control.layers(baseMaps).addTo(map);
                }
            });
    </script>
</body>
</html>
```

## WebServer Integration Code

In `hal_webserver.cpp`, add a method like:

```cpp
void WebServerHAL::addGisEndpoints(GisHAL& gis) {
    // Layer list
    _server->on("/api/gis/layers", HTTP_GET, [&gis]() {
        char json[1024];
        int pos = snprintf(json, sizeof(json), "{\"layers\":[");
        for (int i = 0; i < gis.getLayerCount(); i++) {
            GisLayer l;
            gis.getLayer(i, l);
            pos += snprintf(json + pos, sizeof(json) - pos,
                "%s{\"name\":\"%s\",\"type\":\"%s\","
                "\"zoom_min\":%u,\"zoom_max\":%u,"
                "\"tile_count\":%u,"
                "\"bounds\":[%.6f,%.6f,%.6f,%.6f]}",
                i > 0 ? "," : "", l.name, /* type string */, ...);
        }
        snprintf(json + pos, sizeof(json) - pos, "]}");
        _server->send(200, "application/json", json);
    });

    // Tile serving
    _server->on("/api/gis/tile/*", HTTP_GET, [&gis]() {
        // Parse {layer}/{z}/{x}/{y}.png from URI
        String uri = _server->uri();
        // ... parse layer, z, x, y from path segments ...
        size_t len;
        uint8_t* data = gis.getTile(layer, z, x, y, len);
        if (data) {
            _server->send_P(200, "image/png", (const char*)data, len);
            free(data);
        } else {
            _server->send(404, "text/plain", "Tile not found");
        }
    });
}
```

## SD Card Tile Preparation

Tiles must be prepared on a desktop machine and copied to the SD card.

### Using osmium + tile-generation tools

```bash
# Option 1: Download pre-rendered tiles from a tile server
# Use a tool like maps-dl or JTileDownloader to bulk-download OSM tiles
# for a specific bounding box and zoom range.

# Option 2: Convert MBTiles to flat files
# MBTiles is a standard SQLite-based tile package format.
# Many tools export to MBTiles: TileMill, tippecanoe, MapTiler, etc.
python3 scripts/mbtiles_to_osm.py input.mbtiles /path/to/sdcard/gis/streets

# Option 3: Use QGIS MBTiles export
# QGIS can export any map to MBTiles format, then extract.
```

### SD card layout

```
/sdcard/gis/
  manifest.json
  streets/
    14/
      8192/
        5461.png
        5462.png
      8193/
        5461.png
        5462.png
    15/
      ...
  satellite/
    12/
      ...
```

### Storage estimates

| Area | Zoom Range | Tile Count | Approx Size |
|------|-----------|------------|-------------|
| City block | 14-18 | ~500 | 8 MB |
| Neighborhood | 12-17 | ~5,000 | 75 MB |
| Small city | 10-16 | ~20,000 | 300 MB |
| Metro area | 8-14 | ~50,000 | 750 MB |

Street tiles average ~15 KB/tile (PNG). Satellite tiles are larger (~30 KB JPEG).
A 32 GB SD card can hold a full metro area at multiple zoom levels.

## Node-to-Node Tile Sharing

The `exportRegion()` method streams tiles via callback, making it suitable for
sharing map data between Tritium nodes over ESP-NOW or LoRa:

```cpp
gis.exportRegion("streets", myBounds, 12, 16, [](const GisTile& tile,
    const uint8_t* data, size_t len) {
    // Fragment and send via ESP-NOW mesh
    espnow.sendFragmented(targetMac, data, len, tile.path);
});
```

This enables the "self-replicating data" vision: nodes carry more map data than
they can process, and share tiles with nearby nodes that need coverage for their
current location.

## Related Projects

| Project | Description | Relevance |
|---------|-------------|-----------|
| [IceNav-v3](https://github.com/jgauchia/IceNav-v3) | ESP32-S3 GPS navigator with offline OSM tiles. Uses LovyanGFX + LVGL 9, PSRAM tile cache, SD card storage in `{z}/{x}/{y}.png` format. | Same tile format, same display stack. Their mass-copy script and Maperitive tile generation workflow (zoom 6-17) are directly applicable. |
| [OpenStreetMap-esp32](https://github.com/CelliesProjects/OpenStreetMap-esp32) | PlatformIO library for online OSM tile fetching with PSRAM LRU cache and LovyanGFX sprite output. | Could serve as online tile fetcher when WiFi is available — download tiles on first access, cache to SD. Dual-core tile decode. |
| [ESP32_GPS](https://github.com/aresta/ESP32_GPS) | GPS device with custom vector map format from OSM PBF extracts on SD card. | Vector approach uses less storage than raster tiles. Worth evaluating for low-zoom overview maps. |

### IceNav Tile Compatibility

Our `hal_gis` tile path format (`{layer}/{z}/{x}/{y}.png`) is compatible with
IceNav's tile storage. Tiles prepared for IceNav work with Tritium by placing them
under the appropriate layer directory (e.g., `/sdcard/gis/streets/`). The
`manifest.json` can be regenerated with `gis.writeManifest()` after adding tiles.
