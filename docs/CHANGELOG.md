# Tritium-Edge Changelog

Changes tracked with verification status. All changes on `dev` branch.

## Verification Levels

| Level | Meaning |
|-------|---------|
| **Build Verified** | `pio run -e touch-lcd-43c-box-os` passes, RAM <60%, Flash <50% |
| **Serial Verified** | Verified via serial monitor on real hardware |
| **Hardware Verified** | Full test on physical device (display, touch, networking) |
| **Human Verified** | Manually tested by a human |
| **Deployed** | Running on real hardware in the field |

---

## 2026-03-14 — Wave 26: BLE RSSI History

### BLE Scanner
| Change | Verification |
|--------|-------------|
| RSSI history: circular buffer of 30 readings per BleDevice with timestamps | Build Verified |
| Trend analysis: approaching/departing/stable based on 3dBm threshold | Build Verified |
| New API: `get_rssi_history_json()` and `get_rssi_history_json_by_mac()` | Build Verified |
| New serial command: `BLE_RSSI AA:BB:CC:DD:EE:FF` | Build Verified |

### Fleet Server
| Change | Verification |
|--------|-------------|
| `GET /api/ble/devices/{mac}/rssi_history` — RSSI history with min/max/avg/trend | Unit Tested |

---

## 2026-03-14 — Wave 25: Maintenance & Quality

### Documentation
| Change | Verification |
|--------|-------------|
| READMEs added for hal_rf_monitor, hal_config_sync, hal_radio_scheduler, hal_sighting_buffer, hal_cot | Documented |
| Verified Apple Continuity data (device_type, class) present in BLE sighting JSON | Code Verified |

---

## 2026-03-14 — Wave 15: BLE Scanner Enhancements

### BLE Scanner Caching & Batch Publishing
| Change | Verification |
|--------|-------------|
| `hal_ble_scanner` — configurable cache_ttl_ms (30s default) for scan result freshness | Build Verified (RAM 47.9%, Flash 29.0%) |
| `is_cache_valid()` and `cache_age_ms()` for consumers to check result staleness | Build Verified |
| `get_devices_json_batch()` for chunked MQTT publishing (configurable batch_size, default 10) | Build Verified |
| `get_batch_count()` to calculate number of batches needed | Build Verified |
| ScanConfig extended with `cache_ttl_ms` and `batch_size` fields | Build Verified |

---

## 2026-03-13

### MQTT SC Bridge
| Change | Verification |
|--------|-------------|
| `lib/hal_mqtt/mqtt_sc_bridge.cpp/h` — MQTT bridge for heartbeat, sighting, commands | Build Verified |
| Publishes to `tritium/{device_id}/status` (retained, LWT) | Build Verified |
| Publishes to `tritium/{device_id}/heartbeat` every 30s | Build Verified |
| Publishes to `tritium/{device_id}/sighting` every 15s (BLE/WiFi) | Build Verified |
| Subscribes to `tritium/{device_id}/cmd/#` for SC commands | Build Verified |
| `mqtt_ai_bridge.cpp` — stub when hal_audio unavailable | Build Verified |

### WiFi Management
| Change | Verification |
|--------|-------------|
| `wifi_manager.cpp` — `moveNetwork()` for saved network reordering | Build Verified |
| `shell_apps.cpp` — up/down/delete buttons per saved WiFi network | Build Verified |

### Documentation
| Change | Verification |
|--------|-------------|
| `docs/README.md` — full 24-document index with categories | N/A (docs) |
| `CLAUDE.md` — fixed build path from `esp32-hardware/` to root | N/A (docs) |
| `LICENSE` — AGPL-3.0 added | N/A (legal) |

---

## Build Baseline

| Metric | Value | Date |
|--------|-------|------|
| RAM usage | 45.9% (150,248 / 327,680) | 2026-03-13 |
| Flash usage | 28.8% (1,887,199 / 6,553,600) | 2026-03-13 |
| Warnings | 0 | 2026-03-13 |
| Board | touch-lcd-43c-box-os | — |

## Known Issues

| Issue | Status | Impact |
|-------|--------|--------|
| BLE scanner disabled (WiFi/BLE coex) | Open | No real BLE data; MQTT pipeline ready |
| NimBLE esp_bt.h not found | Open | Blocks BLE serial + BLE OTA |
| RGB display glitches (43C + USB) | Cosmetic | Memory bus contention |
