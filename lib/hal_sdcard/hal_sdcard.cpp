#include "hal_sdcard.h"
#include "debug_log.h"

static constexpr const char* TAG = "sdcard";

// Mount point used for VFS registration — all POSIX paths are relative to this.
static constexpr const char* MOUNT_POINT = "/sdcard";

#ifdef SIMULATOR

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include <chrono>

// --- Simulator stub (uses local filesystem in ./sdcard/) ---

bool SDCardHAL::init() {
    _mounted = true;
    return true;
}

void SDCardHAL::deinit() { _mounted = false; }
uint64_t SDCardHAL::totalBytes() { return 4ULL * 1024 * 1024 * 1024; }
uint64_t SDCardHAL::usedBytes() { return 0; }

bool SDCardHAL::exists(const char *path) {
    char buf[256];
    snprintf(buf, sizeof(buf), "./sdcard%s", path);
    struct stat st;
    return stat(buf, &st) == 0;
}

char* SDCardHAL::readFile(const char *path, size_t &outLen) {
    outLen = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "./sdcard%s", path);
    FILE *f = fopen(buf, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = (char*)malloc(sz + 1);
    if (!data) { fclose(f); return nullptr; }
    outLen = fread(data, 1, sz, f);
    data[outLen] = '\0';
    fclose(f);
    return data;
}

bool SDCardHAL::writeFile(const char *path, const char *data) {
    char buf[256];
    snprintf(buf, sizeof(buf), "./sdcard%s", path);
    FILE *f = fopen(buf, "w");
    if (!f) return false;
    fputs(data, f);
    fclose(f);
    return true;
}

bool SDCardHAL::appendFile(const char *path, const char *data) {
    char buf[256];
    snprintf(buf, sizeof(buf), "./sdcard%s", path);
    FILE *f = fopen(buf, "a");
    if (!f) return false;
    fputs(data, f);
    fclose(f);
    return true;
}

bool SDCardHAL::removeFile(const char *path) {
    char buf[256];
    snprintf(buf, sizeof(buf), "./sdcard%s", path);
    return remove(buf) == 0;
}

bool SDCardHAL::listFiles(const char *dirname, uint8_t levels) {
    return false; // not implemented in simulator
}

bool SDCardHAL::format() {
    DBG_WARN(TAG, "format() not supported in simulator");
    return false;
}

bool SDCardHAL::removeRecursive(const char* path) {
    return _removeRecursive(path);
}

bool SDCardHAL::_removeRecursive(const char* path) {
    (void)path;
    DBG_WARN(TAG, "removeRecursive() not supported in simulator");
    return false;
}

bool SDCardHAL::mkdir(const char* path) {
    char buf[256];
    snprintf(buf, sizeof(buf), "./sdcard%s", path);
    return ::mkdir(buf, 0755) == 0;
}

void SDCardHAL::listDir(const char* dir, int maxDepth) {
    char buf[256];
    snprintf(buf, sizeof(buf), "./sdcard%s", dir);
    DIR* d = opendir(buf);
    if (!d) {
        DBG_WARN(TAG, "Cannot open directory: %s", dir);
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type == DT_DIR) {
            DBG_INFO(TAG, "  DIR : %s", entry->d_name);
            if (maxDepth > 0) {
                char subdir[256];
                snprintf(subdir, sizeof(subdir), "%s/%s", dir, entry->d_name);
                listDir(subdir, maxDepth - 1);
            }
        } else {
            DBG_INFO(TAG, "  FILE: %s", entry->d_name);
        }
    }
    closedir(d);
}

const char* SDCardHAL::getFilesystemType() const {
    return "FAT32 (simulated)";
}

