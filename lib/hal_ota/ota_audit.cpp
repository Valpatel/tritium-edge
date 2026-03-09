#include "ota_audit.h"
#include "debug_log.h"
#include <cstdio>
#include <cstring>

static constexpr const char* TAG = "ota_audit";

#ifndef SIMULATOR

#include <LittleFS.h>
#include <Arduino.h>

bool OtaAuditLog::init() {
    if (!LittleFS.begin(true)) {
        DBG_ERROR(TAG, "LittleFS mount failed");
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
    int n = snprintf(line, sizeof(line),
                     "{\"t\":%lu,\"src\":\"%s\",\"ver\":\"%s\",\"board\":\"%s\","
                     "\"ok\":%s%s%s}\n",
                     (unsigned long)(millis() / 1000),
                     source ? source : "?",
                     version ? version : "?",
                     board ? board : "?",
                     success ? "true" : "false",
                     (!success && detail) ? ",\"err\":\"" : "",
                     (!success && detail) ? detail : "");

    // Close the detail string if present
    if (!success && detail) {
        // Need to append closing quote + brace
        if (n > 0 && n < (int)sizeof(line) - 3) {
            // Replace the trailing }\n with "}\n
            if (line[n-2] == '}') {
                line[n-2] = '"';
                line[n-1] = '}';
                line[n] = '\n';
                line[n+1] = '\0';
                n++;
            }
        }
    }

    // Append to log file
    File f = LittleFS.open(LOG_PATH, FILE_APPEND);
    if (f) {
        f.print(line);
        f.close();
        DBG_INFO(TAG, "Logged: %s v%s %s", source, version, success ? "OK" : "FAIL");
    }

    // Trim if too large
    trimLog();
}

size_t OtaAuditLog::readLog(char* buf, size_t bufSize) {
    if (!_ready || !buf || bufSize == 0) return 0;

    File f = LittleFS.open(LOG_PATH, FILE_READ);
    if (!f) return 0;

    size_t n = f.readBytes(buf, bufSize - 1);
    buf[n] = '\0';
    f.close();
    return n;
}

int OtaAuditLog::getEntryCount() {
    if (!_ready) return 0;

    File f = LittleFS.open(LOG_PATH, FILE_READ);
    if (!f) return 0;

    int count = 0;
    while (f.available()) {
        if (f.read() == '\n') count++;
    }
    f.close();
    return count;
}

void OtaAuditLog::clear() {
    if (!_ready) return;
    LittleFS.remove(LOG_PATH);
    DBG_INFO(TAG, "Audit log cleared");
}

void OtaAuditLog::trimLog() {
    if (!_ready) return;

    File f = LittleFS.open(LOG_PATH, FILE_READ);
    if (!f) return;

    size_t fileSize = f.size();
    if (fileSize <= MAX_LOG_SIZE) {
        f.close();
        return;
    }

    // Read into buffer, skip first half of entries
    size_t skipBytes = fileSize - MAX_LOG_SIZE / 2;
    f.seek(skipBytes);

    // Find next newline (start of complete entry)
    while (f.available()) {
        if (f.read() == '\n') break;
    }

    // Read remaining data
    size_t remaining = f.size() - f.position();
    char* buf = (char*)malloc(remaining + 1);
    if (!buf) {
        f.close();
        return;
    }

    size_t n = f.readBytes(buf, remaining);
    buf[n] = '\0';
    f.close();

    // Rewrite file with trimmed content
    f = LittleFS.open(LOG_PATH, FILE_WRITE);
    if (f) {
        f.write((uint8_t*)buf, n);
        f.close();
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
