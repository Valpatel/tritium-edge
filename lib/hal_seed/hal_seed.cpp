#include "hal_seed.h"
#include "hal_sdcard.h"
#include "hal_fs.h"
#include "debug_log.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>

static constexpr const char* TAG = "seed";

// Seed directory layout on SD card
static constexpr const char* SEED_ROOT       = "/tritium";
static constexpr const char* SEED_MANIFEST   = "/tritium/manifest.json";
static constexpr const char* SEED_FW_PATH    = "/tritium/firmware.bin";
static constexpr const char* SEED_CONFIG_DIR = "/tritium/config";
static constexpr const char* SEED_MODELS_DIR = "/tritium/models";

// ============================================================================
// Shared helpers (both ESP32 and simulator)
// ============================================================================

bool SeedHAL::init(SDCardHAL* sd, FsHAL* fs) {
    _sd = sd;
    _fs = fs;

    if (!_sd && !_fs) {
        DBG_ERROR(TAG, "No storage backend provided");
        return false;
    }

    if (_sd && !_sd->isMounted()) {
        DBG_WARN(TAG, "SD card provided but not mounted");
        _sd = nullptr;
    }

    if (_fs && !_fs->isReady()) {
        DBG_WARN(TAG, "LittleFS provided but not ready");
        _fs = nullptr;
    }

    if (!_sd && !_fs) {
        DBG_ERROR(TAG, "No usable storage backend");
        return false;
    }

    _initialized = true;
    DBG_INFO(TAG, "Seed HAL initialized (SD=%s, LittleFS=%s)",
             _sd ? "yes" : "no", _fs ? "yes" : "no");

    if (hasSeedPayload()) {
        SeedManifest m;
        if (getManifest(m)) {
            DBG_INFO(TAG, "Seed payload found: v%s, %d files, board=%s",
                     m.firmware_version, m.file_count, m.board_type);
        }
    } else {
        DBG_INFO(TAG, "No seed payload on this node");
    }

    return true;
}

bool SeedHAL::hasSeedPayload() const {
    if (!_initialized) return false;
    if (_sd && _sd->exists(SEED_MANIFEST)) return true;
    if (_fs && _fs->exists(SEED_MANIFEST)) return true;
    return false;
}

bool SeedHAL::getManifest(SeedManifest& out) const {
    if (!_initialized) return false;

    size_t len = 0;
    char* json = _readFile(SEED_MANIFEST, len);
    if (!json) {
        DBG_WARN(TAG, "Cannot read manifest");
        return false;
    }

    bool ok = _parseManifest(json, out);
    free(json);
    return ok;
}

char* SeedHAL::getManifestJson() const {
    if (!_initialized) return nullptr;

    size_t len = 0;
    char* json = _readFile(SEED_MANIFEST, len);
    return json;  // caller must free
}

void SeedHAL::onProgress(SeedProgressCb cb) {
    _progressCb = cb;
}

void SeedHAL::_reportProgress(uint8_t percent) const {
    if (_progressCb) _progressCb(percent);
}

// ---------------------------------------------------------------------------
// File abstraction -- read/write through whichever backend is available
// ---------------------------------------------------------------------------

char* SeedHAL::_readFile(const char* path, size_t& outLen) const {
    outLen = 0;

    // Try SD card first (larger storage, preferred for seed payloads)
    if (_sd && _sd->exists(path)) {
        char* data = _sd->readFile(path, outLen);
        if (data) return data;
    }

    // Fall back to LittleFS
    if (_fs && _fs->exists(path)) {
        size_t fsize = _fs->fileSize(path);
        if (fsize == 0) return nullptr;
        char* buf = (char*)malloc(fsize + 1);
        if (!buf) return nullptr;
        size_t bytesRead = 0;
        if (_fs->readFile(path, buf, fsize, &bytesRead)) {
            buf[bytesRead] = '\0';
            outLen = bytesRead;
            return buf;
        }
        free(buf);
    }

    return nullptr;
}

