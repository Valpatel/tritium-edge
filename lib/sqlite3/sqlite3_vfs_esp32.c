// Minimal SQLite VFS for ESP32 — wraps POSIX file I/O via ESP-IDF VFS
//
// When SQLite is built with SQLITE_OS_OTHER=1, it expects us to provide
// sqlite3_os_init() / sqlite3_os_end() and register at least one VFS.
// ESP-IDF mounts SD card at /sdcard via its own VFS layer, which exposes
// standard POSIX calls (open/read/write/close/stat). This shim bridges
// SQLite to those POSIX calls with no-op locking (single-threaded).
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sqlite3_config.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

// ---------------------------------------------------------------------------
// File handle — wraps a POSIX fd
// ---------------------------------------------------------------------------
typedef struct Esp32File {
    sqlite3_file base;
    int fd;
} Esp32File;

// ---------------------------------------------------------------------------
// File methods
// ---------------------------------------------------------------------------
static int esp32Close(sqlite3_file* pFile) {
    Esp32File* p = (Esp32File*)pFile;
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    return SQLITE_OK;
}

static int esp32Read(sqlite3_file* pFile, void* buf, int amt, sqlite3_int64 offset) {
    Esp32File* p = (Esp32File*)pFile;
    if (lseek(p->fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
        return SQLITE_IOERR_READ;
    }
    int got = (int)read(p->fd, buf, (size_t)amt);
    if (got == amt) {
        return SQLITE_OK;
    }
    if (got >= 0) {
        // Zero-fill the remainder for short reads
        memset((char*)buf + got, 0, (size_t)(amt - got));
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_IOERR_READ;
}

static int esp32Write(sqlite3_file* pFile, const void* buf, int amt, sqlite3_int64 offset) {
    Esp32File* p = (Esp32File*)pFile;
    if (lseek(p->fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
        return SQLITE_IOERR_WRITE;
    }
    int wrote = (int)write(p->fd, buf, (size_t)amt);
    if (wrote == amt) {
        return SQLITE_OK;
    }
    return SQLITE_IOERR_WRITE;
}

static int esp32Truncate(sqlite3_file* pFile, sqlite3_int64 size) {
    Esp32File* p = (Esp32File*)pFile;
    if (ftruncate(p->fd, (off_t)size) != 0) {
        return SQLITE_IOERR_TRUNCATE;
    }
    return SQLITE_OK;
}

static int esp32Sync(sqlite3_file* pFile, int flags) {
    Esp32File* p = (Esp32File*)pFile;
    (void)flags;
    if (fsync(p->fd) != 0) {
        return SQLITE_IOERR_FSYNC;
    }
    return SQLITE_OK;
}

static int esp32FileSize(sqlite3_file* pFile, sqlite3_int64* pSize) {
    Esp32File* p = (Esp32File*)pFile;
    struct stat st;
    if (fstat(p->fd, &st) != 0) {
        return SQLITE_IOERR_FSTAT;
    }
    *pSize = (sqlite3_int64)st.st_size;
    return SQLITE_OK;
}

// No-op locking — single-threaded (SQLITE_THREADSAFE=0)
static int esp32Lock(sqlite3_file* pFile, int level)       { (void)pFile; (void)level; return SQLITE_OK; }
static int esp32Unlock(sqlite3_file* pFile, int level)     { (void)pFile; (void)level; return SQLITE_OK; }
static int esp32CheckReservedLock(sqlite3_file* pFile, int* pResOut) {
    (void)pFile;
    *pResOut = 0;
    return SQLITE_OK;
}

static int esp32FileControl(sqlite3_file* pFile, int op, void* pArg) {
    (void)pFile; (void)op; (void)pArg;
    return SQLITE_NOTFOUND;
}

static int esp32SectorSize(sqlite3_file* pFile)   { (void)pFile; return 512; }
static int esp32DeviceCharacteristics(sqlite3_file* pFile) { (void)pFile; return 0; }

static const sqlite3_io_methods esp32IoMethods = {
    1,                          // iVersion
    esp32Close,
    esp32Read,
    esp32Write,
    esp32Truncate,
    esp32Sync,
    esp32FileSize,
    esp32Lock,
    esp32Unlock,
    esp32CheckReservedLock,
    esp32FileControl,
    esp32SectorSize,
    esp32DeviceCharacteristics,
    // v2+ methods
    0, 0, 0, 0, 0
};

// ---------------------------------------------------------------------------
// VFS methods
// ---------------------------------------------------------------------------
static int esp32Open(sqlite3_vfs* pVfs, const char* zName, sqlite3_file* pFile,
                     int flags, int* pOutFlags)
{
    (void)pVfs;
    Esp32File* p = (Esp32File*)pFile;
    p->fd = -1;

    int oflags = 0;
    if (flags & SQLITE_OPEN_READWRITE) {
        oflags = O_RDWR;
    } else {
        oflags = O_RDONLY;
    }
    if (flags & SQLITE_OPEN_CREATE) {
        oflags |= O_CREAT;
    }

    if (zName == NULL) {
        // SQLite temp file — use a fixed path (only one at a time, single-threaded)
        zName = "/sdcard/.sqlite_tmp";
        oflags = O_RDWR | O_CREAT | O_TRUNC;
    }

    p->fd = open(zName, oflags, 0644);
    if (p->fd < 0) {
        return SQLITE_CANTOPEN;
    }

    p->base.pMethods = &esp32IoMethods;
    if (pOutFlags) {
        *pOutFlags = flags;
    }
    return SQLITE_OK;
}

static int esp32Delete(sqlite3_vfs* pVfs, const char* zName, int syncDir) {
    (void)pVfs; (void)syncDir;
    if (unlink(zName) != 0 && errno != ENOENT) {
        return SQLITE_IOERR_DELETE;
    }
    return SQLITE_OK;
}

static int esp32Access(sqlite3_vfs* pVfs, const char* zName, int flags, int* pResOut) {
    (void)pVfs; (void)flags;
    struct stat st;
    *pResOut = (stat(zName, &st) == 0) ? 1 : 0;
    return SQLITE_OK;
}

static int esp32FullPathname(sqlite3_vfs* pVfs, const char* zName,
                             int nOut, char* zOut)
{
    (void)pVfs;
    // Paths on ESP-IDF VFS are already absolute (/sdcard/...)
    int n = (int)strlen(zName);
    if (n >= nOut) n = nOut - 1;
    memcpy(zOut, zName, (size_t)n);
    zOut[n] = '\0';
    return SQLITE_OK;
}

static int esp32Randomness(sqlite3_vfs* pVfs, int nByte, char* zOut) {
    (void)pVfs;
    // Use esp_random via stdlib rand as fallback
    for (int i = 0; i < nByte; i++) {
        zOut[i] = (char)(rand() & 0xFF);
    }
    return nByte;
}

static int esp32Sleep(sqlite3_vfs* pVfs, int microseconds) {
    (void)pVfs;
    usleep((useconds_t)microseconds);
    return microseconds;
}

static int esp32CurrentTime(sqlite3_vfs* pVfs, double* pTime) {
    (void)pVfs;
    time_t t;
    time(&t);
    // Julian day for Unix epoch (Jan 1 1970) = 2440587.5
    *pTime = ((double)t / 86400.0) + 2440587.5;
    return SQLITE_OK;
}

static int esp32GetLastError(sqlite3_vfs* pVfs, int nBuf, char* zBuf) {
    (void)pVfs; (void)nBuf; (void)zBuf;
    return 0;
}

// ---------------------------------------------------------------------------
// VFS registration
// ---------------------------------------------------------------------------
static sqlite3_vfs esp32Vfs = {
    1,                      // iVersion
    sizeof(Esp32File),      // szOsFile
    256,                    // mxPathname
    0,                      // pNext
    "esp32",                // zName
    0,                      // pAppData
    esp32Open,
    esp32Delete,
    esp32Access,
    esp32FullPathname,
    0,                      // xDlOpen
    0,                      // xDlError
    0,                      // xDlSym
    0,                      // xDlClose
    esp32Randomness,
    esp32Sleep,
    esp32CurrentTime,
    esp32GetLastError,
    // v2+ methods
    0, 0, 0
};

int sqlite3_os_init(void) {
    return sqlite3_vfs_register(&esp32Vfs, 1 /* makeDefault */);
}

int sqlite3_os_end(void) {
    return SQLITE_OK;
}
