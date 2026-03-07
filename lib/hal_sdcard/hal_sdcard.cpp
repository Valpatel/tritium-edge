#include "hal_sdcard.h"

#ifdef SIMULATOR

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

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

#else // ESP32

#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>

#ifndef HAS_SDCARD
#define HAS_SDCARD 0
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

#endif // SIMULATOR
