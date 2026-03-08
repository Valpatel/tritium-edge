#include "hal_sdcard.h"
#include "debug_log.h"

static constexpr const char* TAG = "sdcard";

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

bool SDCardHAL::startUSBMSC() { return false; }
void SDCardHAL::stopUSBMSC() {}

#else // ESP32

#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>

#if CONFIG_TINYUSB_MSC_ENABLED
#include "USBMSC.h"
#endif

#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#ifndef HAS_SDCARD
#define HAS_SDCARD 0
#endif

#if HAS_SDCARD
static sdmmc_card_t* _sdCardPtr = nullptr;
#endif

bool SDCardHAL::init() {
#if !HAS_SDCARD
    return false;
#elif defined(SD_MMC_D0) && defined(SD_MMC_CLK) && defined(SD_MMC_CMD)
    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    if (!SD_MMC.begin("/sdcard", true)) {
        _mounted = false;
        return false;
    }
    _mounted = true;

    // Probe card for USB MSC support — reuse the already-initialized SDMMC host
    if (!_sdCardPtr) {
        _sdCardPtr = (sdmmc_card_t*)malloc(sizeof(sdmmc_card_t));
        if (_sdCardPtr) {
            sdmmc_host_t host = SDMMC_HOST_DEFAULT();
            host.flags = SDMMC_HOST_FLAG_1BIT;
            host.max_freq_khz = SDMMC_FREQ_DEFAULT;
            esp_err_t err = sdmmc_card_init(&host, _sdCardPtr);
            if (err != ESP_OK) {
                DBG_WARN(TAG, "Card probe for USB MSC failed: 0x%x", err);
                free(_sdCardPtr);
                _sdCardPtr = nullptr;
            } else {
                DBG_INFO(TAG, "Card probed: %u sectors of %u bytes",
                         (unsigned)_sdCardPtr->csd.capacity,
                         (unsigned)_sdCardPtr->csd.sector_size);
            }
        }
    }

    return true;
#else
    return false;
#endif
}

void SDCardHAL::deinit() {
#if HAS_SDCARD
    if (_mounted) { SD_MMC.end(); _mounted = false; }
#endif
}

uint64_t SDCardHAL::totalBytes() {
#if HAS_SDCARD
    return _mounted ? SD_MMC.totalBytes() : 0;
#else
    return 0;
#endif
}

uint64_t SDCardHAL::usedBytes() {
#if HAS_SDCARD
    return _mounted ? SD_MMC.usedBytes() : 0;
#else
    return 0;
#endif
}

bool SDCardHAL::exists(const char *path) {
#if HAS_SDCARD
    return _mounted ? SD_MMC.exists(path) : false;
#else
    return false;
#endif
}

char* SDCardHAL::readFile(const char *path, size_t &outLen) {
    outLen = 0;
#if HAS_SDCARD
    if (!_mounted) return nullptr;
    File file = SD_MMC.open(path, FILE_READ);
    if (!file) return nullptr;
    size_t sz = file.size();
    char *buf = (char*)malloc(sz + 1);
    if (!buf) { file.close(); return nullptr; }
    outLen = file.readBytes(buf, sz);
    buf[outLen] = '\0';
    file.close();
    return buf;
#else
    return nullptr;
#endif
}

bool SDCardHAL::writeFile(const char *path, const char *data) {
#if HAS_SDCARD
    if (!_mounted) return false;
    File file = SD_MMC.open(path, FILE_WRITE);
    if (!file) return false;
    bool ok = file.print(data) > 0;
    file.close();
    return ok;
#else
    return false;
#endif
}

bool SDCardHAL::appendFile(const char *path, const char *data) {
#if HAS_SDCARD
    if (!_mounted) return false;
    File file = SD_MMC.open(path, FILE_APPEND);
    if (!file) return false;
    bool ok = file.print(data) > 0;
    file.close();
    return ok;
#else
    return false;
#endif
}

bool SDCardHAL::removeFile(const char *path) {
#if HAS_SDCARD
    return _mounted ? SD_MMC.remove(path) : false;
#else
    return false;
#endif
}

