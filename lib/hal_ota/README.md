# hal_ota - OTA Update HAL

Over-The-Air firmware update library for ESP32-S3 supporting three update methods:
WiFi push (HTTP upload), WiFi pull (download from URL), and SD card.

## Requirements

- ESP32-S3 with dual OTA partition scheme (e.g., `min_spiffs.csv` or custom)
- WiFi connected for push/pull OTA modes
- SD card mounted via SD_MMC for SD card OTA mode

## Usage

### WiFi Push OTA (HTTP Upload)

Start an HTTP server and browse to the upload page from any device on the network.

```cpp
#include "hal_ota.h"
#include "wifi_manager.h"

OtaHAL ota;
WifiManager wifi;

void setup() {
    wifi.init();
    wifi.connect();

    ota.init();
    ota.startServer(8080);  // Browse to http://<ip>:8080/update

    ota.onStateChange([](OtaState state, const char* msg) {
        Serial.printf("OTA state: %d - %s\n", (int)state, msg);
    });

    ota.onProgress([](size_t current, size_t total) {
        Serial.printf("OTA progress: %u / %u\n", current, total);
    });
}

void loop() {
    ota.process();  // Required - handles HTTP clients

    if (ota.getState() == OtaState::READY_REBOOT) {
        delay(1000);
        ota.reboot();
    }
}
```

### WiFi Pull OTA (Download from URL)

Download firmware from a remote HTTP server.

```cpp
#include "hal_ota.h"

OtaHAL ota;

void updateFromServer() {
    ota.init();

    ota.onProgress([](size_t current, size_t total) {
        Serial.printf("Download: %u%%\n", (uint8_t)((current * 100) / total));
    });

    if (ota.updateFromUrl("http://192.168.1.10:8000/firmware.bin")) {
        Serial.println("Update successful, rebooting...");
        ota.reboot();
    } else {
        Serial.printf("Update failed: %s\n", ota.getLastError());
    }
}
```

### SD Card OTA

Read firmware from an SD card file. After a successful flash the firmware file
is renamed to `firmware.bin.bak` to prevent re-flashing on next boot.

```cpp
#include "hal_ota.h"

OtaHAL ota;

void updateFromSD() {
    ota.init();

    if (ota.updateFromSD("/firmware.bin")) {
        Serial.println("SD update successful, rebooting...");
        ota.reboot();
    } else {
        Serial.printf("SD update failed: %s\n", ota.getLastError());
    }
}
```

### Rollback

If the new firmware misbehaves, roll back to the previous version
(requires dual OTA partitions).

```cpp
if (ota.canRollback()) {
    ota.rollback();
    ota.reboot();
}
```

### Diagnostics / Self-Test

Run the built-in test harness to verify OTA infrastructure without flashing.

```cpp
OtaHAL ota;
ota.init();

auto result = ota.runTest();
Serial.printf("Partitions OK:  %s\n", result.partition_ok ? "yes" : "no");
Serial.printf("Server start:   %s\n", result.server_start_ok ? "yes" : "no");
Serial.printf("Server stop:    %s\n", result.server_stop_ok ? "yes" : "no");
Serial.printf("Rollback check: %s\n", result.rollback_check_ok ? "yes" : "no");
Serial.printf("SD card:        %s\n", result.sd_detect_ok ? "yes" : "no");
Serial.printf("Running:        %s\n", result.running_partition);
Serial.printf("Max FW size:    %u\n", result.max_firmware_size);
Serial.printf("Test took:      %u ms\n", result.test_duration_ms);
```

## Partition Table

Your `partitions.csv` must include two OTA app partitions. Example:

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xE000,  0x2000
app0,     app,  ota_0,   0x10000, 0x1E0000
app1,     app,  ota_1,   0x1F0000,0x1E0000
spiffs,   data, spiffs,  0x3D0000,0x30000
```

## Simulator

When compiled with `-DSIMULATOR`, all methods return `false` / no-op.
`getState()` returns `OtaState::IDLE` and `runTest()` returns all-false results.
