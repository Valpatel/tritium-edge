# include/boards/

Per-board pin definitions and feature flags. Each header is force-included at compile time via `-include` in the board's PlatformIO environment.

## Board Headers

| Header | Board | Display | Resolution | Status |
|--------|-------|---------|------------|--------|
| `esp32_s3_touch_amoled_241b.h` | ESP32-S3-Touch-AMOLED-2.41-B | RM690B0 QSPI | 450x600 | HW Verified |
| `esp32_s3_amoled_191m.h` | ESP32-S3-AMOLED-1.91-M | RM67162 QSPI | 240x536 | Needs verification |
| `esp32_s3_touch_amoled_18.h` | ESP32-S3-Touch-AMOLED-1.8 | SH8601Z QSPI | 368x448 | Needs verification |
| `esp32_s3_touch_lcd_35bc.h` | ESP32-S3-Touch-LCD-3.5B-C | AXS15231B QSPI | 320x480 | HW Verified |
| `esp32_s3_touch_lcd_43c_box.h` | ESP32-S3-Touch-LCD-4.3C-BOX | ST7262 RGB | 800x480 | Pin-verified |
| `esp32_s3_touch_lcd_349.h` | ESP32-S3-Touch-LCD-3.49 | AXS15231B QSPI | 172x640 | HW Verified |

## What's Defined

Each board header provides the following categories of defines:

- **Display bus pins**: `LCD_QSPI_CS`, `LCD_QSPI_CLK`, `LCD_QSPI_D0`-`D3`, `LCD_RST`, `LCD_BL` (QSPI) or `LCD_PCLK`, `LCD_HSYNC`, `LCD_VSYNC`, `LCD_DE`, `LCD_R*/G*/B*` (RGB)
- **Touch pins**: `TOUCH_SDA`, `TOUCH_SCL`, `TOUCH_I2C_ADDR`, `TOUCH_I2C_NUM`
- **Peripheral pins**: `IMU_*`, `RTC_*`, `AUDIO_*`, `CAMERA_*`, `SD_*`
- **Feature flags**: `HAS_TOUCH`, `HAS_IMU`, `HAS_RTC`, `HAS_CAMERA`, `HAS_AUDIO`, `HAS_IO_EXPANDER`
- **Display metadata**: `DISPLAY_WIDTH`, `DISPLAY_HEIGHT`, `DISPLAY_ROTATION`, `DISPLAY_DRIVER`, `DISPLAY_IF`, `DISPLAY_BPP`

## Reference

See [../../docs/boards.md](../../docs/boards.md) for detailed specs, schematics, and wiki links.
