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

## 2026-03-14 — Wave 45: Multi-Vendor BLE Ad Parsing

### hal_ble_scanner enhancement (Code Review Verified)
- Added manufacturer-specific BLE advertising data parsing for non-Apple vendors
- Samsung (0x0075, 0x01D5): Galaxy phone/watch/buds/tab classification from type hint byte
- Google Fast Pair (0x00E0, 0x02E0): Model ID extraction, audio device classification
- Microsoft CDP (0x0006): Windows Desktop, Xbox, Surface Hub, HoloLens, WinPhone, WinIoT
- Fitbit (0x0224): Fitness tracker vs smartwatch classification from product type byte
- New `manufacturer` field in BleDevice struct — "Apple", "Samsung", "Google", "Microsoft", "Fitbit"
- Manufacturer included in get_devices_json() and get_device_extended_json() output
- Non-Apple parsing runs after Apple Continuity parser, only if device_class is still UNKNOWN
- Integrated into both new-device discovery and existing-device update paths

---

## 2026-03-14 — Wave 44: LED Status Indicator HAL

### hal_led (Code Review Verified)
- `hal_led.h/cpp` — Low-level LED driver for WS2812/NeoPixel via ESP32-S3 RMT peripheral
- Status colors: GREEN (connected), BLUE (scanning), RED (error), YELLOW (OTA), PURPLE (motion), CYAN (MQTT), WHITE (boot)
- `set_status()`, `set_rgb()`, `pulse()`, `off()`, `set_brightness()`
- Simple GPIO LED fallback for boards without addressable LEDs
- `led_service.h/cpp` — High-level integration with system status
- Auto-detect via HAS_NEOPIXEL/BOARD_LED_PIN/LED_BUILTIN defines
- Priority-based color selection with override/expiry support
- `update(SystemStatus)` called from main loop

---

## 2026-03-14 — Wave 39: Documentation Pass

- CLAUDE.md updated with all 42 HALs in directory structure section
- Previously undocumented HALs now listed: hal_acoustic, hal_acoustic_modem, hal_ble, hal_ble_ota, hal_ble_scanner, hal_ble_serial, hal_config_sync, hal_cot, hal_debug, hal_diag, hal_diaglog, hal_espnow, hal_fs, hal_gis, hal_heartbeat, hal_io_expander, hal_lora, hal_meshtastic, hal_mqtt, hal_ntp, hal_ota, hal_power_saver, hal_provision, hal_radio_scheduler, hal_rf_monitor, hal_sdcard, hal_seed, hal_sighting_buffer, hal_sleep, hal_touch, hal_ui, hal_voice, hal_watchdog, hal_webserver, hal_wifi_probe, hal_wifi_scanner

---

## 2026-03-14 — Wave 38: Power-Saving Mode

### hal_power_saver — Battery-Aware Scan Interval Manager (Build Verified)
- New `hal_power_saver` library — automatic power-saving when battery drops below 20%
- Three power states: NORMAL, POWER_SAVE (<20%), CRITICAL (<10%), CHARGING
- POWER_SAVE mode: BLE scan 10s->30s, WiFi 30s->120s, heartbeat 30s->120s, brightness 10%
- CRITICAL mode: BLE 60s, WiFi 5min, heartbeat 5min, brightness minimal
- Hysteresis: enters at 20%, exits at 25% to prevent oscillation
- Resumes normal operation when charging/USB detected
- JSON status output for serial and API reporting
- Singleton `PowerSaver::instance()` integrates with PowerService tick loop

---

## 2026-03-14 — Wave 35: BLE Raw Advertisement Capture

### hal_ble_scanner Raw Advertisement Payload (Build Verified)
- Added `raw_adv[62]` and `raw_adv_len` fields to `BleDevice` struct
- Captures full BLE advertisement payload on first detection via `getPayload()`
- Raw data included as base64 in `get_devices_json()` output (`raw_adv` field)
- New `get_device_extended_json()` API for per-device detailed JSON with raw adv
- New `BLE_EXT AA:BB:CC:DD:EE:FF` serial command for extended device info
- Embedded base64 encoder (`_base64_enc`) for compact binary-to-text conversion
- Simulator stubs updated for new API
- Enables SC-side deep parsing of manufacturer-specific data without firmware updates

---

## 2026-03-14 — Wave 33: WiFi Probe MQTT Integration

### mqtt_sc_bridge WiFi Probe Integration (Build Verified)
- Wired `hal_wifi_probe` to `mqtt_sc_bridge` for automatic MQTT publishing
  - Probe summary included in periodic heartbeat JSON (`wifi_probe` field)
  - Probe device list published as `wifi_probe` sighting type in `publish_sightings()`
  - Conditional compilation via `#if HAS_WIFI_PROBE` (auto-detected from header)
  - Publishes device MAC, RSSI, channel, probe count, randomization flag, probed SSIDs
  - Flow: ESP32 promiscuous mode -> hal_wifi_probe -> mqtt_sc_bridge -> SC edge_tracker plugin

---

## 2026-03-14 — Wave 31: WiFi Probe Request Capture

### WiFi Probe HAL (`lib/hal_wifi_probe/`)
- New HAL for passive WiFi probe request capture (Build Verified)
  - Uses ESP32 promiscuous mode to capture management frames
  - Extracts source MAC and probed SSIDs from probe requests
  - Detects MAC randomization (locally administered bit)
  - Tracks RSSI min/max range per device
  - Up to 8 probed SSIDs tracked per device
  - 10-minute device timeout, 64 device capacity
  - Optional channel hopping (1-13) for broader coverage
- `wifi_probe_service.h` — ServiceInterface adapter (Build Verified)
  - Priority 55 (after WiFi + MQTT)
  - Periodic MQTT publishing of wifi_probe sightings
  - Serial commands: `WIFI_PROBE_STATUS`, `WIFI_PROBE_LIST`
  - JSON API for web dashboard summary

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