SDCardHAL::TestResult SDCardHAL::runTest(int cycles, size_t blockSize) {
    TestResult result = {};
    auto startTime = std::chrono::steady_clock::now();

    DBG_INFO(TAG, "--- SD Card Test Begin (simulator) ---");

    result.mount_ok = _mounted;
    if (!result.mount_ok) {
        DBG_ERROR(TAG, "SD card not mounted");
        auto endTime = std::chrono::steady_clock::now();
        result.test_duration_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        return result;
    }

    result.total_bytes = (size_t)totalBytes();
    result.used_bytes = (size_t)usedBytes();

    // Generate test data pattern
    char* testData = (char*)malloc(blockSize);
    char* readBuf = (char*)malloc(blockSize);
    if (!testData || !readBuf) {
        free(testData);
        free(readBuf);
        DBG_ERROR(TAG, "Failed to allocate test buffers");
        return result;
    }
    for (size_t i = 0; i < blockSize; i++) {
        testData[i] = (char)(i & 0xFF);
    }

    result.mkdir_ok = mkdir("/test_dir");
    DBG_INFO(TAG, "mkdir /test_dir: %s", result.mkdir_ok ? "OK" : "FAIL");

    size_t totalWritten = 0;
    size_t totalRead = 0;
    auto writeStart = std::chrono::steady_clock::now();

    for (int i = 0; i < cycles; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/test_dir/test_%d.bin", i);

        bool wOk = writeFile(path, testData);
        result.write_ok = result.write_ok || wOk;
        if (wOk) totalWritten += blockSize;

        size_t readLen = 0;
        char* rData = readFile(path, readLen);
        if (rData) {
            result.read_ok = true;
            totalRead += readLen;
            if (readLen == blockSize && memcmp(rData, testData, blockSize) == 0) {
                result.verify_ok = true;
            }
            free(rData);
        }

        bool dOk = removeFile(path);
        result.delete_ok = result.delete_ok || dOk;

        result.cycles_completed = i + 1;
        DBG_INFO(TAG, "Cycle %d/%d: write=%s read=%s verify=%s delete=%s",
                 i + 1, cycles, wOk ? "OK" : "FAIL",
                 rData ? "OK" : "FAIL", result.verify_ok ? "OK" : "FAIL",
                 dOk ? "OK" : "FAIL");
    }

    auto writeEnd = std::chrono::steady_clock::now();
    uint32_t elapsedUs = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(writeEnd - writeStart).count();
    if (elapsedUs > 0) {
        result.write_speed_kbps = (uint32_t)(totalWritten * 1000000ULL / elapsedUs / 1024);
        result.read_speed_kbps = (uint32_t)(totalRead * 1000000ULL / elapsedUs / 1024);
    }

    // Clean up test directory
    removeFile("/test_dir");

    free(testData);
    free(readBuf);

    auto endTime = std::chrono::steady_clock::now();
    result.test_duration_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    DBG_INFO(TAG, "--- SD Card Test Complete ---");
    DBG_INFO(TAG, "Write speed: %u KB/s, Read speed: %u KB/s", result.write_speed_kbps, result.read_speed_kbps);
    DBG_INFO(TAG, "Duration: %u ms, Cycles: %d", result.test_duration_ms, result.cycles_completed);

    return result;
}

#else // ESP32

#include "tritium_compat.h"

#ifndef HAS_SDCARD
#define HAS_SDCARD 0
#endif

#if HAS_SDCARD

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"
#include "driver/sdmmc_host.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

// CH422G IO expander support for boards that need pin setup before SD init
// (e.g., 43C-BOX: EXIO4 = SD D3 pull-up for SDMMC mode)
//
// CH422G protocol:
//   0x24 = config address (write 0x01 to enable I/O mode)
//   0x38 = output address (write single byte = pin states, no register addr)
//
// Pin assignments on 43C-BOX:
//   EXIO1 = Touch RST (must stay HIGH)
//   EXIO2 = Backlight  (must stay HIGH)
//   EXIO4 = SD D3 pull-up (must be HIGH for SDMMC mode)
#if defined(SD_CS_VIA_EXPANDER) && SD_CS_VIA_EXPANDER
    #include "tritium_i2c.h"
    static constexpr uint8_t CH422G_SET_ADDR   = 0x24;
    static constexpr uint8_t CH422G_WR_IO_ADDR = 0x38;
    static constexpr uint8_t CH422G_RD_IO_ADDR = 0x38;

    static void ch422g_drive_pin_high(uint8_t pin) {
        // Ensure I/O mode is enabled
        i2c0.beginTransmission(CH422G_SET_ADDR);
        i2c0.wireWrite(0x01);
        i2c0.endTransmission();

        // Read current state to preserve other pins (backlight, touch RST)
        i2c0.requestFrom((uint8_t)CH422G_RD_IO_ADDR, (uint8_t)1);
        uint8_t state = i2c0.wireAvailable() ? (uint8_t)i2c0.wireRead() : 0x00;
        DBG_INFO(TAG, "CH422G read state: 0x%02X", state);

        // Ensure critical pins stay high + set target pin high
        state |= (1 << 1) | (1 << 2);  // EXIO1=RST, EXIO2=BL
        state |= (1 << pin);            // Target pin HIGH

        i2c0.beginTransmission(CH422G_WR_IO_ADDR);
        i2c0.wireWrite(state);
        i2c0.endTransmission();
        DBG_INFO(TAG, "CH422G write state: 0x%02X (pin %d HIGH)", state, pin);
    }
