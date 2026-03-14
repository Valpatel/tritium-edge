# hal_cot — Cursor on Target (CoT) for TAK

Generates MIL-STD-2045 CoT XML so edge devices appear in ATAK, WinTAK, and WebTAK. Follows tritium-lib/cot conventions for UID format, type codes, and metadata elements.

## Transport

- **UDP multicast**: `239.2.3.1:6969` (standard TAK SA)
- **TCP streaming**: Length-prefixed XML to a TAK server

## Files

| File | Purpose |
|------|---------|
| `hal_cot.h` | API: init, send_sa, config struct, CoT type constants |
| `hal_cot.cpp` | XML generation, UDP/TCP transport |
| `cot_service.h` | Service wrapper for periodic SA broadcasts |

## CoT Details

- UID format: `tritium-{device_id}`
- Group: `__group` detail element with role/name
- Custom `tritium_edge` element with battery, firmware version, sensors
