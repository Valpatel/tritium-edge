# Waveshare ESP32-S3 Board Reference

## Board Summary

| Board | Display | Resolution | Driver | Interface | Touch | IMU | Pins Verified |
|-------|---------|-----------|--------|-----------|-------|-----|---------------|
| [ESP32-S3-AMOLED-1.91-M][wiki-191m] | 1.91" AMOLED | 240x536 | RM67162 | QSPI | FT3168 | QMI8658 | No |
| [ESP32-S3-Touch-AMOLED-2.41-B][wiki-241b] | 2.41" AMOLED | 600x450 | RM690B0 | QSPI | FT6336 | QMI8658 | Yes |
| [ESP32-S3-Touch-AMOLED-1.8][wiki-18] | 1.8" AMOLED | 368x448 | SH8601 | QSPI | FT3168 | QMI8658 | No |
| [ESP32-S3-Touch-LCD-3.5B-C][wiki-35bc] | 3.5" IPS LCD | 320x480 | AXS15231B | QSPI | Integrated | QMI8658 | No |
| [ESP32-S3-Touch-LCD-4.3C-BOX][wiki-43c] | 4.3" IPS LCD | 800x480 | ST7262 | RGB Parallel | GT911 | None | Yes |
| [ESP32-S3-Touch-LCD-3.49][wiki-349] | 3.49" IPS LCD | 172x640 | AXS15231B | QSPI | Integrated | QMI8658 | No |

[wiki-191m]: https://www.waveshare.com/wiki/ESP32-S3-AMOLED-1.91-M
[wiki-241b]: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.41-B
[wiki-18]: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8
[wiki-35bc]: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.5B-C
[wiki-43c]: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3C-BOX
[wiki-349]: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49

## Common Specs (All Boards)

- MCU: ESP32-S3 (Xtensa dual-core LX7, 240MHz)
- Flash: 16MB
- PSRAM: 8MB
- WiFi: 2.4GHz 802.11 b/g/n
- Bluetooth: BLE 5
- USB: Type-C

## Per-Board Details

### ESP32-S3-AMOLED-1.91-M
- Part: ESP32-S3-AMOLED-1.91-M (SKU 28873)
- "-M" variant = pre-soldered male headers
- Display: RM67162 AMOLED, 240x536, 16.7M colors, QSPI
- Touch: FT3168 capacitive (I2C)
- Sensors: QMI8658 6-axis IMU (accel + gyro)
- PlatformIO env: `amoled-191m`

### ESP32-S3-Touch-AMOLED-2.41-B
- Part: ESP32-S3-Touch-AMOLED-2.41-B (SKU 30589)
- Display: RM690B0 AMOLED, 600x450, 16.7M colors, QSPI
- Touch: FT6336 5-point capacitive (I2C via GPIO47/48)
- Sensors: QMI8658 IMU, PCF85063 RTC
- IO Expander: yes (EXIO pins for TE, touch INT, IMU INT)
- PlatformIO env: `touch-amoled-241b`

### ESP32-S3-Touch-AMOLED-1.8
- Part: ESP32-S3-Touch-AMOLED-1.8 (SKU 29957)
- Display: SH8601 AMOLED, 368x448, 16.7M colors, QSPI
- Touch: FT3168 capacitive (I2C)
- Sensors: QMI8658 IMU, PCF85063 RTC
- Power: AXP2101 PMIC, battery connector
- Audio: ES8311 codec
- PlatformIO env: `touch-amoled-18`

### ESP32-S3-Touch-LCD-3.5B-C
- Part: ESP32-S3-Touch-LCD-3.5B-C (SKU 31334)
- Display: AXS15231B IPS LCD, 320x480, 262K colors, QSPI
- Touch: Integrated in AXS15231B (I2C)
- Sensors: QMI8658 IMU, PCF85063 RTC
- Power: AXP2101 PMIC
- Audio: ES8311 codec
- Camera: OV5640 interface ("-C" variant includes camera)
- PlatformIO env: `touch-lcd-35bc`

### ESP32-S3-Touch-LCD-4.3C-BOX
- Part: ESP32-S3-Touch-LCD-4.3C-BOX (SKU 33630)
- Display: ST7262 IPS LCD, 800x480, 65K colors, 16-bit RGB parallel
- Touch: GT911 5-point capacitive (I2C via GPIO8/9, INT GPIO4)
- IO Expander: CH422G (backlight, reset)
- Audio: Dual-MIC array
- "-BOX" = enclosed in case
- PlatformIO env: `touch-lcd-43c-box`

### ESP32-S3-Touch-LCD-3.49
- Part: ESP32-S3-Touch-LCD-3.49 (SKU 32373)
- Display: AXS15231B IPS LCD, 172x640, 16.7M colors, QSPI
- Touch: Integrated in AXS15231B (I2C)
- Sensors: QMI8658 IMU, PCF85063 RTC
- Power: integrated PMIC
- Audio: ES8311 + ES7210 (dual-mic array)
- IO Expander: TCA9554PWR
- PlatformIO env: `touch-lcd-349`

## Pin Verification Checklist

When a new board arrives, verify pin assignments by:
1. Check Waveshare wiki for latest schematic PDF
2. Run the board's demo code from Waveshare GitHub
3. Update the board header in `include/boards/` with verified pins
4. Update this doc's "Pins Verified" column