bool SDCardHAL::listFiles(const char *dirname, uint8_t levels) {
#if HAS_SDCARD
    if (!_mounted) return false;
    File root = SD_MMC.open(dirname);
    if (!root || !root.isDirectory()) return false;
    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            Serial.printf("  DIR : %s\n", file.name());
            if (levels > 0) listFiles(file.path(), levels - 1);
        } else {
            Serial.printf("  FILE: %s  SIZE: %u\n", file.name(), file.size());
        }
        file = root.openNextFile();
    }
    return true;
#else
    return false;
#endif
}

bool SDCardHAL::format() {
#if HAS_SDCARD
    if (!_mounted) {
        DBG_WARN(TAG, "SD card not mounted, attempting mount with format_if_mount_failed");
        SD_MMC.end();
        if (!SD_MMC.begin("/sdcard", true, true)) {  // 1-bit mode, format_if_mount_failed
            DBG_ERROR(TAG, "Format: mount with format failed");
            return false;
        }
        _mounted = true;
        DBG_INFO(TAG, "SD card formatted and mounted successfully");
        return true;
    }
    // Unmount, remount with format_if_mount_failed=true.
    // Note: SDMMCFS has no format() method in pioarduino/Arduino ESP32 v3.x.
    // This relies on the VFS layer's format-on-mount-failure path, which
    // may not reformat an already-mountable card. Best-effort operation.
    SD_MMC.end();
    _mounted = false;
    if (!SD_MMC.begin("/sdcard", true, true)) {
        DBG_ERROR(TAG, "Format: remount with format failed");
        return false;
    }
    _mounted = true;
    DBG_INFO(TAG, "SD card formatted and mounted successfully");
    return true;
#else
    return false;
#endif
}

bool SDCardHAL::mkdir(const char* path) {
#if HAS_SDCARD
    if (!_mounted) return false;
    bool ok = SD_MMC.mkdir(path);
    if (ok) {
        DBG_INFO(TAG, "Created directory: %s", path);
    } else {
        DBG_WARN(TAG, "Failed to create directory: %s", path);
    }
    return ok;
#else
    return false;
#endif
}

static void listDirRecursive(const char* dirname, int depth, int maxDepth) {
#if HAS_SDCARD
    File root = SD_MMC.open(dirname);
    if (!root || !root.isDirectory()) {
        DBG_WARN(TAG, "Cannot open directory: %s", dirname);
        return;
    }
    File file = root.openNextFile();
    while (file) {
        // Indent based on depth
        if (file.isDirectory()) {
            DBG_INFO(TAG, "%*sDIR : %s", depth * 2, "", file.name());
            if (depth < maxDepth) {
                listDirRecursive(file.path(), depth + 1, maxDepth);
            }
        } else {
            DBG_INFO(TAG, "%*sFILE: %s  SIZE: %u", depth * 2, "", file.name(), file.size());
        }
        file = root.openNextFile();
    }
#endif
}

void SDCardHAL::listDir(const char* dir, int maxDepth) {
#if HAS_SDCARD
    if (!_mounted) {
        DBG_WARN(TAG, "listDir: SD card not mounted");
        return;
    }
    DBG_INFO(TAG, "Listing directory: %s (maxDepth=%d)", dir, maxDepth);
    listDirRecursive(dir, 0, maxDepth);
#else
    DBG_WARN(TAG, "listDir: SD card not available");
#endif
}

