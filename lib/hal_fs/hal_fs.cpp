#include "hal_fs.h"
#include "debug_log.h"

#include <cstring>
#include <cstdlib>

static const char* TAG = "fs";

// ============================================================================
// SIMULATOR STUB — uses local ./littlefs/ directory with standard C stdio
// ============================================================================
#ifdef SIMULATOR

#include <cstdio>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <chrono>

static const char* SIM_ROOT = "./littlefs";

static uint32_t sim_micros() {
    auto now = std::chrono::steady_clock::now();
    return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
}

// Build a host-side path from a LittleFS path
static void sim_path(const char* path, char* out, size_t outSize) {
    snprintf(out, outSize, "%s%s", SIM_ROOT, path);
}

// Recursively create directories for a path
static void sim_mkdirs(const char* hostPath) {
    char tmp[512];
    strncpy(tmp, hostPath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            ::mkdir(tmp, 0755);
            *p = '/';
        }
    }
    ::mkdir(tmp, 0755);
}

bool FsHAL::init(bool /*formatOnFail*/) {
    sim_mkdirs(SIM_ROOT);
    _ready = true;
    DBG_INFO(TAG, "LittleFS simulator init (root: %s)", SIM_ROOT);
    return true;
}

void FsHAL::deinit() {
    _ready = false;
    DBG_INFO(TAG, "LittleFS simulator deinit");
}

bool FsHAL::isReady() const { return _ready; }
size_t FsHAL::totalBytes() const { return 1024 * 1024; } // fake 1 MB
size_t FsHAL::usedBytes() const { return 0; }
size_t FsHAL::freeBytes() const { return totalBytes(); }

bool FsHAL::readFile(const char* path, char* buf, size_t bufSize, size_t* bytesRead) {
    if (!_ready) return false;
    char hp[512]; sim_path(path, hp, sizeof(hp));
    FILE* f = fopen(hp, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, bufSize, f);
    fclose(f);
    if (bytesRead) *bytesRead = n;
    return true;
}

