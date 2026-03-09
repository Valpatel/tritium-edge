# Display Debugging Lessons (AXS15231B / ESP32-S3-Touch-LCD-3.49)

## What Works
- Vendor-exact esp_lcd code path: SPI mode 3, 40MHz, SPI3_HOST, QSPI
- Manual GPIO reset (pin 21): HIGH 30ms -> LOW 250ms -> HIGH 30ms BEFORE panel_init()
- Init commands: just SleepOut (0x11) + DisplayOn (0x29) -- ROM defaults handle the rest
- DMA chunked push: 172x64x2=22016 bytes per chunk, 10 chunks for full frame
- PSRAM framebuffer + internal SRAM DMA buffer pattern

## What Doesn't Work
- LovyanGFX Panel_AXS15231B with pioarduino/ESP-IDF 5.x -- display stays black
- Raw SPI (spi_device_transmit) with LovyanGFX init sequence -- display stays black
- SPI_MODE0 (LovyanGFX uses MODE0, vendor uses mode 3) -- mode 3 is correct for AXS15231B
- Sending full register init (0xBB unlock, 0xA0-0xE5) via raw SPI -- not needed, ROM defaults work

## Critical Gotchas
- **SPI host enum**: ESP-IDF 5.x changed SPI3_HOST from 3 to 2. Using raw int `3` silently picks the wrong peripheral.
- **Power cycle vs soft reset**: The AXS15231B panel retains state across ESP32 soft resets (RTS). A bad init can persist until USB power is physically unplugged. Always power cycle between test iterations.
- **Byte swapping**: RGB565 pixels must be byte-swapped for SPI transport: `(c >> 8) | (c << 8)`
- **Backlight polarity**: GPIO 8 is active-LOW (duty 0 = full brightness)
- **vendor_specific_init_default**: Contains a full register config (~30 writes) but the vendor demo SKIPS it by passing custom init_cmds (just SleepOut+DispOn). The ROM defaults are sufficient.

## Reference Files
- Working vendor demo: `references/ESP32-S3-Touch-LCD-3.49/Examples/Arduino/10_LVGL_V9_Test/`
- Factory firmware binary: `references/ESP32-S3-Touch-LCD-3.49/Firmware/ESP32-S3-Touch-LCD-3.49-FactoryProgram.bin`
- Display HAL: `lib/display/` (display.h, display.cpp, drivers/, boards/)