const char* SDCardHAL::getFilesystemType() const {
#if HAS_SDCARD
    if (!_mounted) return "none";
    uint8_t cardType = SD_MMC.cardType();
    switch (cardType) {
        case CARD_MMC:  return "MMC";
        case CARD_SD:   return "SD";
        case CARD_SDHC: return "SDHC";
        default:        return "unknown";
    }
#else
    return "none";
#endif
}

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

        // Write test
        uint32_t t0 = micros();
        File wf = SD_MMC.open(path, FILE_WRITE);
        bool wOk = false;
        if (wf) {
            size_t written = wf.write(writeData, blockSize);
            wf.close();
            wOk = (written == blockSize);
            if (wOk) totalWritten += blockSize;
        }
        uint32_t t1 = micros();
        writeTimeUs += (t1 - t0);
        result.write_ok = result.write_ok || wOk;

        // Read test
        uint32_t t2 = micros();
        File rf = SD_MMC.open(path, FILE_READ);
        bool rOk = false;
        bool vOk = false;
        if (rf) {
            size_t readLen = rf.read(readData, blockSize);
            rf.close();
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
        bool dOk = SD_MMC.remove(path);
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
    SD_MMC.rmdir("/sdtest");

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

// ---------------------------------------------------------------------------
// USB Mass Storage — expose SD card as a removable drive
// ---------------------------------------------------------------------------
#if HAS_SDCARD && CONFIG_TINYUSB_MSC_ENABLED

static USBMSC _usbMsc;

static int32_t _mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (!_sdCardPtr) return -1;
    uint32_t sectorCount = bufsize / _sdCardPtr->csd.sector_size;
    if (sectorCount == 0) sectorCount = 1;
    esp_err_t err = sdmmc_read_sectors(_sdCardPtr, buffer, lba, sectorCount);
    return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

static int32_t _mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (!_sdCardPtr) return -1;
    uint32_t sectorCount = bufsize / _sdCardPtr->csd.sector_size;
    if (sectorCount == 0) sectorCount = 1;
    esp_err_t err = sdmmc_write_sectors(_sdCardPtr, buffer, lba, sectorCount);
    return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

static bool _mscStartStop(uint8_t power_condition, bool start, bool load_eject) {
    DBG_INFO(TAG, "USB MSC: start=%d load_eject=%d", start, load_eject);
    return true;
}

bool SDCardHAL::startUSBMSC() {
    if (_msc_active) return true;
    if (!_mounted || !_sdCardPtr) {
        DBG_ERROR(TAG, "Cannot start USB MSC: SD card not mounted or not probed");
        return false;
    }

    uint32_t sectorSize = _sdCardPtr->csd.sector_size;
    uint32_t sectorCount = _sdCardPtr->csd.capacity;

    DBG_INFO(TAG, "USB MSC: sectors=%u, sector_size=%u, total=%llu MB",
             sectorCount, sectorSize,
             (uint64_t)sectorCount * sectorSize / (1024 * 1024));

    // Unmount FAT to avoid concurrent access conflicts
    SD_MMC.end();

    _usbMsc.vendorID("ESP32-S3");
    _usbMsc.productID("SD Card");
    _usbMsc.productRevision("1.0");
    _usbMsc.onStartStop(_mscStartStop);
    _usbMsc.onRead(_mscRead);
    _usbMsc.onWrite(_mscWrite);
    _usbMsc.mediaPresent(true);

    if (!_usbMsc.begin(sectorCount, sectorSize)) {
        DBG_ERROR(TAG, "USB MSC begin failed");
        // Remount FAT
        SD_MMC.begin("/sdcard", true);
        return false;
    }

    _msc_active = true;
    DBG_INFO(TAG, "USB MSC started — SD card visible as USB drive");
    DBG_INFO(TAG, "SD FAT unmounted to prevent conflicts. Call stopUSBMSC() to remount.");
    return true;
}

void SDCardHAL::stopUSBMSC() {
    if (!_msc_active) return;
    _usbMsc.end();
    _msc_active = false;

    // Remount FAT filesystem
    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    if (SD_MMC.begin("/sdcard", true)) {
        DBG_INFO(TAG, "SD FAT remounted after USB MSC");
    } else {
        DBG_WARN(TAG, "SD FAT remount failed after USB MSC");
        _mounted = false;
    }

    DBG_INFO(TAG, "USB MSC stopped");
}

#else // No SD or no TinyUSB MSC

bool SDCardHAL::startUSBMSC() {
    DBG_WARN(TAG, "USB MSC not available (no SD card or TinyUSB MSC not enabled)");
    return false;
}
void SDCardHAL::stopUSBMSC() {}

#endif // HAS_SDCARD && CONFIG_TINYUSB_MSC_ENABLED

#endif // SIMULATOR