bool SeedHAL::_writeFile(const char* path, const char* data, size_t len) {
    // Prefer SD for large files
    if (_sd) {
        // SDCardHAL::writeFile takes null-terminated string; for binary data
        // we need the data to be null-terminated or use a different approach.
        // For manifest JSON and small configs this is fine.
        return _sd->writeFile(path, data);
    }
    if (_fs) {
        return _fs->writeFile(path, data, len);
    }
    return false;
}

bool SeedHAL::_ensureDirs() {
    if (_sd) {
        _sd->mkdir(SEED_ROOT);
        _sd->mkdir(SEED_CONFIG_DIR);
        _sd->mkdir(SEED_MODELS_DIR);
        return true;
    }
    if (_fs) {
        _fs->mkdir(SEED_ROOT);
        _fs->mkdir(SEED_CONFIG_DIR);
        _fs->mkdir(SEED_MODELS_DIR);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Minimal JSON parser (no external dependency)
// ---------------------------------------------------------------------------

// Find a JSON string value by key. Returns pointer into json, sets outLen.
static const char* jsonFindString(const char* json, const char* key,
                                  size_t& outLen) {
    outLen = 0;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* pos = strstr(json, needle);
    if (!pos) return nullptr;
    pos += strlen(needle);

    // Skip whitespace and colon
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n'))
        pos++;
    if (*pos != '"') return nullptr;
    pos++;  // skip opening quote

    const char* end = strchr(pos, '"');
    if (!end) return nullptr;
    outLen = end - pos;
    return pos;
}

// Find a JSON integer value by key
static bool jsonFindInt(const char* json, const char* key, uint32_t& out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* pos = strstr(json, needle);
    if (!pos) return false;
    pos += strlen(needle);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n'))
        pos++;
    char* end = nullptr;
    long val = strtol(pos, &end, 10);
    if (end == pos) return false;
    out = (uint32_t)val;
    return true;
}

bool SeedHAL::_parseManifest(const char* json, SeedManifest& out) const {
    memset(&out, 0, sizeof(out));

    size_t len;
    const char* val;

    val = jsonFindString(json, "firmware_version", len);
    if (val && len < sizeof(out.firmware_version)) {
        memcpy(out.firmware_version, val, len);
        out.firmware_version[len] = '\0';
    }

    val = jsonFindString(json, "board_type", len);
    if (val && len < sizeof(out.board_type)) {
        memcpy(out.board_type, val, len);
        out.board_type[len] = '\0';
    }

    val = jsonFindString(json, "node_id", len);
    if (val && len < sizeof(out.node_id)) {
        memcpy(out.node_id, val, len);
        out.node_id[len] = '\0';
    }

    jsonFindInt(json, "created_epoch", out.created_epoch);

    uint32_t fc = 0;
    jsonFindInt(json, "file_count", fc);
    out.file_count = (int)fc;
    if (out.file_count > SEED_MAX_FILES) out.file_count = SEED_MAX_FILES;

    // Parse files array -- look for "files" key then iterate objects
    const char* filesArr = strstr(json, "\"files\"");
    if (filesArr) {
        filesArr = strchr(filesArr, '[');
        if (filesArr) {
            filesArr++;  // skip '['
            for (int i = 0; i < out.file_count; i++) {
                // Find next object
                const char* obj = strchr(filesArr, '{');
                if (!obj) break;
                const char* objEnd = strchr(obj, '}');
                if (!objEnd) break;

                // Extract path, sha256, size from this object
                val = jsonFindString(obj, "path", len);
                if (val && len < sizeof(out.files[i].path)) {
                    memcpy(out.files[i].path, val, len);
                    out.files[i].path[len] = '\0';
                }

                val = jsonFindString(obj, "sha256", len);
                if (val && len < SEED_HASH_LEN) {
                    memcpy(out.files[i].sha256, val, len);
                    out.files[i].sha256[len] = '\0';
                }

                jsonFindInt(obj, "size", out.files[i].size);

                filesArr = objEnd + 1;
            }
        }
    }

    return out.firmware_version[0] != '\0';
}

char* SeedHAL::_serializeManifest(const SeedManifest& m) const {
    // Estimate buffer size: header ~256 + per-file ~256
    size_t bufSize = 512 + (size_t)m.file_count * 256;
    char* buf = (char*)malloc(bufSize);
    if (!buf) return nullptr;

    int pos = snprintf(buf, bufSize,
        "{\n"
        "  \"firmware_version\": \"%s\",\n"
        "  \"board_type\": \"%s\",\n"
        "  \"node_id\": \"%s\",\n"
        "  \"created_epoch\": %u,\n"
        "  \"file_count\": %d,\n"
        "  \"files\": [\n",
        m.firmware_version,
        m.board_type,
        m.node_id,
        (unsigned)m.created_epoch,
        m.file_count);

    for (int i = 0; i < m.file_count && i < SEED_MAX_FILES; i++) {
        pos += snprintf(buf + pos, bufSize - pos,
            "    {\"path\": \"%s\", \"sha256\": \"%s\", \"size\": %u}%s\n",
            m.files[i].path,
            m.files[i].sha256,
            (unsigned)m.files[i].size,
            (i < m.file_count - 1) ? "," : "");
    }

    pos += snprintf(buf + pos, bufSize - pos,
        "  ]\n"
        "}\n");

    return buf;
}

// ============================================================================
// Platform: ESP32
// ============================================================================
#ifndef SIMULATOR

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

bool SeedHAL::hashFile(const char* path, char* out) const {
    if (!_initialized || !out) return false;

    // Use SD_MMC directly for streaming hash (avoids loading entire file)
    if (_sd && _sd->exists(path)) {
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) {
            DBG_ERROR(TAG, "hashFile: cannot open %s", path);
            return false;
        }

        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);

        uint8_t chunk[4096];
        size_t total = 0;
        size_t fsize = f.size();
        while (f.available()) {
            size_t n = f.read(chunk, sizeof(chunk));
            if (n == 0) break;
            mbedtls_sha256_update(&ctx, chunk, n);
            total += n;

            // Report progress for large files
            if (fsize > 0) {
                _reportProgress((uint8_t)(total * 100 / fsize));
            }
        }
        f.close();

        uint8_t hash[32];
        mbedtls_sha256_finish(&ctx, hash);
        mbedtls_sha256_free(&ctx);

        // Convert to hex string
        for (int i = 0; i < 32; i++) {
            snprintf(out + i * 2, 3, "%02x", hash[i]);
        }
        out[64] = '\0';

        DBG_DEBUG(TAG, "SHA-256 of %s (%u bytes): %.16s...", path,
                  (unsigned)total, out);
        return true;
    }

    // LittleFS fallback: read entire file (small files only)
    if (_fs && _fs->exists(path)) {
        size_t fsize = _fs->fileSize(path);
        if (fsize == 0) return false;

        char* data = (char*)malloc(fsize);
        if (!data) return false;
        size_t bytesRead = 0;
        if (!_fs->readFile(path, data, fsize, &bytesRead)) {
            free(data);
            return false;
        }

        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, (const uint8_t*)data, bytesRead);
        uint8_t hash[32];
        mbedtls_sha256_finish(&ctx, hash);
        mbedtls_sha256_free(&ctx);
        free(data);

        for (int i = 0; i < 32; i++) {
            snprintf(out + i * 2, 3, "%02x", hash[i]);
        }
        out[64] = '\0';
        return true;
    }

    return false;
}

