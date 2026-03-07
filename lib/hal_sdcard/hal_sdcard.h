#pragma once
// SD Card HAL via SDMMC 1-bit mode
//
// Usage:
//   #include "hal_sdcard.h"
//   SDCardHAL sd;
//   sd.init();
//   sd.listFiles("/");
//   size_t len;
//   const char* data = sd.readFile("/config.txt", len);

#include <cstdint>
#include <cstddef>

class SDCardHAL {
public:
    bool init();
    void deinit();
    bool isMounted() const { return _mounted; }
    uint64_t totalBytes();
    uint64_t usedBytes();
    bool exists(const char *path);
    // Caller must free() the returned buffer. Returns nullptr on failure.
    char* readFile(const char *path, size_t &outLen);
    bool writeFile(const char *path, const char *data);
    bool appendFile(const char *path, const char *data);
    bool removeFile(const char *path);
    bool listFiles(const char *dirname, uint8_t levels = 0);

private:
    bool _mounted = false;
};
