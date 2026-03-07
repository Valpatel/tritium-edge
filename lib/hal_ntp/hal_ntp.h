#pragma once
#include <cstdint>

// NTP time synchronization.
// Syncs system time from internet NTP servers, then pushes to RTC if available.
class NtpHAL {
public:
    bool init(const char* tz = "UTC",
              const char* server1 = "pool.ntp.org",
              const char* server2 = "time.nist.gov");

    // Trigger sync (also happens automatically on init)
    bool sync();
    bool isSynced() const;

    // Get current time
    uint32_t getEpoch() const;
    bool getTime(int& year, int& month, int& day, int& hour, int& min, int& sec) const;
    const char* getTimeStr() const;   // "2026-03-06 14:30:00"
    const char* getDateStr() const;   // "2026-03-06"

    // Timezone
    void setTimezone(const char* tz);  // POSIX TZ string e.g. "EST5EDT"

    // Sync status
    uint32_t getLastSyncEpoch() const;
    int32_t getDriftMs() const;        // Estimated drift since last sync

    // Test harness
    struct TestResult {
        bool init_ok;
        bool sync_ok;
        bool time_valid;      // Year > 2024
        uint32_t sync_time_ms;  // How long sync took
        uint32_t epoch;
        char time_str[32];
        uint32_t test_duration_ms;
    };
    TestResult runTest();

private:
    bool _synced = false;
    uint32_t _lastSync = 0;
    char _timeStr[32] = {0};
    char _dateStr[16] = {0};
};