bool FsHAL::writeFile(const char* path, const char* data, size_t len) {
    if (!_ready) return false;
    char hp[512]; sim_path(path, hp, sizeof(hp));
    // Ensure parent directory exists
    char dir[512];
    strncpy(dir, hp, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
    char* sl = strrchr(dir, '/');
    if (sl) { *sl = '\0'; sim_mkdirs(dir); }
    FILE* f = fopen(hp, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

bool FsHAL::writeFile(const char* path, const char* str) {
    return writeFile(path, str, strlen(str));
}

bool FsHAL::appendFile(const char* path, const char* data, size_t len) {
    if (!_ready) return false;
    char hp[512]; sim_path(path, hp, sizeof(hp));
    FILE* f = fopen(hp, "ab");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

bool FsHAL::removeFile(const char* path) {
    if (!_ready) return false;
    char hp[512]; sim_path(path, hp, sizeof(hp));
    return ::remove(hp) == 0;
}

bool FsHAL::exists(const char* path) {
    if (!_ready) return false;
    char hp[512]; sim_path(path, hp, sizeof(hp));
    struct stat st;
    return stat(hp, &st) == 0;
}

size_t FsHAL::fileSize(const char* path) {
    if (!_ready) return 0;
    char hp[512]; sim_path(path, hp, sizeof(hp));
    struct stat st;
    if (stat(hp, &st) != 0) return 0;
    return (size_t)st.st_size;
}

bool FsHAL::mkdir(const char* path) {
    if (!_ready) return false;
    char hp[512]; sim_path(path, hp, sizeof(hp));
    sim_mkdirs(hp);
    return true;
}

bool FsHAL::rmdir(const char* path) {
    if (!_ready) return false;
    char hp[512]; sim_path(path, hp, sizeof(hp));
    return ::rmdir(hp) == 0;
}

bool FsHAL::format() {
    DBG_WARN(TAG, "format() not supported in simulator");
    return false;
}

void FsHAL::listFiles(const char* dir, int maxDepth) {
    if (!_ready) return;
    char hp[512]; sim_path(dir, hp, sizeof(hp));
    DIR* d = opendir(hp);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (ent->d_type == DT_DIR) {
            DBG_INFO(TAG, "  DIR : %s", full);
            if (maxDepth > 0) listFiles(full, maxDepth - 1);
        } else {
            DBG_INFO(TAG, "  FILE: %s  SIZE: %zu", full, fileSize(full));
        }
    }
    closedir(d);
}

bool FsHAL::readConfig(const char* path, char* buf, size_t bufSize) {
    return readFile(path, buf, bufSize);
}

bool FsHAL::writeConfig(const char* path, const char* json) {
    return writeFile(path, json);
}

FsHAL::TestResult FsHAL::runTest(int cycles, size_t blockSize) {
    TestResult r = {};
    if (!_ready) return r;

    const char* testFile = "/_fs_test.bin";
    const char* testDir  = "/_fs_testdir";
    const char* appendFile_ = "/_fs_append.bin";

    uint32_t writeMin = UINT32_MAX, writeMax = 0, writeSum = 0;
    uint32_t readMin  = UINT32_MAX, readMax  = 0, readSum  = 0;
    uint32_t appendTotal = 0;

    char* wbuf = (char*)malloc(blockSize);
    char* rbuf = (char*)malloc(blockSize);
    if (!wbuf || !rbuf) { free(wbuf); free(rbuf); return r; }

    // Format-only test when cycles == 0
    if (cycles == 0) {
        r.format_ok = format();
        if (r.format_ok) {
            r.format_ok = writeFile(testFile, "test", 4) &&
                          exists(testFile);
            if (r.format_ok) {
                size_t nr = 0;
                char tb[8];
                r.format_ok = readFile(testFile, tb, sizeof(tb), &nr) && nr == 4;
            }
            removeFile(testFile);
        }
        free(wbuf); free(rbuf);
        return r;
    }

    bool allVerify = true;
    r.dir_ok = true;

    for (int i = 0; i < cycles; i++) {
        // Fill write buffer with known pattern
        for (size_t b = 0; b < blockSize; b++) {
            wbuf[b] = (char)(0xAA ^ (uint8_t)(i & 0xFF) ^ (uint8_t)(b & 0xFF));
        }

        // Write test
        uint32_t t0 = sim_micros();
        bool wok = writeFile(testFile, wbuf, blockSize);
        uint32_t wt = sim_micros() - t0;

        // Read test
        uint32_t t1 = sim_micros();
        size_t nr = 0;
        bool rok = readFile(testFile, rbuf, blockSize, &nr);
        uint32_t rt = sim_micros() - t1;

        // Verify
        bool vok = rok && (nr == blockSize) && (memcmp(wbuf, rbuf, blockSize) == 0);
        if (!vok) allVerify = false;

        if (wt < writeMin) writeMin = wt;
        if (wt > writeMax) writeMax = wt;
        writeSum += wt;
        if (rt < readMin) readMin = rt;
        if (rt > readMax) readMax = rt;
        readSum += rt;

        r.write_ok = wok;
        r.read_ok  = rok;

        DBG_INFO(TAG, "cycle %d/%d: write %luus read %luus verify:%s",
                 i + 1, cycles,
                 (unsigned long)wt, (unsigned long)rt,
                 vok ? "PASS" : "FAIL");

        r.cycles_completed = i + 1;
    }

    // Append test: 10 appends of blockSize bytes
    removeFile(appendFile_);
    uint32_t at0 = sim_micros();
    for (int a = 0; a < 10; a++) {
        memset(wbuf, (char)(0xBB + a), blockSize);
        appendFile(appendFile_, wbuf, blockSize);
    }
    appendTotal = sim_micros() - at0;
    size_t expectedAppendSize = blockSize * 10;
    size_t actualAppendSize = fileSize(appendFile_);
    if (actualAppendSize != expectedAppendSize) {
        DBG_WARN(TAG, "Append size mismatch: expected %zu got %zu",
                 expectedAppendSize, actualAppendSize);
    }
    removeFile(appendFile_);

    // Directory test
    bool mkOk = mkdir(testDir);
    bool dirExists = exists(testDir);
    bool rmOk = rmdir(testDir);
    r.dir_ok = mkOk && dirExists && rmOk;

    // Fill result
    r.write_us      = (cycles > 0) ? (writeSum / (uint32_t)cycles) : 0;
    r.read_us       = (cycles > 0) ? (readSum / (uint32_t)cycles) : 0;
    r.append_us     = appendTotal;
    r.bytes_written  = blockSize;
    r.bytes_read     = blockSize;
    r.verify_ok      = allVerify;
    r.format_ok      = false; // Not tested when cycles > 0

    // Cleanup
    removeFile(testFile);

    bool allPassed = r.write_ok && r.read_ok && r.verify_ok && r.dir_ok;
    DBG_INFO(TAG, "Test complete: %d cycles, write avg %luus, read avg %luus, all passed: %s",
             cycles,
             (unsigned long)r.write_us, (unsigned long)r.read_us,
             allPassed ? "YES" : "NO");

    free(wbuf);
    free(rbuf);
    return r;
}

// ============================================================================
// ESP32 — real LittleFS on internal flash
// ============================================================================
#else

#include <Arduino.h>
#include <LittleFS.h>
#include <FS.h>

bool FsHAL::init(bool formatOnFail) {
    if (_ready) return true;
    if (!LittleFS.begin(formatOnFail)) {
        DBG_ERROR(TAG, "LittleFS mount failed");
        _ready = false;
        return false;
    }
    _ready = true;
    DBG_INFO(TAG, "LittleFS mounted — total: %zu  used: %zu",
             totalBytes(), usedBytes());
    return true;
}

void FsHAL::deinit() {
    if (_ready) {
        LittleFS.end();
        _ready = false;
        DBG_INFO(TAG, "LittleFS unmounted");
    }
}

bool FsHAL::isReady() const { return _ready; }

size_t FsHAL::totalBytes() const {
    return _ready ? LittleFS.totalBytes() : 0;
}

size_t FsHAL::usedBytes() const {
    return _ready ? LittleFS.usedBytes() : 0;
}

size_t FsHAL::freeBytes() const {
    return totalBytes() - usedBytes();
}

bool FsHAL::readFile(const char* path, char* buf, size_t bufSize, size_t* bytesRead) {
    if (!_ready) return false;
    File f = LittleFS.open(path, "r");
    if (!f) {
        DBG_WARN(TAG, "readFile: cannot open %s", path);
        return false;
    }
    size_t n = f.readBytes(buf, bufSize);
    f.close();
    if (bytesRead) *bytesRead = n;
    return true;
}

bool FsHAL::writeFile(const char* path, const char* data, size_t len) {
    if (!_ready) return false;
    File f = LittleFS.open(path, "w");
    if (!f) {
        DBG_ERROR(TAG, "writeFile: cannot open %s", path);
        return false;
    }
    size_t w = f.write((const uint8_t*)data, len);
    f.close();
    if (w != len) {
        DBG_ERROR(TAG, "writeFile: short write %zu/%zu on %s", w, len, path);
        return false;
    }
    return true;
}

bool FsHAL::writeFile(const char* path, const char* str) {
    return writeFile(path, str, strlen(str));
}

bool FsHAL::appendFile(const char* path, const char* data, size_t len) {
    if (!_ready) return false;
    File f = LittleFS.open(path, "a");
    if (!f) {
        DBG_ERROR(TAG, "appendFile: cannot open %s", path);
        return false;
    }
    size_t w = f.write((const uint8_t*)data, len);
    f.close();
    return w == len;
}

bool FsHAL::removeFile(const char* path) {
    if (!_ready) return false;
    return LittleFS.remove(path);
}

bool FsHAL::exists(const char* path) {
    if (!_ready) return false;
    return LittleFS.exists(path);
}

size_t FsHAL::fileSize(const char* path) {
    if (!_ready) return 0;
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    size_t sz = f.size();
    f.close();
    return sz;
}

bool FsHAL::mkdir(const char* path) {
    if (!_ready) return false;
    return LittleFS.mkdir(path);
}

bool FsHAL::rmdir(const char* path) {
    if (!_ready) return false;
    return LittleFS.rmdir(path);
}

bool FsHAL::format() {
    if (!_ready) {
        DBG_WARN(TAG, "format: not mounted");
        return false;
    }
    DBG_WARN(TAG, "Formatting LittleFS...");
    bool ok = LittleFS.format();
    if (ok) {
        DBG_INFO(TAG, "Format complete");
    } else {
        DBG_ERROR(TAG, "Format failed");
    }
    return ok;
}

static void listFilesRecursive(File root, int depth, int maxDepth) {
    File f = root.openNextFile();
    while (f) {
        if (f.isDirectory()) {
            DBG_INFO(TAG, "%*sDIR : %s", depth * 2, "", f.path());
            if (depth < maxDepth) {
                listFilesRecursive(f, depth + 1, maxDepth);
            }
        } else {
            DBG_INFO(TAG, "%*sFILE: %s  SIZE: %zu", depth * 2, "", f.path(), (size_t)f.size());
        }
        f = root.openNextFile();
    }
}

void FsHAL::listFiles(const char* dir, int maxDepth) {
    if (!_ready) return;
    File root = LittleFS.open(dir);
    if (!root || !root.isDirectory()) {
        DBG_WARN(TAG, "listFiles: cannot open %s", dir);
        return;
    }
    DBG_INFO(TAG, "Listing: %s", dir);
    listFilesRecursive(root, 0, maxDepth);
}

bool FsHAL::readConfig(const char* path, char* buf, size_t bufSize) {
    size_t n = 0;
    if (!readFile(path, buf, bufSize - 1, &n)) return false;
    buf[n] = '\0'; // ensure null-terminated for JSON parsing
    return true;
}

bool FsHAL::writeConfig(const char* path, const char* json) {
    return writeFile(path, json);
}

FsHAL::TestResult FsHAL::runTest(int cycles, size_t blockSize) {
    TestResult r = {};
    if (!_ready) return r;

    const char* testFile    = "/_fs_test.bin";
    const char* testDir     = "/_fs_testdir";
    const char* appendPath  = "/_fs_append.bin";

    uint32_t writeMin = UINT32_MAX, writeMax = 0, writeSum = 0;
    uint32_t readMin  = UINT32_MAX, readMax  = 0, readSum  = 0;
    uint32_t appendTotal = 0;

    char* wbuf = (char*)malloc(blockSize);
    char* rbuf = (char*)malloc(blockSize);
    if (!wbuf || !rbuf) {
        DBG_ERROR(TAG, "runTest: malloc failed for blockSize %zu", blockSize);
        free(wbuf); free(rbuf);
        return r;
    }

    // Format-only test when cycles == 0
    if (cycles == 0) {
        r.format_ok = format();
        if (r.format_ok) {
            r.format_ok = writeFile(testFile, "test", 4) &&
                          exists(testFile);
            if (r.format_ok) {
                size_t nr = 0;
                char tb[8];
                r.format_ok = readFile(testFile, tb, sizeof(tb), &nr) && nr == 4;
            }
            removeFile(testFile);
        }
        DBG_INFO(TAG, "Format test: %s", r.format_ok ? "PASS" : "FAIL");
        free(wbuf); free(rbuf);
        return r;
    }

    bool allVerify = true;
    r.dir_ok = true;

    for (int i = 0; i < cycles; i++) {
        // Fill write buffer with known pattern: 0xAA XOR cycle XOR offset
        for (size_t b = 0; b < blockSize; b++) {
            wbuf[b] = (char)(0xAA ^ (uint8_t)(i & 0xFF) ^ (uint8_t)(b & 0xFF));
        }

        // Write test
        uint32_t t0 = micros();
        bool wok = writeFile(testFile, wbuf, blockSize);
        uint32_t wt = micros() - t0;

        // Read test
        uint32_t t1 = micros();
        size_t nr = 0;
        bool rok = readFile(testFile, rbuf, blockSize, &nr);
        uint32_t rt = micros() - t1;

        // Verify data integrity byte-by-byte
        bool vok = rok && (nr == blockSize) && (memcmp(wbuf, rbuf, blockSize) == 0);
        if (!vok) allVerify = false;

        if (wt < writeMin) writeMin = wt;
        if (wt > writeMax) writeMax = wt;
        writeSum += wt;
        if (rt < readMin) readMin = rt;
        if (rt > readMax) readMax = rt;
        readSum += rt;

        r.write_ok = wok;
        r.read_ok  = rok;

        DBG_INFO(TAG, "cycle %d/%d: write %luus read %luus verify:%s",
                 i + 1, cycles,
                 (unsigned long)wt, (unsigned long)rt,
                 vok ? "PASS" : "FAIL");

        r.cycles_completed = i + 1;
    }

    // Append test: 10 appends of blockSize bytes, verify total size
    removeFile(appendPath);
    uint32_t at0 = micros();
    for (int a = 0; a < 10; a++) {
        memset(wbuf, (char)(0xBB + a), blockSize);
        appendFile(appendPath, wbuf, blockSize);
    }
    appendTotal = micros() - at0;

    size_t expectedAppendSize = blockSize * 10;
    size_t actualAppendSize = fileSize(appendPath);
    if (actualAppendSize != expectedAppendSize) {
        DBG_WARN(TAG, "Append size mismatch: expected %zu got %zu",
                 expectedAppendSize, actualAppendSize);
    }
    removeFile(appendPath);

    // Directory test
    bool mkOk = mkdir(testDir);
    bool dirExists = exists(testDir);
    bool rmOk = rmdir(testDir);
    r.dir_ok = mkOk && dirExists && rmOk;

    // Fill result
    r.write_us       = writeSum / (uint32_t)cycles;
    r.read_us        = readSum / (uint32_t)cycles;
    r.append_us      = appendTotal;
    r.bytes_written  = blockSize;
    r.bytes_read     = blockSize;
    r.verify_ok      = allVerify;
    r.format_ok      = false; // Not tested when cycles > 0 (destructive)

    // Cleanup
    removeFile(testFile);

    bool allPassed = r.write_ok && r.read_ok && r.verify_ok && r.dir_ok;
    DBG_INFO(TAG, "Test complete: %d cycles, write avg %luus, read avg %luus, all passed: %s",
             cycles,
             (unsigned long)r.write_us, (unsigned long)r.read_us,
             allPassed ? "YES" : "NO");

    free(wbuf);
    free(rbuf);
    return r;
}

#endif // SIMULATOR
