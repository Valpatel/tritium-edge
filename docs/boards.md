# Waveshare ESP32-S3 Board Reference

## Board Summary

| Board | Display | Resolution | Driver | Interface | Touch | IMU | HW Verified |
|-------|---------|-----------|--------|-----------|-------|-----|-------------|
| [ESP32-S3-AMOLED-1.91-M][wiki-191m] | 1.91" AMOLED | 240x536 | RM67162 | QSPI | — | QMI8658 | No |
| [ESP32-S3-Touch-AMOLED-2.41-B][wiki-241b] | 2.41" AMOLED | 450x600 | RM690B0 | QSPI | FT5x06 | QMI8658 | Yes |
| [ESP32-S3-Touch-AMOLED-1.8][wiki-18] | 1.8" AMOLED | 368x448 | SH8601Z | QSPI | FT3168 | QMI8658 | No |
| [ESP32-S3-Touch-LCD-3.5B-C][wiki-35bc] | 3.5" IPS LCD | 320x480 | AXS15231B | QSPI | Integrated | QMI8658 | Yes |
| [ESP32-S3-Touch-LCD-4.3C-BOX][wiki-43c] | 4.3" IPS LCD | 800x480 | ST7262 | RGB Parallel | GT911 | — | No |
| [ESP32-S3-Touch-LCD-3.49][wiki-349] | 3.49" IPS LCD | 172x640 | AXS15231B | QSPI | Integrated | QMI8658 | Yes |

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
- USB: Type-C (native USB CDC)

## Per-Board Details

### ESP32-S3-Touch-AMOLED-2.41-B
- Display: RM690B0 AMOLED, 450x600, QSPI. Memory 452px wide, needs offset_x=16.
- Touch: FT5x06 capacitive (I2C)
- Sensors: QMI8658 IMU, PCF85063 RTC
- PlatformIO env: `touch-amoled-241b`

### ESP32-S3-AMOLED-1.91-M
- Display: RM67162 AMOLED, 240x536, QSPI
- Touch: None (non-touch variant)
- Sensors: QMI8658 IMU
- PlatformIO env: `amoled-191m`

### ESP32-S3-Touch-AMOLED-1.8
- Display: SH8601Z AMOLED, 368x448, QSPI
- Touch: FT3168 capacitive (I2C)
- Sensors: QMI8658 IMU, PCF85063 RTC
- Power: AXP2101 PMIC, battery connector
- Audio: ES8311 codec
- PlatformIO env: `touch-amoled-18`

### ESP32-S3-Touch-LCD-3.5B-C
- Display: AXS15231B IPS LCD, 320x480, QSPI. Needs full register init (~500 bytes).
- **Display reset via TCA9554 I/O expander** (I2C 0x20, pin 1). Must toggle before display.init().
- Touch: Integrated in AXS15231B (I2C 0x3B, 4-byte handshake protocol)
- Sensors: QMI8658 IMU, PCF85063 RTC
- Power: AXP2101 PMIC
- Audio: ES8311 codec (mic + speaker verified)
- Camera: OV5640 DVP ("-C" variant). Needs `setFlip(false, true)` for correct orientation.
- I2C bus (SDA=8, SCL=7) shared by: ES8311 (0x18), TCA9554 (0x20), AXP2101 (0x34), touch (0x3B), PCF85063 (0x51), QMI8658 (0x6B)
- PlatformIO env: `touch-lcd-35bc`, `touch-lcd-35bc-camera`, `touch-lcd-35bc-system`

### ESP32-S3-Touch-LCD-4.3C-BOX
- Display: ST7262 IPS LCD, 800x480, 16-bit RGB parallel
- Touch: GT911 capacitive (I2C)
- Audio: Dual-MIC array
- "-BOX" = enclosed in case
- PlatformIO env: `touch-lcd-43c-box`

### ESP32-S3-Touch-LCD-3.49
- Display: AXS15231B IPS LCD, 172x640, QSPI. Memory 360px wide. Needs full register init.
- Touch: Integrated in AXS15231B (I2C on separate bus: SDA=17, SCL=18, I2C_NUM_1)
- Display on SPI3_HOST (not SPI2_HOST)
- Sensors: QMI8658 IMU, PCF85063 RTC
- Audio: ES8311 + ES7210 dual-mic
- PlatformIO env: `touch-lcd-349`

## Reference Code

Official Waveshare demo code is in `references/` (not committed — download with git clone).
Repos are under the `waveshareteam` GitHub org (not `waveshare`).
Some boards have no GitHub repo; demos are on `files.waveshare.com`.

## Pin Verification Checklist

When a new board arrives:
1. Check the Waveshare wiki for the latest schematic
2. Run the official demo code to confirm the display works
3. Compare pin assignments against our board header in `include/boards/`
4. Update the "HW Verified" column above
