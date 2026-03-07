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

    // Format the SD card as FAT32 — WARNING: erases all data!
    bool format();

    // Create directory (and parents)
    bool mkdir(const char* path);

    // List directory contents to debug log
    void listDir(const char* dir = "/", int maxDepth = 2);

    // Get filesystem type string
    const char* getFilesystemType() const;

    // Test harness for SD card operations
    struct TestResult {
        bool mount_ok;
        bool write_ok;
        bool read_ok;
        bool verify_ok;        // Data integrity
        bool mkdir_ok;
        bool delete_ok;
        uint32_t write_speed_kbps;   // KB/s write speed
        uint32_t read_speed_kbps;    // KB/s read speed
        size_t total_bytes;
        size_t used_bytes;
        int cycles_completed;
        uint32_t test_duration_ms;
    };
    // Run read/write performance test. Writes blockSize bytes for cycles iterations.
    TestResult runTest(int cycles = 10, size_t blockSize = 4096);

private:
    bool _mounted = false;
};
