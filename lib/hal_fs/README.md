# hal_fs — LittleFS HAL

Thin wrapper around LittleFS for ESP32-S3 internal flash storage. Provides file I/O, directory operations, JSON config helpers, and a built-in test harness.

Simulator builds (`-DSIMULATOR`) use standard C stdio against a local `./littlefs/` directory.

## Usage

```cpp
#include "hal_fs.h"

FsHAL fs;

void setup() {
    fs.init();

    // Write and read a file
    fs.writeFile("/hello.txt", "Hello, LittleFS!");

    char buf[64];
    size_t n;
    fs.readFile("/hello.txt", buf, sizeof(buf), &n);

    // JSON config
    fs.writeConfig("/config.json", "{\"ssid\":\"MyNet\",\"pass\":\"1234\"}");

    char cfg[256];
    fs.readConfig("/config.json", cfg, sizeof(cfg));

    // Capacity info
    size_t total = fs.totalBytes();
    size_t used  = fs.usedBytes();
    size_t free_ = fs.freeBytes();

    // List all files
    fs.listFiles("/");

    // Run performance/integrity test
    FsHAL::TestResult result = fs.runTest(10, 1024);
}
```

## Test Harness

`runTest(cycles, blockSize)` writes, reads, and verifies a data block each cycle, then tests append and directory operations. Pass `cycles=0` to run a destructive format test instead.
