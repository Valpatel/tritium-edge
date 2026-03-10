#!/usr/bin/env python3
"""
Download OpenStreetMap tiles and package them into MBTiles for Tritium-OS.

Downloads raster tiles from OSM tile servers and creates a compact SQLite
(MBTiles) database suitable for the ESP32 map viewer.

Usage:
    # Download California at zoom levels 8-12 (small, ~5MB)
    python3 tools/download_tiles.py --region california --zoom 8-12

    # Download a custom bounding box
    python3 tools/download_tiles.py --bbox "36.5,-122.5,38.0,-121.0" --zoom 10-14

    # Download San Francisco area at higher detail
    python3 tools/download_tiles.py --region sf --zoom 10-15

Output: sdcard_data/map.mbtiles
Copy to SD card: cp sdcard_data/map.mbtiles /sdcard/data/map.mbtiles
"""

import argparse
import math
import os
import sqlite3
import sys
import time
import urllib.request

# Predefined regions
REGIONS = {
    "california": (32.5, -124.5, 42.0, -114.0),  # Full state
    "sf": (37.6, -122.55, 37.85, -122.35),        # San Francisco
    "la": (33.7, -118.7, 34.3, -118.1),           # Los Angeles
    "sd": (32.5, -117.3, 33.0, -116.9),           # San Diego
    "bay": (37.2, -122.6, 37.9, -121.8),          # SF Bay Area
    "socal": (32.5, -119.0, 34.5, -116.0),        # Southern California
    "norcal": (37.0, -124.0, 42.0, -119.5),       # Northern California
}

OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "..", "sdcard_data")
OUTPUT_DB = os.path.join(OUTPUT_DIR, "map.mbtiles")

# OSM tile server (respect usage policy: max 2 req/sec, include User-Agent)
TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
USER_AGENT = "Tritium-OS/1.0 (ESP32 offline map viewer; github.com/Valpatel/tritium-edge)"
REQUEST_DELAY = 0.5  # seconds between requests (be nice to OSM servers)


def lon_to_tile_x(lon: float, zoom: int) -> int:
    return int(math.floor((lon + 180.0) / 360.0 * (1 << zoom)))


