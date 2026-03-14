# hal_rf_monitor — RF Motion Detection

Tracks RSSI variance between Tritium peer nodes to detect physical motion in the RF environment. When a person or object moves between two nodes, the RSSI between them fluctuates due to multipath interference. High variance = motion detected.

## How It Works

- **Data source**: ESP-NOW mesh peer RSSI values (from EspNowHAL)
- **Algorithm**: Sliding window variance over 60 samples per peer
- **Output**: JSON array for MQTT sighting topic and SC RF motion plugin

## Files

| File | Purpose |
|------|---------|
| `hal_rf_monitor.h` | API: init, tick, get_json, peer tracking structs |
| `hal_rf_monitor.cpp` | RSSI variance computation, motion classification |

## Configuration

- `MAX_PEERS`: 16 simultaneous tracked peers
- `WINDOW_SIZE`: 60-sample sliding window
- Threshold configurable via fleet server config sync
