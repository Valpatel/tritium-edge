#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

// Firmware image header — prepended to firmware.bin for validation
//
// Version 1 (unsigned): 64 bytes
//   [magic(4)][hdr_ver(2)][flags(2)][fw_size(4)][fw_crc32(4)]
//   [version(24)][board(16)][reserved(8)]
//
// Version 2 (signed): 128 bytes = 64-byte v1 header + 64-byte signature
//   Same as v1 plus:
//   [signature_r(32)][signature_s(32)]
//   Signature covers: firmware data only (not header)
//   Algorithm: ECDSA P-256 (secp256r1) with SHA-256

struct __attribute__((packed)) OtaFirmwareHeader {
    uint32_t magic;          // 0x4154304F ('OT0A')
    uint16_t header_version; // 1 = unsigned, 2 = signed
    uint16_t flags;          // Bit 0: signed, Bit 1: encrypted
    uint32_t firmware_size;  // Size of firmware data (after header)
    uint32_t firmware_crc32; // CRC32 of firmware data
    char     version[24];    // Semantic version string (null-terminated)
    char     board[16];      // Target board identifier (e.g., "touch-lcd-349")
    uint8_t  reserved[8];    // Reserved for future use

    static constexpr uint32_t MAGIC = 0x4154304F;  // 'OT0A' in little-endian
    static constexpr uint16_t HEADER_VER_UNSIGNED = 1;
    static constexpr uint16_t HEADER_VER_SIGNED = 2;
    static constexpr uint16_t FLAG_SIGNED = 0x01;
    static constexpr uint16_t FLAG_ENCRYPTED = 0x02;

    bool isValid() const {
        return magic == MAGIC &&
               (header_version == HEADER_VER_UNSIGNED ||
                header_version == HEADER_VER_SIGNED);
    }

    bool isSigned() const {
        return header_version == HEADER_VER_SIGNED && (flags & FLAG_SIGNED);
    }

    bool isEncrypted() const {
        return (flags & FLAG_ENCRYPTED) != 0;
    }

    // Total header size including signature (if present)
    size_t totalHeaderSize() const {
        return isSigned() ? 128 : 64;
    }
};

// Signature block appended after the 64-byte header in v2
struct __attribute__((packed)) OtaSignature {
    uint8_t r[32];  // ECDSA P-256 signature r component
    uint8_t s[32];  // ECDSA P-256 signature s component
};

static_assert(sizeof(OtaFirmwareHeader) == 64, "OTA header must be 64 bytes");
static_assert(sizeof(OtaSignature) == 64, "OTA signature must be 64 bytes");

// Simple semver comparison: returns -1 (a<b), 0 (a==b), 1 (a>b)
// Parses "major.minor.patch[-suffix]" where suffix is ignored for comparison
// Falls back to strcmp if format doesn't match
inline int otaVersionCompare(const char* a, const char* b) {
    int aMaj = 0, aMin = 0, aPat = 0;
    int bMaj = 0, bMin = 0, bPat = 0;
    int aFields = sscanf(a, "%d.%d.%d", &aMaj, &aMin, &aPat);
    int bFields = sscanf(b, "%d.%d.%d", &bMaj, &bMin, &bPat);
    if (aFields < 2 || bFields < 2) {
        // Not semver, fall back to string comparison
        int r = strcmp(a, b);
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    }
    if (aMaj != bMaj) return aMaj < bMaj ? -1 : 1;
    if (aMin != bMin) return aMin < bMin ? -1 : 1;
    if (aPat != bPat) return aPat < bPat ? -1 : 1;
    return 0;
}
