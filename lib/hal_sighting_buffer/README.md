# hal_sighting_buffer — SD Card Sighting Storage

Buffers BLE/WiFi sighting data in RAM and periodically flushes to SD card for persistent local storage. Enables offline operation when MQTT is unavailable.

## Files

| File | Purpose |
|------|---------|
| `hal_sighting_buffer.h` | API: init, record, flush, config struct |
| `hal_sighting_buffer.cpp` | RAM buffer, SD card file rotation, JSONL format |
| `sighting_buffer_service.h` | Service wrapper for OS service manager |

## Configuration

- `base_dir`: SD card directory (default `/sdcard/sightings`)
- `flush_interval_ms`: Flush to SD every 10s
- `max_file_size`: 1MB per file, then rotate
- `max_memory_entries`: 100 entries buffered in RAM before flush
