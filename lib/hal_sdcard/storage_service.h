// storage_service.h
// Tritium-OS Storage Service — SD card + LittleFS management
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "service.h"

#if __has_include("hal_sdcard.h")
#include "hal_sdcard.h"
#define STORAGE_HAS_SD 1
#else
#define STORAGE_HAS_SD 0
#endif

#if __has_include("hal_fs.h")
#include "hal_fs.h"
#define STORAGE_HAS_FS 1
#else
#define STORAGE_HAS_FS 0
#endif

class StorageService : public ServiceInterface {
public:
    const char* name() const override { return "storage"; }
    uint8_t capabilities() const override { return SVC_SERIAL_CMD | SVC_WEB_API; }
    int initPriority() const override { return 15; }  // Early, after settings but before most services

    bool init() override {
        bool ok = false;
#if STORAGE_HAS_FS
        ok = _fs.init(true) || ok;  // format on fail
#endif
#if STORAGE_HAS_SD
        ok = _sd.init() || ok;
#endif
        return ok;
    }

    void shutdown() override {
#if STORAGE_HAS_SD
        _sd.deinit();
#endif
#if STORAGE_HAS_FS
        _fs.deinit();
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
        // Commands: SD_MOUNT, SD_EJECT, SD_FORMAT, SD_TEST, SD_INFO, FS_INFO, FS_FORMAT, STORAGE
        if (strcmp(cmd, "SD_MOUNT") == 0) {
#if STORAGE_HAS_SD
            if (_sd.isMounted()) { _sd.deinit(); }
            bool ok = _sd.init();
            Serial.printf("[storage] SD mount: %s\n", ok ? "OK" : "FAILED");
            if (ok) {
                Serial.printf("[storage] Type: %s, Total: %lluMB, Used: %lluMB\n",
                    _sd.getFilesystemType(),
                    (unsigned long long)(_sd.totalBytes() / (1024*1024)),
                    (unsigned long long)(_sd.usedBytes() / (1024*1024)));
            }
#else
            Serial.printf("[storage] SD card not available on this board\n");
#endif
            return true;
        }
        if (strcmp(cmd, "SD_EJECT") == 0) {
#if STORAGE_HAS_SD
            _sd.deinit();
            Serial.printf("[storage] SD ejected\n");
#else
            Serial.printf("[storage] SD card not available\n");
#endif
            return true;
        }
        if (strcmp(cmd, "SD_FORMAT") == 0) {
#if STORAGE_HAS_SD
            Serial.printf("[storage] Formatting SD card (this may take a while)...\n");
            bool ok = _sd.format();
            Serial.printf("[storage] SD format: %s\n", ok ? "OK" : "FAILED");
            if (ok && _sd.isMounted()) {
                Serial.printf("[storage] Total: %lluMB, Used: %lluMB\n",
                    (unsigned long long)(_sd.totalBytes() / (1024*1024)),
                    (unsigned long long)(_sd.usedBytes() / (1024*1024)));
            }
#else
            Serial.printf("[storage] SD card not available\n");
#endif
            return true;
        }
        if (strcmp(cmd, "SD_TEST") == 0) {
#if STORAGE_HAS_SD
            if (!_sd.isMounted()) {
                Serial.printf("[storage] SD not mounted\n");
                return true;
            }
            Serial.printf("[storage] Running SD card test...\n");
            auto r = _sd.runTest(5, 4096);
            Serial.printf("[storage] Mount:%s Write:%s Read:%s Verify:%s Delete:%s Mkdir:%s\n",
                r.mount_ok?"OK":"FAIL", r.write_ok?"OK":"FAIL",
                r.read_ok?"OK":"FAIL", r.verify_ok?"OK":"FAIL",
                r.delete_ok?"OK":"FAIL", r.mkdir_ok?"OK":"FAIL");
            Serial.printf("[storage] Write: %uKB/s  Read: %uKB/s  Duration: %ums\n",
                r.write_speed_kbps, r.read_speed_kbps, r.test_duration_ms);
#else
            Serial.printf("[storage] SD card not available\n");
#endif
            return true;
        }
        if (strcmp(cmd, "SD_INFO") == 0) {
#if STORAGE_HAS_SD
            if (!_sd.isMounted()) {
                Serial.printf("[storage] SD: not mounted\n");
                return true;
            }
            Serial.printf("[storage] SD: %s, Total: %lluMB, Used: %lluMB, Free: %lluMB\n",
                _sd.getFilesystemType(),
                (unsigned long long)(_sd.totalBytes() / (1024*1024)),
                (unsigned long long)(_sd.usedBytes() / (1024*1024)),
                (unsigned long long)((_sd.totalBytes() - _sd.usedBytes()) / (1024*1024)));
#else
            Serial.printf("[storage] SD card not available\n");
#endif
            return true;
        }
        if (strcmp(cmd, "FS_INFO") == 0) {
#if STORAGE_HAS_FS
            if (!_fs.isReady()) {
                Serial.printf("[storage] LittleFS: not ready\n");
                return true;
            }
            Serial.printf("[storage] LittleFS: Total: %uKB, Used: %uKB, Free: %uKB\n",
                (unsigned)(_fs.totalBytes() / 1024),
                (unsigned)(_fs.usedBytes() / 1024),
                (unsigned)(_fs.freeBytes() / 1024));
#else
            Serial.printf("[storage] LittleFS not available\n");
#endif
            return true;
        }
        if (strcmp(cmd, "FS_FORMAT") == 0) {
#if STORAGE_HAS_FS
            Serial.printf("[storage] Formatting LittleFS...\n");
            bool ok = _fs.format();
            Serial.printf("[storage] LittleFS format: %s\n", ok ? "OK" : "FAILED");
#else
            Serial.printf("[storage] LittleFS not available\n");
#endif
            return true;
        }
        if (strcmp(cmd, "STORAGE") == 0) {
            // Print summary of all storage
#if STORAGE_HAS_SD
            if (_sd.isMounted()) {
                Serial.printf("[storage] SD: %s  %lluMB / %lluMB\n",
                    _sd.getFilesystemType(),
                    (unsigned long long)(_sd.usedBytes() / (1024*1024)),
                    (unsigned long long)(_sd.totalBytes() / (1024*1024)));
            } else {
                Serial.printf("[storage] SD: not mounted\n");
            }
#endif
#if STORAGE_HAS_FS
            if (_fs.isReady()) {
                Serial.printf("[storage] LittleFS: %uKB / %uKB\n",
                    (unsigned)(_fs.usedBytes() / 1024),
                    (unsigned)(_fs.totalBytes() / 1024));
            } else {
                Serial.printf("[storage] LittleFS: not ready\n");
            }
#endif
            return true;
        }
        return false;
    }