def lat_to_tile_y(lat: float, zoom: int) -> int:
    lat_rad = math.radians(lat)
    return int(math.floor((1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * (1 << zoom)))


def count_tiles(south: float, west: float, north: float, east: float,
                zoom_min: int, zoom_max: int) -> int:
    total = 0
    for z in range(zoom_min, zoom_max + 1):
        x_min = lon_to_tile_x(west, z)
        x_max = lon_to_tile_x(east, z)
        y_min = lat_to_tile_y(north, z)  # Note: y is inverted
        y_max = lat_to_tile_y(south, z)
        total += (x_max - x_min + 1) * (y_max - y_min + 1)
    return total


def download_tiles(south: float, west: float, north: float, east: float,
                   zoom_min: int, zoom_max: int, db_path: str):
    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    if os.path.exists(db_path):
        os.remove(db_path)

    conn = sqlite3.connect(db_path)
    cur = conn.cursor()

    # MBTiles schema
    cur.execute("""
        CREATE TABLE metadata (name TEXT, value TEXT)
    """)
    cur.execute("""
        CREATE TABLE tiles (
            zoom_level INTEGER, tile_column INTEGER, tile_row INTEGER,
            tile_data BLOB
        )
    """)
    cur.execute("""
        CREATE UNIQUE INDEX tile_index ON tiles (zoom_level, tile_column, tile_row)
    """)

    # Metadata
    center_lat = (south + north) / 2
    center_lon = (west + east) / 2
    center_zoom = (zoom_min + zoom_max) // 2
    metadata = {
        "name": "Tritium-OS Map",
        "format": "png",
        "minzoom": str(zoom_min),
        "maxzoom": str(zoom_max),
        "bounds": f"{west},{south},{east},{north}",
        "center": f"{center_lon},{center_lat},{center_zoom}",
        "type": "baselayer",
        "attribution": "© OpenStreetMap contributors",
        "description": f"Offline map tiles for Tritium-OS ({zoom_min}-{zoom_max})",
    }
    for k, v in metadata.items():
        cur.execute("INSERT INTO metadata VALUES (?, ?)", (k, v))

    total = count_tiles(south, west, north, east, zoom_min, zoom_max)
    print(f"Downloading {total:,} tiles (zoom {zoom_min}-{zoom_max})...")
    print(f"Bounds: {south:.4f},{west:.4f} to {north:.4f},{east:.4f}")
    print(f"Center: {center_lat:.4f},{center_lon:.4f} @ z{center_zoom}")
    print(f"Estimated time: ~{total * REQUEST_DELAY / 60:.0f} minutes")
    print()

    downloaded = 0
    errors = 0
    total_bytes = 0

    for z in range(zoom_min, zoom_max + 1):
        x_min = lon_to_tile_x(west, z)
        x_max = lon_to_tile_x(east, z)
        y_min = lat_to_tile_y(north, z)
        y_max = lat_to_tile_y(south, z)

        z_tiles = (x_max - x_min + 1) * (y_max - y_min + 1)
        print(f"Zoom {z}: {z_tiles} tiles ({x_min}-{x_max} x {y_min}-{y_max})")

        for x in range(x_min, x_max + 1):
            for y in range(y_min, y_max + 1):
                url = TILE_URL.format(z=z, x=x, y=y)
                # MBTiles uses TMS y-coordinate (flipped)
                tms_y = (1 << z) - 1 - y

                try:
                    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
                    with urllib.request.urlopen(req, timeout=10) as resp:
                        tile_data = resp.read()
                        cur.execute("INSERT INTO tiles VALUES (?, ?, ?, ?)",
                                    (z, x, tms_y, tile_data))
                        total_bytes += len(tile_data)
                        downloaded += 1
                except Exception as e:
                    errors += 1
                    if errors <= 5:
                        print(f"  Error: {url}: {e}")
                    elif errors == 6:
                        print("  (suppressing further errors...)")

                if downloaded % 50 == 0:
                    conn.commit()
                    pct = downloaded / total * 100
                    print(f"  {downloaded:,}/{total:,} ({pct:.1f}%) — {total_bytes / 1024:.0f} KB")

                time.sleep(REQUEST_DELAY)

    conn.commit()
    cur.execute("ANALYZE")
    conn.execute("VACUUM")
    conn.commit()
    conn.close()

    db_size = os.path.getsize(db_path)
    print(f"\nDone! {downloaded:,} tiles downloaded ({errors} errors)")
    print(f"Output: {db_path} ({db_size / 1024 / 1024:.1f} MB)")
    print(f"Copy to SD card: cp {db_path} /sdcard/data/map.mbtiles")


def main():
    parser = argparse.ArgumentParser(description="Download OSM tiles to MBTiles for Tritium-OS")
    parser.add_argument("--region", choices=list(REGIONS.keys()),
                        help="Predefined region name")
    parser.add_argument("--bbox", type=str,
                        help="Custom bounding box: 'south,west,north,east'")
    parser.add_argument("--zoom", type=str, default="10-13",
                        help="Zoom range: 'min-max' (default: 10-13)")
    parser.add_argument("--output", type=str, default=OUTPUT_DB,
                        help=f"Output path (default: {OUTPUT_DB})")
    parser.add_argument("--delay", type=float, default=REQUEST_DELAY,
                        help=f"Delay between requests in seconds (default: {REQUEST_DELAY})")
    parser.add_argument("--dry-run", action="store_true",
                        help="Count tiles without downloading")
    args = parser.parse_args()

    if args.region:
        south, west, north, east = REGIONS[args.region]
    elif args.bbox:
        parts = [float(x.strip()) for x in args.bbox.split(",")]
        if len(parts) != 4:
            print("Error: --bbox must be 'south,west,north,east'")
            sys.exit(1)
        south, west, north, east = parts
    else:
        print("Predefined regions:")
        for name, (s, w, n, e) in REGIONS.items():
            z_min, z_max = 10, 13
            cnt = count_tiles(s, w, n, e, z_min, z_max)
            print(f"  {name:12s}: {cnt:>8,} tiles at z{z_min}-{z_max}")
        print("\nUsage: --region <name> or --bbox 'south,west,north,east'")
        sys.exit(0)

    zoom_parts = args.zoom.split("-")
    zoom_min = int(zoom_parts[0])
    zoom_max = int(zoom_parts[1]) if len(zoom_parts) > 1 else zoom_min

    global REQUEST_DELAY
    REQUEST_DELAY = args.delay

    total = count_tiles(south, west, north, east, zoom_min, zoom_max)
    est_mb = total * 15 / 1024  # ~15KB avg per tile
    est_min = total * REQUEST_DELAY / 60

    print(f"Region: ({south},{west}) to ({north},{east})")
    print(f"Zoom: {zoom_min}-{zoom_max}")
    print(f"Tiles: {total:,}")
    print(f"Estimated size: ~{est_mb:.0f} MB")
    print(f"Estimated time: ~{est_min:.0f} minutes")

    if args.dry_run:
        print("\n(Dry run — no tiles downloaded)")
        return

    if total > 10000:
        print(f"\nWARNING: {total:,} tiles is a lot. OSM tile servers have usage limits.")
        resp = input("Continue? [y/N] ").strip().lower()
        if resp != "y":
            print("Aborted.")
            return

    print()
    download_tiles(south, west, north, east, zoom_min, zoom_max, args.output)


if __name__ == "__main__":
    main()
