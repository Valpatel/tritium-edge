# hal_watchdog -- Task Watchdog & System Health

Task watchdog for crash recovery and system health monitoring on ESP32-S3. Uses the ESP-IDF task watchdog timer to detect hangs and automatically resets the device if the main loop stops feeding the watchdog.

Simulator builds (`-DSIMULATOR`) return fake health data and all operations succeed as no-ops.

## Usage

```cpp
#include "hal_watchdog.h"

WatchdogHAL wdt;

void setup() {
    wdt.init(30);  // 30-second timeout

    if (wdt.wasWatchdogReset()) {
        // Last reboot was from a watchdog timeout
    }
}

void loop() {
    wdt.loopStart();
    wdt.feed();

    // ... application logic ...

    // For long operations, pause the watchdog
    wdt.pause();
    // ... long OTA update, etc ...
    wdt.resume();

    wdt.loopEnd();

    // Periodic health check
    WatchdogHAL::Health h = wdt.getHealth();
    // h.free_heap, h.heap_fragmentation, h.max_loop_time_us, etc.
}
```

## Test Harness

`runTest()` initializes the watchdog, feeds it 10 times, tests pause/resume, collects health metrics, and verifies all values are sane (heap > 0, task count > 0). Does NOT trigger an actual watchdog timeout.