    int toJson(char* buf, size_t bufSize) override {
        int n = snprintf(buf, bufSize, "{");
#if STORAGE_HAS_SD
        bool sdMounted = _sd.isMounted();
        n += snprintf(buf + n, bufSize - n,
            "\"sd\":{\"mounted\":%s", sdMounted ? "true" : "false");
        if (sdMounted) {
            n += snprintf(buf + n, bufSize - n,
                ",\"type\":\"%s\",\"total\":%llu,\"used\":%llu,\"free\":%llu",
                _sd.getFilesystemType(),
                (unsigned long long)_sd.totalBytes(),
                (unsigned long long)_sd.usedBytes(),
                (unsigned long long)(_sd.totalBytes() - _sd.usedBytes()));
        }
        n += snprintf(buf + n, bufSize - n, "}");
#endif
#if STORAGE_HAS_FS
        if (n > 1) n += snprintf(buf + n, bufSize - n, ",");
        bool fsReady = _fs.isReady();
        n += snprintf(buf + n, bufSize - n,
            "\"fs\":{\"ready\":%s", fsReady ? "true" : "false");
        if (fsReady) {
            n += snprintf(buf + n, bufSize - n,
                ",\"total\":%u,\"used\":%u,\"free\":%u",
                (unsigned)_fs.totalBytes(),
                (unsigned)_fs.usedBytes(),
                (unsigned)_fs.freeBytes());
        }
        n += snprintf(buf + n, bufSize - n, "}");
#endif
        n += snprintf(buf + n, bufSize - n, "}");
        return n;
    }

    // Public accessors for other code (shell app, web API)
#if STORAGE_HAS_SD
    SDCardHAL& sd() { return _sd; }
    const SDCardHAL& sd() const { return _sd; }
#endif
#if STORAGE_HAS_FS
    FsHAL& fs() { return _fs; }
    const FsHAL& fs() const { return _fs; }
#endif

private:
#if STORAGE_HAS_SD
    SDCardHAL _sd;
#endif
#if STORAGE_HAS_FS
    FsHAL _fs;
#endif
};
