#pragma once
// Seed HAL -- self-replicating firmware payload manager.
//
// A tiny ESP32 can't run a vision model, but with an SD card it carries
// firmware, models, configs -- even the source code. When it meets a Jetson
// (or another ESP32), it seeds it.
//
// Seed payloads live on SD card under /tritium/:
//   /tritium/manifest.json   -- file list, checksums, versions
//   /tritium/firmware.bin    -- firmware binary for OTA seeding
//   /tritium/config/         -- device config files
//   /tritium/models/         -- ML models, voice templates, etc.
//   /tritium/source/         -- optional source code archive
//
// For nodes without SD cards, a minimal config-only seed uses LittleFS.
//
// Web integration: hal_webserver can expose:
//   GET  /api/seed/manifest           -- returns manifest JSON
//   GET  /api/seed/download?file=X    -- streams a seed file
//   POST /api/seed/import             -- upload a seed package
//
// Usage:
//   #include "hal_seed.h"
//   SeedHAL seed;
//   seed.init(&sd, &fs);  // pass SD and/or LittleFS HAL
//   if (seed.hasSeedPayload()) {
//       auto manifest = seed.getManifest();
//       seed.serveFile("/tritium/firmware.bin", [](const uint8_t* data, size_t len) {
//           // send chunk to peer
//       });
//   }

#include <cstdint>
#include <cstddef>
#include <functional>

class SDCardHAL;
class FsHAL;

// Maximum entries in a seed manifest
static constexpr int SEED_MAX_FILES = 32;

// SHA-256 hash as 64-char hex string + null
static constexpr int SEED_HASH_LEN = 65;

// Individual file entry in the manifest
struct SeedFileEntry {
    char path[128];              // e.g. "/tritium/firmware.bin"
    char sha256[SEED_HASH_LEN]; // hex-encoded SHA-256
    uint32_t size;               // file size in bytes
};

// Seed manifest -- describes what this node carries
struct SeedManifest {
    char firmware_version[32];   // e.g. "1.2.0"
    char board_type[32];         // e.g. "touch-lcd-35bc"
    char node_id[24];            // MAC or unique ID
    uint32_t created_epoch;      // unix timestamp when packaged
    int file_count;              // number of entries in files[]
    SeedFileEntry files[SEED_MAX_FILES];
};

// Callback for streaming file data in chunks
using SeedChunkCb = std::function<bool(const uint8_t* data, size_t len)>;

// Callback for seed transfer progress (0-100)
using SeedProgressCb = std::function<void(uint8_t percent)>;

class SeedHAL {
public:
    // Initialize with storage backends. Either or both may be nullptr.
    // SD card is preferred for large payloads; LittleFS for config-only seeds.
    bool init(SDCardHAL* sd = nullptr, FsHAL* fs = nullptr);

    // Check if this node carries a valid seed payload
    bool hasSeedPayload() const;

    // Get the current manifest (returns false if no manifest found)
    bool getManifest(SeedManifest& out) const;

    // Get manifest as JSON string. Caller must free() the returned buffer.
    char* getManifestJson() const;

    // Stream a file to a requester in chunks. Returns false on error.
    // The callback receives chunks of data; return false from callback to abort.
    bool serveFile(const char* path, SeedChunkCb cb, size_t chunkSize = 4096) const;

    // Import firmware.bin from SD card into the seed directory.
    // Copies the running firmware partition to /tritium/firmware.bin on SD.
    bool importFirmware();

    // Import a config file into the seed config directory
    bool importConfig(const char* srcPath, const char* name);

    // Create a complete seed package from current state:
    // - Exports running firmware to /tritium/firmware.bin
    // - Copies config from LittleFS to /tritium/config/
    // - Computes SHA-256 checksums for all files
    // - Writes /tritium/manifest.json
    bool createSeedPackage();

    // Validate manifest checksums against actual files on disk.
    // Returns the number of files that pass verification (out of file_count).
    int verifyChecksums() const;

    // Set progress callback for long operations (import, package creation)
    void onProgress(SeedProgressCb cb);

    // Compute SHA-256 of a file. Result written as hex string to `out`.
    // out must be at least SEED_HASH_LEN (65) bytes.
    bool hashFile(const char* path, char* out) const;

    // Storage info
    bool hasSD() const { return _sd != nullptr; }
    bool hasFS() const { return _fs != nullptr; }

private:
    SDCardHAL* _sd = nullptr;
    FsHAL* _fs = nullptr;
    bool _initialized = false;
    SeedProgressCb _progressCb = nullptr;

    // Read a file from whichever backend has it (SD preferred)
    char* _readFile(const char* path, size_t& outLen) const;

    // Write a file to SD (preferred) or LittleFS
    bool _writeFile(const char* path, const char* data, size_t len);

    // Ensure /tritium/ directory structure exists
    bool _ensureDirs();

    // Report progress if callback is set
    void _reportProgress(uint8_t percent) const;

    // Parse manifest JSON into struct
    bool _parseManifest(const char* json, SeedManifest& out) const;

    // Serialize manifest struct to JSON. Caller must free().
    char* _serializeManifest(const SeedManifest& m) const;
};