bool SeedHAL::serveFile(const char* path, SeedChunkCb cb,
                         size_t chunkSize) const {
    if (!_initialized || !cb) return false;

    // Stream from SD card
    if (_sd && _sd->exists(path)) {
        File f = SD_MMC.open(path, FILE_READ);
        if (!f) {
            DBG_ERROR(TAG, "serveFile: cannot open %s", path);
            return false;
        }

        uint8_t* chunk = (uint8_t*)malloc(chunkSize);
        if (!chunk) {
            f.close();
            return false;
        }

        size_t total = 0;
        size_t fsize = f.size();
        bool ok = true;

        while (f.available() && ok) {
            size_t n = f.read(chunk, chunkSize);
            if (n == 0) break;
            ok = cb(chunk, n);
            total += n;
            if (fsize > 0) {
                _reportProgress((uint8_t)(total * 100 / fsize));
            }
        }

        free(chunk);
        f.close();
        DBG_INFO(TAG, "Served %s: %u bytes, %s", path, (unsigned)total,
                 ok ? "complete" : "aborted");
        return ok;
    }

    // LittleFS fallback
    if (_fs && _fs->exists(path)) {
        size_t len = 0;
        char* data = _readFile(path, len);
        if (!data) return false;

        bool ok = true;
        size_t offset = 0;
        while (offset < len && ok) {
            size_t n = (len - offset > chunkSize) ? chunkSize : (len - offset);
            ok = cb((const uint8_t*)data + offset, n);
            offset += n;
            if (len > 0) {
                _reportProgress((uint8_t)(offset * 100 / len));
            }
        }
        free(data);
        return ok;
    }

    DBG_WARN(TAG, "serveFile: %s not found", path);
    return false;
}

