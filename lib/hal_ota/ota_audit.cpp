#include "ota_audit.h"
#include "debug_log.h"
#include <cstdio>
#include <cstring>

static constexpr const char* TAG = "ota_audit";

#ifndef SIMULATOR

#include "tritium_compat.h"
#include <sys/stat.h>

// LittleFS is mounted at /littlefs via VFS — use POSIX file ops
static const char* LFS_PREFIX = "/littlefs";
static const char* AUDIT_LOG_PATH = "/ota_audit.log";

static void audit_fullpath(char* out, size_t outSize) {
    snprintf(out, outSize, "%s%s", LFS_PREFIX, AUDIT_LOG_PATH);
}

bool OtaAuditLog::init() {
    // LittleFS is already mounted by webserver_service — just verify
    struct stat st;
    if (stat(LFS_PREFIX, &st) != 0) {
        DBG_ERROR(TAG, "LittleFS not mounted at %s", LFS_PREFIX);
        return false;
    }
    _ready = true;
    DBG_INFO(TAG, "Audit log ready (%s)", LOG_PATH);
    return true;
}

void OtaAuditLog::logAttempt(const char* source, const char* version,
                              const char* board, bool success, const char* detail) {
    if (!_ready) return;

    // Build JSON line
    char line[256];
    int n;
    if (!success && detail) {
        n = snprintf(line, sizeof(line),
                     "{\"t\":%lu,\"src\":\"%s\",\"ver\":\"%s\",\"board\":\"%s\","
                     "\"ok\":false,\"err\":\"%s\"}\n",
                     (unsigned long)(millis() / 1000),
                     source ? source : "?",
                     version ? version : "?",
                     board ? board : "?",
                     detail);
    } else {
        n = snprintf(line, sizeof(line),
                     "{\"t\":%lu,\"src\":\"%s\",\"ver\":\"%s\",\"board\":\"%s\","
                     "\"ok\":%s}\n",
                     (unsigned long)(millis() / 1000),
                     source ? source : "?",
                     version ? version : "?",
                     board ? board : "?",
                     success ? "true" : "false");
    }

    // Append to log file
    char fp[256];
    audit_fullpath(fp, sizeof(fp));
    FILE* f = fopen(fp, "a");
    if (f) {
        fwrite(line, 1, n, f);
        fclose(f);
        DBG_INFO(TAG, "Logged: %s v%s %s", source, version, success ? "OK" : "FAIL");
    }

    // Trim if too large
    trimLog();
}

size_t OtaAuditLog::readLog(char* buf, size_t bufSize) {
    if (!_ready || !buf || bufSize == 0) return 0;

    char fp[256];
    audit_fullpath(fp, sizeof(fp));
    FILE* f = fopen(fp, "r");
    if (!f) return 0;

    size_t n = fread(buf, 1, bufSize - 1, f);
    buf[n] = '\0';
    fclose(f);
    return n;
}

int OtaAuditLog::getEntryCount() {
    if (!_ready) return 0;

    char fp[256];
    audit_fullpath(fp, sizeof(fp));
    FILE* f = fopen(fp, "r");
    if (!f) return 0;

    int count = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') count++;
    }
    fclose(f);
    return count;
}

void OtaAuditLog::clear() {
    if (!_ready) return;
    char fp[256];
    audit_fullpath(fp, sizeof(fp));
    remove(fp);
    DBG_INFO(TAG, "Audit log cleared");
}

void OtaAuditLog::trimLog() {
    if (!_ready) return;

    char fp[256];
    audit_fullpath(fp, sizeof(fp));

    struct stat st;
    if (stat(fp, &st) != 0) return;
    size_t fileSize = st.st_size;
    if (fileSize <= MAX_LOG_SIZE) return;

    // Read file, skip first half of entries
    FILE* f = fopen(fp, "r");
    if (!f) return;

    size_t skipBytes = fileSize - MAX_LOG_SIZE / 2;
    fseek(f, skipBytes, SEEK_SET);

    // Find next newline (start of complete entry)
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') break;
    }

    // Read remaining data
    long pos = ftell(f);
    size_t remaining = fileSize - pos;
    char* buf = (char*)malloc(remaining + 1);
    if (!buf) { fclose(f); return; }

    size_t n = fread(buf, 1, remaining, f);
    buf[n] = '\0';
    fclose(f);

    // Rewrite file with trimmed content
    f = fopen(fp, "w");
    if (f) {
        fwrite(buf, 1, n, f);
        fclose(f);
        DBG_INFO(TAG, "Trimmed audit log: %u -> %u bytes", fileSize, n);
    }
    free(buf);
}

#else // SIMULATOR

bool OtaAuditLog::init() { return false; }
void OtaAuditLog::logAttempt(const char*, const char*, const char*, bool, const char*) {}
size_t OtaAuditLog::readLog(char*, size_t) { return 0; }
int OtaAuditLog::getEntryCount() { return 0; }
void OtaAuditLog::clear() {}
void OtaAuditLog::trimLog() {}

#endif
