#include "hal_ntp.h"
#include "debug_log.h"

#ifdef SIMULATOR

// ---------------------------------------------------------------------------
// Simulator stubs — return system time via time()
// ---------------------------------------------------------------------------

#include <ctime>
#include <cstdio>
#include <cstring>

bool NtpHAL::init(const char* tz, const char* server1, const char* server2) {
    (void)server1;
    (void)server2;
    setTimezone(tz);
    _synced = true;
    _lastSync = (uint32_t)time(nullptr);
    DBG_INFO("ntp", "Simulator NTP init (tz=%s)", tz);
    return true;
}

bool NtpHAL::sync() {
    _synced = true;
    _lastSync = (uint32_t)time(nullptr);
    return true;
}

bool NtpHAL::isSynced() const { return _synced; }

uint32_t NtpHAL::getEpoch() const {
    return (uint32_t)time(nullptr);
}

bool NtpHAL::getTime(int& year, int& month, int& day, int& hour, int& min, int& sec) const {
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    if (!tm) return false;
    year  = 1900 + tm->tm_year;
    month = 1 + tm->tm_mon;
    day   = tm->tm_mday;
    hour  = tm->tm_hour;
    min   = tm->tm_min;
    sec   = tm->tm_sec;
    return true;
}

const char* NtpHAL::getTimeStr() const {
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    if (!tm) return "0000-00-00 00:00:00";
    snprintf(const_cast<char*>(_timeStr), sizeof(_timeStr),
             "%04d-%02d-%02d %02d:%02d:%02d",
             1900 + tm->tm_year, 1 + tm->tm_mon, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    return _timeStr;
}

const char* NtpHAL::getDateStr() const {
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    if (!tm) return "0000-00-00";
    snprintf(const_cast<char*>(_dateStr), sizeof(_dateStr),
             "%04d-%02d-%02d",
             1900 + tm->tm_year, 1 + tm->tm_mon, tm->tm_mday);
    return _dateStr;
}

void NtpHAL::setTimezone(const char* tz) {
    (void)tz;
    // Simulator ignores timezone setting
}

uint32_t NtpHAL::getLastSyncEpoch() const { return _lastSync; }

int32_t NtpHAL::getDriftMs() const {
    return 0; // Simulator has no drift
}

NtpHAL::TestResult NtpHAL::runTest() {
    TestResult r = {};
    uint32_t start = (uint32_t)(clock() / (CLOCKS_PER_SEC / 1000));

    r.init_ok = init();
    r.sync_ok = sync();
    r.epoch = getEpoch();

    int year, month, day, hour, min, sec;
    getTime(year, month, day, hour, min, sec);
    r.time_valid = (year > 2024);

    const char* ts = getTimeStr();
    strncpy(r.time_str, ts, sizeof(r.time_str) - 1);

    r.sync_time_ms = 0;
    r.test_duration_ms = (uint32_t)(clock() / (CLOCKS_PER_SEC / 1000)) - start;
    return r;
}

#else // ESP32

// ---------------------------------------------------------------------------
// ESP32-S3 implementation using Arduino configTime / SNTP
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <time.h>
#include <esp_sntp.h>

bool NtpHAL::init(const char* tz, const char* server1, const char* server2) {
    DBG_INFO("ntp", "Init tz=%s servers=%s,%s", tz, server1, server2);

    setTimezone(tz);
    configTime(0, 0, server1, server2);

    // Attempt initial sync
    return sync();
}

bool NtpHAL::sync() {
    DBG_INFO("ntp", "Syncing...");

    uint32_t start = millis();
    uint32_t timeout = 10000; // 10 seconds

    while ((millis() - start) < timeout) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            _synced = true;
            _lastSync = getEpoch();
            DBG_INFO("ntp", "Synced in %ums, epoch=%u", millis() - start, _lastSync);
            return true;
        }
        delay(100);
    }

    DBG_WARN("ntp", "Sync timeout after %ums", millis() - start);

    // Check if time is valid anyway (may have synced before timeout check)
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        if (timeinfo.tm_year > (2024 - 1900)) {
            _synced = true;
            _lastSync = getEpoch();
            return true;
        }
    }

    _synced = false;
    return false;
}

bool NtpHAL::isSynced() const { return _synced; }

uint32_t NtpHAL::getEpoch() const {
    time_t now;
    time(&now);
    return (uint32_t)now;
}

bool NtpHAL::getTime(int& year, int& month, int& day, int& hour, int& min, int& sec) const {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return false;
    year  = 1900 + timeinfo.tm_year;
    month = 1 + timeinfo.tm_mon;
    day   = timeinfo.tm_mday;
    hour  = timeinfo.tm_hour;
    min   = timeinfo.tm_min;
    sec   = timeinfo.tm_sec;
    return true;
}

const char* NtpHAL::getTimeStr() const {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return "0000-00-00 00:00:00";
    snprintf(const_cast<char*>(_timeStr), sizeof(_timeStr),
             "%04d-%02d-%02d %02d:%02d:%02d",
             1900 + timeinfo.tm_year, 1 + timeinfo.tm_mon, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return _timeStr;
}

const char* NtpHAL::getDateStr() const {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return "0000-00-00";
    snprintf(const_cast<char*>(_dateStr), sizeof(_dateStr),
             "%04d-%02d-%02d",
             1900 + timeinfo.tm_year, 1 + timeinfo.tm_mon, timeinfo.tm_mday);
    return _dateStr;
}

void NtpHAL::setTimezone(const char* tz) {
    setenv("TZ", tz, 1);
    tzset();
    DBG_INFO("ntp", "Timezone set: %s", tz);
}

uint32_t NtpHAL::getLastSyncEpoch() const { return _lastSync; }

int32_t NtpHAL::getDriftMs() const {
    if (!_synced || _lastSync == 0) return 0;
    // Rough estimate: typical crystal drift ~20ppm = ~1.7s/day
    uint32_t elapsed_s = getEpoch() - _lastSync;
    // 20 ppm drift estimate in milliseconds
    return (int32_t)((elapsed_s * 20ULL) / 1000);
}

NtpHAL::TestResult NtpHAL::runTest() {
    TestResult r = {};
    uint32_t start = millis();

    // Init with defaults
    r.init_ok = init();

    // Attempt sync (init already tries, but record timing)
    uint32_t syncStart = millis();
    r.sync_ok = _synced;
    if (!r.sync_ok) {
        r.sync_ok = sync();
    }
    r.sync_time_ms = millis() - syncStart;

    // Validate time
    r.epoch = getEpoch();
    int year, month, day, hour, min, sec;
    if (getTime(year, month, day, hour, min, sec)) {
        r.time_valid = (year > 2024);
    } else {
        r.time_valid = false;
    }

    // Copy time string
    const char* ts = getTimeStr();
    strncpy(r.time_str, ts, sizeof(r.time_str) - 1);

    r.test_duration_ms = millis() - start;

    DBG_INFO("ntp", "Test complete: init=%d sync=%d valid=%d epoch=%u time=%s (%ums)",
             r.init_ok, r.sync_ok, r.time_valid, r.epoch, r.time_str,
             r.test_duration_ms);

    return r;
}

#endif // SIMULATOR
