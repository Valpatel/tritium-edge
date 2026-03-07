# hal_ntp -- NTP Time Synchronization

Syncs system time from internet NTP servers on ESP32-S3. Uses the Arduino/ESP-IDF SNTP client to set the internal RTC, with POSIX timezone support and drift estimation.

Simulator builds (`-DSIMULATOR`) return the host system time via `time()`.

## Usage

```cpp
#include "hal_ntp.h"

NtpHAL ntp;

void setup() {
    // After WiFi is connected:
    ntp.init("EST5EDT", "pool.ntp.org", "time.nist.gov");

    if (ntp.isSynced()) {
        uint32_t epoch = ntp.getEpoch();
        const char* time = ntp.getTimeStr();  // "2026-03-06 14:30:00"
        const char* date = ntp.getDateStr();  // "2026-03-06"

        int year, month, day, hour, min, sec;
        ntp.getTime(year, month, day, hour, min, sec);
    }

    // Change timezone at runtime
    ntp.setTimezone("PST8PDT");

    // Re-sync periodically
    ntp.sync();

    // Check drift since last sync
    int32_t drift = ntp.getDriftMs();
}
```

## Test Harness

`runTest()` initializes with default servers, attempts sync (waits up to 10s), verifies the time year is after 2024, and reports sync duration. If WiFi is not connected, `sync_ok` will be `false` and the test completes gracefully.