#endif

// ── Helper: build VFS full path from user path ──────────────────────────────

static void vfs_path(char* out, size_t outLen, const char* path) {
    // User paths are like "/foo.txt" — prepend mount point to get "/sdcard/foo.txt"
    if (path[0] == '/') {
        snprintf(out, outLen, "%s%s", MOUNT_POINT, path);
    } else {
        snprintf(out, outLen, "%s/%s", MOUNT_POINT, path);
    }
}

// ── Init / Deinit ───────────────────────────────────────────────────────────

bool SDCardHAL::init() {
#if defined(SD_MMC_D0) && defined(SD_MMC_CLK) && defined(SD_MMC_CMD)
    // If D3/CS is routed through an IO expander, drive it HIGH before SDMMC init.
    // In SDMMC mode, D3 must be pulled high to keep the card in SD mode.
    #if defined(SD_CS_VIA_EXPANDER) && SD_CS_VIA_EXPANDER
        ch422g_drive_pin_high(SD_CS_EXPANDER_PIN);
        delay(20);  // Give card time to see D3 high
    #endif

    DBG_INFO(TAG, "SD SDMMC init: CLK=%d CMD=%d D0=%d", SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)SD_MMC_CLK;
    slot.cmd = (gpio_num_t)SD_MMC_CMD;
    slot.d0  = (gpio_num_t)SD_MMC_D0;
    slot.width = 1;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;

    // Retry up to 3 times — card may need time after power-on
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            DBG_INFO(TAG, "SD SDMMC retry %d/3...", attempt + 1);
            delay(100);
        }
        ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &_card);
        if (ret == ESP_OK) break;
    }

    if (ret != ESP_OK) {
        _mounted = false;
        _card = nullptr;
        DBG_WARN(TAG, "SD SDMMC init failed after 3 attempts: %s", esp_err_to_name(ret));
        return false;
    }

    _mounted = true;
    DBG_INFO(TAG, "SD card mounted (SDMMC 1-bit), type=%s, total=%lluMB",
             getFilesystemType(), totalBytes() / (1024 * 1024));
    return true;
#else
    return false;
#endif
}

void SDCardHAL::deinit() {
    if (_mounted && _card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, _card);
        _card = nullptr;
        _mounted = false;
    }
}

// ── Card info ───────────────────────────────────────────────────────────────

uint64_t SDCardHAL::totalBytes() {
    if (!_mounted || !_card) return 0;
    // Card capacity = number of sectors * sector size
    return (uint64_t)_card->csd.capacity * _card->csd.sector_size;
}

uint64_t SDCardHAL::usedBytes() {
    if (!_mounted) return 0;
    FATFS* fs = nullptr;
    DWORD free_clusters = 0;
    // FATFS drive number: mount point is registered as "0:" internally
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK) return 0;
    uint64_t total = (uint64_t)((fs->n_fatent - 2) * fs->csize) * fs->ssize;
    uint64_t free_bytes = (uint64_t)(free_clusters * fs->csize) * fs->ssize;
    return total - free_bytes;
}

// ── File operations (POSIX via VFS) ─────────────────────────────────────────

bool SDCardHAL::exists(const char *path) {
    if (!_mounted) return false;
    char fullpath[272];
    vfs_path(fullpath, sizeof(fullpath), path);
    struct stat st;
    return stat(fullpath, &st) == 0;
}

char* SDCardHAL::readFile(const char *path, size_t &outLen) {
    outLen = 0;
    if (!_mounted) return nullptr;

    char fullpath[272];
    vfs_path(fullpath, sizeof(fullpath), path);

    FILE* f = fopen(fullpath, "rb");
    if (!f) return nullptr;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return nullptr; }

    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return nullptr; }

    outLen = fread(buf, 1, (size_t)sz, f);
    buf[outLen] = '\0';
    fclose(f);
    return buf;
}

bool SDCardHAL::writeFile(const char *path, const char *data) {
    if (!_mounted) return false;
    char fullpath[272];
    vfs_path(fullpath, sizeof(fullpath), path);

    FILE* f = fopen(fullpath, "w");
    if (!f) return false;
    bool ok = fputs(data, f) >= 0;
    fclose(f);
    return ok;
}

