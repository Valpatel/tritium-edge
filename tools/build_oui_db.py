#!/usr/bin/env python3
"""
Build OUI SQLite database for Tritium-OS SD card.

Downloads the IEEE OUI (MA-L) CSV and converts it to a compact SQLite
database optimized for ESP32 lookup. Output goes to sdcard_data/oui.db.

Usage:
    python3 tools/build_oui_db.py

The resulting oui.db should be copied to /sdcard/data/oui.db on the device.
"""

import csv
import sqlite3
import os
import sys
import urllib.request

OUI_CSV_URL = "https://standards-oui.ieee.org/oui/oui.csv"
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "..", "sdcard_data")
OUTPUT_DB = os.path.join(OUTPUT_DIR, "oui.db")
CACHE_CSV = "/tmp/oui.csv"


def download_oui():
    """Download OUI CSV if not cached."""
    if os.path.exists(CACHE_CSV):
        size = os.path.getsize(CACHE_CSV)
        if size > 1_000_000:  # >1MB means it's probably valid
            print(f"Using cached {CACHE_CSV} ({size:,} bytes)")
            return CACHE_CSV

    print(f"Downloading {OUI_CSV_URL}...")
    urllib.request.urlretrieve(OUI_CSV_URL, CACHE_CSV)
    print(f"Downloaded {os.path.getsize(CACHE_CSV):,} bytes")
    return CACHE_CSV


def build_db(csv_path: str, db_path: str):
    """Parse OUI CSV and build SQLite database."""
    os.makedirs(os.path.dirname(db_path), exist_ok=True)

    if os.path.exists(db_path):
        os.remove(db_path)

    conn = sqlite3.connect(db_path)
    cur = conn.cursor()

    # Compact schema — prefix is 6-char hex (e.g. "4C1DBE"), name is truncated to 48 chars
    cur.execute("""
        CREATE TABLE oui (
            prefix TEXT PRIMARY KEY,
            name TEXT NOT NULL
        ) WITHOUT ROWID
    """)

    count = 0
    with open(csv_path, "r", encoding="utf-8", errors="replace") as f:
        reader = csv.DictReader(f)
        for row in reader:
            registry = row.get("Registry", "")
            if registry != "MA-L":
                continue  # Only large (24-bit) assignments

            prefix = row.get("Assignment", "").strip().upper()
            name = row.get("Organization Name", "").strip().strip('"')

            if not prefix or not name:
                continue

            # Truncate long names to save space
            if len(name) > 48:
                name = name[:45] + "..."

            cur.execute("INSERT OR IGNORE INTO oui VALUES (?, ?)", (prefix, name))
            count += 1

    conn.commit()

    # Optimize for read-only use on ESP32
    cur.execute("ANALYZE")
    cur.execute("PRAGMA journal_mode=DELETE")  # Clean up WAL if any
    cur.close()
    conn.execute("VACUUM")
    conn.commit()
    conn.close()

    db_size = os.path.getsize(db_path)
    print(f"Built {db_path}: {count:,} entries, {db_size:,} bytes ({db_size/1024:.0f} KB)")


def print_stats(db_path: str):
    """Print some interesting stats from the database."""
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()

    cur.execute("SELECT COUNT(*) FROM oui")
    total = cur.fetchone()[0]

    # Top manufacturers by number of OUI assignments
    cur.execute("""
        SELECT name, COUNT(*) as cnt FROM oui
        GROUP BY name ORDER BY cnt DESC LIMIT 10
    """)
    print(f"\nTop 10 manufacturers (by OUI count, {total} total):")
    for name, cnt in cur.fetchall():
        print(f"  {cnt:4d}  {name}")

    # Common BLE device prefixes
    known = {
        "4C1DBE": "Apple", "DCCF96": "Apple", "3CE072": "Apple",
        "EC1F72": "Samsung", "8425DB": "Samsung",
        "F4F5D8": "Google", "F4F5E8": "Google",
        "74C246": "Amazon",
    }
    print("\nKnown BLE device prefixes in DB:")
    for prefix, expected in known.items():
        cur.execute("SELECT name FROM oui WHERE prefix=?", (prefix,))
        row = cur.fetchone()
        status = f"✓ {row[0]}" if row else "✗ NOT FOUND"
        print(f"  {prefix} ({expected}): {status}")

    conn.close()


if __name__ == "__main__":
    csv_path = download_oui()
    build_db(csv_path, OUTPUT_DB)
    print_stats(OUTPUT_DB)
    print(f"\nCopy to SD card: cp {OUTPUT_DB} /sdcard/data/oui.db")
