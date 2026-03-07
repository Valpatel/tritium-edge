# Created by Matthew Valancy
# Copyright 2026 Valpatel Software LLC
# Licensed under AGPL-3.0 — see LICENSE for details.
"""OTA header parsing and firmware utilities.

Matches the binary format defined in lib/hal_ota/ota_header.h.
Designed for extraction into tritium-lib as shared OTA utilities.
"""

import binascii
import struct

OTA_MAGIC = 0x4154304F  # 'OT0A' little-endian
OTA_HEADER_SIZE = 64
OTA_SIG_SIZE = 64


def parse_ota_header(data: bytes) -> dict | None:
    """Parse OTA header from binary data. Returns metadata dict or None."""
    if len(data) < OTA_HEADER_SIZE:
        return None
    magic, hdr_ver, flags, fw_size, fw_crc = struct.unpack('<IHHII', data[:16])
    if magic != OTA_MAGIC:
        return None
    version = data[16:40].split(b'\x00')[0].decode(errors='replace')
    board = data[40:56].split(b'\x00')[0].decode(errors='replace')
    is_signed = (hdr_ver == 2) and bool(flags & 0x01)
    is_encrypted = bool(flags & 0x02)
    # Build timestamp from reserved bytes
    build_ts = struct.unpack('<I', data[56:60])[0]
    return {
        "firmware_size": fw_size,
        "firmware_crc32": fw_crc,
        "version": version,
        "board": board,
        "signed": is_signed,
        "encrypted": is_encrypted,
        "build_timestamp": build_ts if build_ts > 1600000000 else None,
        "total_header_size": OTA_HEADER_SIZE + (OTA_SIG_SIZE if is_signed else 0),
    }


def compute_crc32(data: bytes) -> int:
    """Compute CRC32 matching the ESP32 firmware implementation."""
    return binascii.crc32(data) & 0xFFFFFFFF
