# hal_radio_scheduler — BLE/WiFi Time-Division Multiplexing

The ESP32-S3 cannot run NimBLE and WiFi simultaneously due to shared radio and memory constraints. This scheduler alternates between WiFi-active and BLE-active time slots, gracefully tearing down one stack before bringing up the other.

## Time Slots

- **WiFi slot** (default 25s): MQTT heartbeats, OTA checks, config sync
- **BLE slot** (default 10s): BLE scanning for device discovery

## Files

| File | Purpose |
|------|---------|
| `hal_radio_scheduler.h` | API: init, tick, config struct, slot callbacks |
| `hal_radio_scheduler.cpp` | Slot state machine, stack teardown/bringup |

## Events

Publishes `os_events` on radio mode changes so other services can react (e.g., MQTT bridge pauses during BLE slot).