bool SeedHAL::importFirmware() {
    if (!_initialized || !_sd) {
        DBG_ERROR(TAG, "importFirmware requires SD card");
        return false;
    }

    _ensureDirs();
    _reportProgress(0);

    // Read the running OTA partition and write it to SD card
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        DBG_ERROR(TAG, "Cannot determine running partition");
        return false;
    }

    DBG_INFO(TAG, "Exporting firmware from partition '%s' (%u bytes)",
             running->label, (unsigned)running->size);

    // Open output file on SD
    File out = SD_MMC.open(SEED_FW_PATH, FILE_WRITE);
    if (!out) {
        DBG_ERROR(TAG, "Cannot create %s on SD card", SEED_FW_PATH);
        return false;
    }

    // Stream partition data to SD in chunks
    static constexpr size_t CHUNK_SIZE = 4096;
    uint8_t* chunk = (uint8_t*)malloc(CHUNK_SIZE);
    if (!chunk) {
        out.close();
        return false;
    }

    size_t total = running->size;
    size_t written = 0;
    bool ok = true;

    for (size_t offset = 0; offset < total && ok; offset += CHUNK_SIZE) {
        size_t n = (total - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total - offset);
        esp_err_t err = esp_partition_read(running, offset, chunk, n);
        if (err != ESP_OK) {
            DBG_ERROR(TAG, "Partition read error at offset %u: 0x%x",
                      (unsigned)offset, err);
            ok = false;
            break;
        }

        // Check for end of firmware (0xFF padding means we're past the image)
        // Stop early if we hit a page of all 0xFF
        bool allFF = true;
        for (size_t i = 0; i < n; i++) {
            if (chunk[i] != 0xFF) { allFF = false; break; }
        }
        if (allFF && offset > 0x10000) {
            DBG_INFO(TAG, "Detected end of firmware at offset %u", (unsigned)offset);
            break;
        }

        size_t w = out.write(chunk, n);
        if (w != n) {
            DBG_ERROR(TAG, "SD write error at offset %u", (unsigned)offset);
            ok = false;
            break;
        }
        written += w;
        _reportProgress((uint8_t)(offset * 100 / total));
    }

    free(chunk);
    out.close();

    _reportProgress(100);
    DBG_INFO(TAG, "Firmware exported: %u bytes to %s", (unsigned)written,
             SEED_FW_PATH);
    return ok;
}

bool SeedHAL::importConfig(const char* srcPath, const char* name) {
    if (!_initialized) return false;
    _ensureDirs();

    // Read from LittleFS
    if (!_fs || !_fs->exists(srcPath)) {
        DBG_WARN(TAG, "Config source not found: %s", srcPath);
        return false;
    }

    size_t len = 0;
    char* data = _readFile(srcPath, len);
    if (!data) return false;

    // Write to seed config directory
    char destPath[192];
    snprintf(destPath, sizeof(destPath), "%s/%s", SEED_CONFIG_DIR, name);
    bool ok = _writeFile(destPath, data, len);
    free(data);

    if (ok) {
        DBG_INFO(TAG, "Config imported: %s -> %s", srcPath, destPath);
    }
    return ok;
}

