#pragma once
#include <cstdint>
#include <cstddef>

// OTA Audit Log — persistent record of all OTA update attempts
//
// Records are appended to /ota_audit.log on LittleFS.
// Each line is a JSON object with timestamp, source, version, result.
// The log is capped at MAX_LOG_SIZE; oldest entries are trimmed.

class OtaAuditLog {
public:
    // Initialize with LittleFS (must be mounted)
    bool init();

    // Log an OTA attempt
    void logAttempt(const char* source,      // "wifi_pull", "wifi_push", "sd", "ble", "mesh", "serial"
                    const char* version,      // firmware version string
                    const char* board,        // target board from header
                    bool success,
                    const char* detail = nullptr);  // error message on failure

    // Read the audit log into buffer (returns bytes read)
    size_t readLog(char* buf, size_t bufSize);

    // Get number of entries in the log
    int getEntryCount();

    // Clear the log
    void clear();

private:
    bool _ready = false;
    static constexpr const char* LOG_PATH = "/ota_audit.log";
    static constexpr size_t MAX_LOG_SIZE = 8192;  // 8KB max
    void trimLog();
};