bool SDCardHAL::appendFile(const char *path, const char *data) {
    if (!_mounted) return false;
    char fullpath[272];
    vfs_path(fullpath, sizeof(fullpath), path);

    FILE* f = fopen(fullpath, "a");
    if (!f) return false;
    bool ok = fputs(data, f) >= 0;
    fclose(f);
    return ok;
}

bool SDCardHAL::removeFile(const char *path) {
    if (!_mounted) return false;
    char fullpath[272];
    vfs_path(fullpath, sizeof(fullpath), path);
    return remove(fullpath) == 0;
}

bool SDCardHAL::listFiles(const char *dirname, uint8_t levels) {
    if (!_mounted) return false;

    char fullpath[272];
    vfs_path(fullpath, sizeof(fullpath), dirname);

    DIR* dir = opendir(fullpath);
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        // Build child path for stat / recursion
        char childpath[272];
        if (dirname[0] == '/' && dirname[1] == '\0') {
            snprintf(childpath, sizeof(childpath), "/%s", entry->d_name);
        } else {
            snprintf(childpath, sizeof(childpath), "%s/%s", dirname, entry->d_name);
        }

        char child_fullpath[272];
        vfs_path(child_fullpath, sizeof(child_fullpath), childpath);

        struct stat st;
        if (stat(child_fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            DBG_INFO(TAG, "  DIR : %s", entry->d_name);
            if (levels > 0) listFiles(childpath, levels - 1);
        } else {
            DBG_INFO(TAG, "  FILE: %s  SIZE: %lu", entry->d_name, (unsigned long)st.st_size);
        }
    }
    closedir(dir);
    return true;
}

// ── Recursive removal ───────────────────────────────────────────────────────

bool SDCardHAL::removeRecursive(const char* path) {
    return _removeRecursive(path);
}

bool SDCardHAL::_removeRecursive(const char* path) {
    if (!_mounted) return false;

    char fullpath[272];
    vfs_path(fullpath, sizeof(fullpath), path);

    struct stat st;
    if (stat(fullpath, &st) != 0) return false;

    if (!S_ISDIR(st.st_mode)) {
        return remove(fullpath) == 0;
    }

    DIR* dir = opendir(fullpath);
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        char childpath[272];
        if (path[0] == '/' && path[1] == '\0') {
            snprintf(childpath, sizeof(childpath), "/%s", entry->d_name);
        } else {
            snprintf(childpath, sizeof(childpath), "%s/%s", path, entry->d_name);
        }

        _removeRecursive(childpath);
    }
    closedir(dir);

    // Remove the now-empty directory
    return ::rmdir(fullpath) == 0;
}

// ── Format ──────────────────────────────────────────────────────────────────

bool SDCardHAL::format() {
#if defined(SD_MMC_D0) && defined(SD_MMC_CLK) && defined(SD_MMC_CMD)
    if (!_mounted && !_card) {
        DBG_WARN(TAG, "SD card not mounted, attempting mount with format_if_mount_failed");

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.flags = SDMMC_HOST_FLAG_1BIT;
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.clk = (gpio_num_t)SD_MMC_CLK;
        slot.cmd = (gpio_num_t)SD_MMC_CMD;
        slot.d0  = (gpio_num_t)SD_MMC_D0;
        slot.width = 1;

        esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
        mount_cfg.format_if_mount_failed = true;
        mount_cfg.max_files = 5;
        mount_cfg.allocation_unit_size = 16 * 1024;

        esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &_card);
        if (ret != ESP_OK) {
            DBG_ERROR(TAG, "Format: mount with format failed: %s", esp_err_to_name(ret));
            _card = nullptr;
            return false;
        }
        _mounted = true;
        DBG_INFO(TAG, "SD card formatted and mounted successfully");
        return true;
    }

    // Already mounted — wipe all files then remount with format_if_mount_failed
    DBG_INFO(TAG, "Formatting SD card (clearing all files)...");

    // Delete all top-level entries
    char rootpath[272];
    vfs_path(rootpath, sizeof(rootpath), "/");

    DIR* dir = opendir(rootpath);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            char childpath[272];
            snprintf(childpath, sizeof(childpath), "/%s", entry->d_name);

            char child_fullpath[272];
            vfs_path(child_fullpath, sizeof(child_fullpath), childpath);

            struct stat st;
            if (stat(child_fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
                _removeRecursive(childpath);
            } else {
                remove(child_fullpath);
                DBG_INFO(TAG, "Deleted: %s", childpath);
            }
        }
        closedir(dir);
    }

    // Unmount and remount with format flag to ensure clean state
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, _card);
    _card = nullptr;
    _mounted = false;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)SD_MMC_CLK;
    slot.cmd = (gpio_num_t)SD_MMC_CMD;
    slot.d0  = (gpio_num_t)SD_MMC_D0;
    slot.width = 1;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = true;
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &_card);
    if (ret != ESP_OK) {
        DBG_ERROR(TAG, "Format: remount failed: %s", esp_err_to_name(ret));
        _card = nullptr;
        return false;
    }
    _mounted = true;
    DBG_INFO(TAG, "SD card formatted and remounted successfully");
    return true;