bool SeedHAL::createSeedPackage() {
    if (!_initialized) {
        DBG_ERROR(TAG, "Not initialized");
        return false;
    }

    DBG_INFO(TAG, "Creating seed package...");
    _reportProgress(0);
    _ensureDirs();

    SeedManifest m;
    memset(&m, 0, sizeof(m));

    // Get firmware version from running app
    const esp_app_desc_t* desc = esp_ota_get_app_description();
    if (desc && desc->version[0] != '\0') {
        strncpy(m.firmware_version, desc->version, sizeof(m.firmware_version) - 1);
    } else {
        strncpy(m.firmware_version, "unknown", sizeof(m.firmware_version) - 1);
    }

    // Board type from build flag
#if defined(BOARD_TOUCH_LCD_35BC)
    strncpy(m.board_type, "touch-lcd-35bc", sizeof(m.board_type) - 1);
#elif defined(BOARD_TOUCH_LCD_349)
    strncpy(m.board_type, "touch-lcd-349", sizeof(m.board_type) - 1);
#elif defined(BOARD_TOUCH_AMOLED_241B)
    strncpy(m.board_type, "touch-amoled-241b", sizeof(m.board_type) - 1);
#elif defined(BOARD_TOUCH_AMOLED_18)
    strncpy(m.board_type, "touch-amoled-18", sizeof(m.board_type) - 1);
#elif defined(BOARD_AMOLED_191M)
    strncpy(m.board_type, "amoled-191m", sizeof(m.board_type) - 1);
#elif defined(BOARD_TOUCH_LCD_43C_BOX)
    strncpy(m.board_type, "touch-lcd-43c-box", sizeof(m.board_type) - 1);
#else
    strncpy(m.board_type, "unknown", sizeof(m.board_type) - 1);
#endif

    // Node ID from MAC address
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(m.node_id, sizeof(m.node_id), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Timestamp (0 if no NTP sync)
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    m.created_epoch = (uint32_t)tv.tv_sec;

    _reportProgress(10);

    // Step 1: Export firmware
    if (_sd) {
        DBG_INFO(TAG, "Exporting firmware to SD...");
        if (importFirmware()) {
            // Add firmware entry
            int idx = m.file_count;
            strncpy(m.files[idx].path, SEED_FW_PATH, sizeof(m.files[idx].path) - 1);

            // Get file size
            File f = SD_MMC.open(SEED_FW_PATH, FILE_READ);
            if (f) {
                m.files[idx].size = f.size();
                f.close();
            }

            // Compute checksum
            hashFile(SEED_FW_PATH, m.files[idx].sha256);
            m.file_count++;
            DBG_INFO(TAG, "Firmware: %u bytes, sha256=%.16s...",
                     (unsigned)m.files[idx].size, m.files[idx].sha256);
        } else {
            DBG_WARN(TAG, "Firmware export failed, continuing without it");
        }
    }

    _reportProgress(50);

    // Step 2: Copy config files from LittleFS to SD
    if (_fs && _sd) {
        static const char* configFiles[] = {
            "/config.json",
            "/wifi.json",
            "/device.json",
            nullptr
        };

        for (int i = 0; configFiles[i] != nullptr; i++) {
            if (_fs->exists(configFiles[i]) && m.file_count < SEED_MAX_FILES) {
                const char* filename = configFiles[i] + 1;  // skip leading '/'
                if (importConfig(configFiles[i], filename)) {
                    int idx = m.file_count;
                    snprintf(m.files[idx].path, sizeof(m.files[idx].path),
                             "%s/%s", SEED_CONFIG_DIR, filename);
                    m.files[idx].size = _fs->fileSize(configFiles[i]);
                    hashFile(m.files[idx].path, m.files[idx].sha256);
                    m.file_count++;
                }
            }
        }
    }

    _reportProgress(80);

    // Step 3: Write manifest
    char* json = _serializeManifest(m);
    if (!json) {
        DBG_ERROR(TAG, "Failed to serialize manifest");
        return false;
    }

    bool ok = _writeFile(SEED_MANIFEST, json, strlen(json));
    free(json);

    _reportProgress(100);

    if (ok) {
        DBG_INFO(TAG, "Seed package created: v%s, %d files, node=%s",
                 m.firmware_version, m.file_count, m.node_id);
    } else {
        DBG_ERROR(TAG, "Failed to write manifest");
    }

    return ok;
}

int SeedHAL::verifyChecksums() const {
    if (!_initialized) return 0;

    SeedManifest m;
    if (!getManifest(m)) return 0;

    int verified = 0;
    char computed[SEED_HASH_LEN];

    for (int i = 0; i < m.file_count; i++) {
        if (hashFile(m.files[i].path, computed)) {
            if (strcmp(computed, m.files[i].sha256) == 0) {
                verified++;
                DBG_INFO(TAG, "  PASS: %s", m.files[i].path);
            } else {
                DBG_WARN(TAG, "  FAIL: %s (expected %.16s..., got %.16s...)",
                         m.files[i].path, m.files[i].sha256, computed);
            }
        } else {
            DBG_WARN(TAG, "  MISS: %s (file not found)", m.files[i].path);
        }
    }

    DBG_INFO(TAG, "Checksum verification: %d/%d passed", verified, m.file_count);
    return verified;
}

// ============================================================================
// Platform: Simulator
// ============================================================================
#else

#include <cstdio>
#include <sys/stat.h>

bool SeedHAL::hashFile(const char* path, char* out) const {
    // Simulator stub: generate a deterministic fake hash from the path
    if (!out) return false;

    size_t len = 0;
    char* data = _readFile(path, len);
    if (!data) return false;

    // Simple hash for simulator testing (not cryptographic)
    uint32_t h = 0x811c9dc5;  // FNV-1a offset basis
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 0x01000193;
    }
    free(data);

    // Fill 64 hex chars with repeating pattern from hash
    for (int i = 0; i < 8; i++) {
        snprintf(out + i * 8, 9, "%08x", h ^ (uint32_t)i);
    }
    out[64] = '\0';
    return true;
}

