#pragma once
// LittleFS HAL — wrapper for internal flash storage.
// Used for config files, web assets, logs, cached data.
//
// Usage:
//   #include "hal_fs.h"
//   FsHAL fs;
//   fs.init();
//   fs.writeFile("/config.json", "{\"key\":\"value\"}");
//   char buf[256];
//   size_t n;
//   fs.readFile("/config.json", buf, sizeof(buf), &n);

#include <cstdint>
#include <cstddef>

class FsHAL {
public:
    bool init(bool formatOnFail = true);
    void deinit();
    bool isReady() const;

    // Capacity
    size_t totalBytes() const;
    size_t usedBytes() const;
    size_t freeBytes() const;

    // File I/O
    bool readFile(const char* path, char* buf, size_t bufSize, size_t* bytesRead = nullptr);
    bool writeFile(const char* path, const char* data, size_t len);
    bool writeFile(const char* path, const char* str); // null-terminated convenience
    bool appendFile(const char* path, const char* data, size_t len);
    bool removeFile(const char* path);
    bool exists(const char* path);
    size_t fileSize(const char* path);

    // Directories
    bool mkdir(const char* path);
    bool rmdir(const char* path);

    // Utility
    bool format();  // Erase and reformat the filesystem
    void listFiles(const char* dir = "/", int maxDepth = 2);  // Print to debug log

    // JSON config helpers
    bool readConfig(const char* path, char* buf, size_t bufSize);
    bool writeConfig(const char* path, const char* json);

    // Test harness
    struct TestResult {
        uint32_t write_us;      // microseconds for write test
        uint32_t read_us;       // microseconds for read test
        uint32_t append_us;     // microseconds for append test
        size_t bytes_written;
        size_t bytes_read;
        bool write_ok;
        bool read_ok;
        bool verify_ok;        // data integrity check
        bool dir_ok;           // mkdir/rmdir test
        bool format_ok;
        int cycles_completed;
    };
    TestResult runTest(int cycles = 10, size_t blockSize = 1024);

private:
    bool _ready = false;
};