#else
    return false;
#endif
}

// ── mkdir ───────────────────────────────────────────────────────────────────

bool SDCardHAL::mkdir(const char* path) {
    if (!_mounted) return false;

    char fullpath[272];
    vfs_path(fullpath, sizeof(fullpath), path);

    int ret = ::mkdir(fullpath, 0755);
    if (ret == 0) {
        DBG_INFO(TAG, "Created directory: %s", path);
        return true;
    }
    DBG_WARN(TAG, "Failed to create directory: %s", path);
    return false;
}

// ── listDir ─────────────────────────────────────────────────────────────────

static void listDirRecursive(const char* dirname, int depth, int maxDepth) {
    char fullpath[272];
    vfs_path(fullpath, sizeof(fullpath), dirname);

    DIR* dir = opendir(fullpath);
    if (!dir) {
        DBG_WARN(TAG, "Cannot open directory: %s", dirname);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        char childpath[272];
        if (dirname[0] == '/' && dirname[1] == '\0') {
            snprintf(childpath, sizeof(childpath), "/%s", entry->d_name);
        } else {
            snprintf(childpath, sizeof(childpath), "%s/%s", dirname, entry->d_name);
        }

        char child_fullpath[272];
        vfs_path(child_fullpath, sizeof(child_fullpath), childpath);

        struct stat st;
        if (stat(child_fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            DBG_INFO(TAG, "%*sDIR : %s", depth * 2, "", entry->d_name);
            if (depth < maxDepth) {
                listDirRecursive(childpath, depth + 1, maxDepth);
            }
        } else {
            DBG_INFO(TAG, "%*sFILE: %s  SIZE: %lu", depth * 2, "", entry->d_name, (unsigned long)st.st_size);
        }
    }
    closedir(dir);
}

void SDCardHAL::listDir(const char* dir, int maxDepth) {
    if (!_mounted) {
        DBG_WARN(TAG, "listDir: SD card not mounted");
        return;
    }
    DBG_INFO(TAG, "Listing directory: %s (maxDepth=%d)", dir, maxDepth);
    listDirRecursive(dir, 0, maxDepth);
}

// ── getFilesystemType ───────────────────────────────────────────────────────

const char* SDCardHAL::getFilesystemType() const {
    if (!_mounted || !_card) return "none";
    if (_card->is_mmc) return "MMC";
    if ((_card->ocr & SD_OCR_SDHC_CAP) != 0) return "SDHC";
    return "SD";
}

// ── runTest ─────────────────────────────────────────────────────────────────

SDCardHAL::TestResult SDCardHAL::runTest(int cycles, size_t blockSize) {
    TestResult result = {};
    uint32_t startMs = millis();

    DBG_INFO(TAG, "--- SD Card Test Begin ---");
    DBG_INFO(TAG, "Cycles: %d, Block size: %u bytes", cycles, blockSize);

    result.mount_ok = _mounted;
    if (!result.mount_ok) {
        DBG_ERROR(TAG, "SD card not mounted, aborting test");
        result.test_duration_ms = millis() - startMs;
        return result;
    }

    result.total_bytes = (size_t)totalBytes();
    result.used_bytes = (size_t)usedBytes();
    DBG_INFO(TAG, "Card type: %s, Total: %u bytes, Used: %u bytes",
             getFilesystemType(), result.total_bytes, result.used_bytes);

    // Allocate test buffers
    uint8_t* writeData = (uint8_t*)malloc(blockSize);
    uint8_t* readData = (uint8_t*)malloc(blockSize);
    if (!writeData || !readData) {
        free(writeData);
        free(readData);
        DBG_ERROR(TAG, "Failed to allocate test buffers (%u bytes)", blockSize);
        result.test_duration_ms = millis() - startMs;
        return result;
    }

    // Fill with known pattern
    for (size_t i = 0; i < blockSize; i++) {
        writeData[i] = (uint8_t)(i & 0xFF);
    }

    // Test mkdir
    result.mkdir_ok = mkdir("/sdtest");
    DBG_INFO(TAG, "mkdir /sdtest: %s", result.mkdir_ok ? "OK" : "FAIL");

    size_t totalWritten = 0;
    size_t totalRead = 0;
    uint32_t writeTimeUs = 0;
    uint32_t readTimeUs = 0;

    for (int i = 0; i < cycles; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/sdtest/test_%d.bin", i);

        // Build VFS path for direct binary I/O (writeFile uses text mode)
        char fullpath[272];
        vfs_path(fullpath, sizeof(fullpath), path);

        // Write test — use binary fwrite for exact byte count
        uint32_t t0 = micros();
        bool wOk = false;
        FILE* wf = fopen(fullpath, "wb");
        if (wf) {
            size_t written = fwrite(writeData, 1, blockSize, wf);
            fclose(wf);
            wOk = (written == blockSize);
            if (wOk) totalWritten += blockSize;
        }
        uint32_t t1 = micros();
        writeTimeUs += (t1 - t0);
        result.write_ok = result.write_ok || wOk;

        // Read test — use binary fread for exact byte count
        uint32_t t2 = micros();
        bool rOk = false;
        bool vOk = false;
        FILE* rf = fopen(fullpath, "rb");
        if (rf) {
            size_t readLen = fread(readData, 1, blockSize, rf);
            fclose(rf);
            rOk = (readLen == blockSize);
            if (rOk) {
                totalRead += blockSize;
                vOk = (memcmp(writeData, readData, blockSize) == 0);
            }
        }
        uint32_t t3 = micros();
        readTimeUs += (t3 - t2);
        result.read_ok = result.read_ok || rOk;
        if (vOk) result.verify_ok = true;

        // Delete test file
        bool dOk = (remove(fullpath) == 0);
        result.delete_ok = result.delete_ok || dOk;

        result.cycles_completed = i + 1;
        DBG_INFO(TAG, "Cycle %d/%d: write=%s read=%s verify=%s delete=%s",
                 i + 1, cycles, wOk ? "OK" : "FAIL",
                 rOk ? "OK" : "FAIL", vOk ? "OK" : "FAIL",
                 dOk ? "OK" : "FAIL");
    }

    // Calculate throughput
    if (writeTimeUs > 0) {
        result.write_speed_kbps = (uint32_t)(totalWritten * 1000000ULL / writeTimeUs / 1024);
    }
    if (readTimeUs > 0) {
        result.read_speed_kbps = (uint32_t)(totalRead * 1000000ULL / readTimeUs / 1024);
    }

    // Clean up test directory
    char testdir[272];
    vfs_path(testdir, sizeof(testdir), "/sdtest");
    ::rmdir(testdir);

    free(writeData);
    free(readData);

    result.test_duration_ms = millis() - startMs;

    DBG_INFO(TAG, "--- SD Card Test Complete ---");
    DBG_INFO(TAG, "Write speed: %u KB/s, Read speed: %u KB/s",
             result.write_speed_kbps, result.read_speed_kbps);
    DBG_INFO(TAG, "Duration: %u ms, Cycles: %d/%d",
             result.test_duration_ms, result.cycles_completed, cycles);

    return result;
}

#else // !HAS_SDCARD — stubs

bool SDCardHAL::init() { return false; }
void SDCardHAL::deinit() {}
uint64_t SDCardHAL::totalBytes() { return 0; }
uint64_t SDCardHAL::usedBytes() { return 0; }
bool SDCardHAL::exists(const char*) { return false; }
char* SDCardHAL::readFile(const char*, size_t &outLen) { outLen = 0; return nullptr; }
bool SDCardHAL::writeFile(const char*, const char*) { return false; }
bool SDCardHAL::appendFile(const char*, const char*) { return false; }
bool SDCardHAL::removeFile(const char*) { return false; }
bool SDCardHAL::listFiles(const char*, uint8_t) { return false; }
bool SDCardHAL::format() { return false; }
bool SDCardHAL::mkdir(const char*) { return false; }
void SDCardHAL::listDir(const char*, int) {}
const char* SDCardHAL::getFilesystemType() const { return "none"; }
bool SDCardHAL::removeRecursive(const char*) { return false; }
bool SDCardHAL::_removeRecursive(const char*) { return false; }
SDCardHAL::TestResult SDCardHAL::runTest(int, size_t) { return {}; }

#endif // HAS_SDCARD
#endif // SIMULATOR