bool SeedHAL::serveFile(const char* path, SeedChunkCb cb,
                         size_t chunkSize) const {
    if (!_initialized || !cb) return false;

    size_t len = 0;
    char* data = _readFile(path, len);
    if (!data) return false;

    bool ok = true;
    size_t offset = 0;
    while (offset < len && ok) {
        size_t n = (len - offset > chunkSize) ? chunkSize : (len - offset);
        ok = cb((const uint8_t*)data + offset, n);
        offset += n;
        if (len > 0) {
            _reportProgress((uint8_t)(offset * 100 / len));
        }
    }
    free(data);
    return ok;
}

bool SeedHAL::importFirmware() {
    DBG_WARN(TAG, "importFirmware not supported in simulator");
    return false;
}

bool SeedHAL::importConfig(const char* srcPath, const char* name) {
    if (!_initialized) return false;
    _ensureDirs();

    size_t len = 0;
    char* data = _readFile(srcPath, len);
    if (!data) return false;

    char destPath[192];
    snprintf(destPath, sizeof(destPath), "%s/%s", SEED_CONFIG_DIR, name);
    bool ok = _writeFile(destPath, data, len);
    free(data);
    return ok;
}

bool SeedHAL::createSeedPackage() {
    DBG_WARN(TAG, "createSeedPackage: limited in simulator (no OTA partition)");

    if (!_initialized) return false;
    _ensureDirs();

    SeedManifest m;
    memset(&m, 0, sizeof(m));
    strncpy(m.firmware_version, "sim-1.0.0", sizeof(m.firmware_version) - 1);
    strncpy(m.board_type, "simulator", sizeof(m.board_type) - 1);
    strncpy(m.node_id, "SIM:00:00:00:00:00", sizeof(m.node_id) - 1);
    m.created_epoch = 0;

    char* json = _serializeManifest(m);
    if (!json) return false;

    bool ok = _writeFile(SEED_MANIFEST, json, strlen(json));
    free(json);
    return ok;
}

int SeedHAL::verifyChecksums() const {
    if (!_initialized) return 0;

    SeedManifest m;
    if (!getManifest(m)) return 0;

    int verified = 0;
    char computed[SEED_HASH_LEN];

    for (int i = 0; i < m.file_count; i++) {
        if (hashFile(m.files[i].path, computed)) {
            if (strcmp(computed, m.files[i].sha256) == 0) {
                verified++;
            }
        }
    }
    return verified;
}

#endif // SIMULATOR
