# hal_config_sync — Remote Configuration Sync

Pulls device config from the fleet server on boot and applies it to NVS. Subscribes to MQTT for live config pushes. Reports config version in heartbeat for SC to verify sync state.

## Synced Fields

- `heartbeat_interval_s`
- `sighting_interval_s`
- `scan_interval_s`
- `display_brightness`
- `display_timeout_s`
- `rf_monitor_threshold_dbm`
- `rf_monitor_window_s`
- `config_version` (monotonic, only apply if newer)

## Files

| File | Purpose |
|------|---------|
| `hal_config_sync.h` | API: init, apply_config, get_version |
| `hal_config_sync.cpp` | HTTP fetch, MQTT subscription, NVS write |

## Transport

- **Boot**: HTTP GET from fleet server `/api/devices/{id}/config`
- **Live**: MQTT subscribe to `tritium/{device_id}/cmd/config`
