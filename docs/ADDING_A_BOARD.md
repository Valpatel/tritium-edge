# Adding a New Board

This guide covers adding support for a new ESP32-S3 display board.

## Prerequisites

Before starting, gather:
- The board's schematic or pin table (usually on the manufacturer's wiki)
- Display panel IC model (e.g., RM690B0, ST7789, ILI9341)
- Display bus type (SPI, QSPI, RGB parallel, I2C)
- Display resolution
- Touch controller IC and I2C address
- Any other peripherals (IMU, RTC, PMIC, audio codec)

## Steps

### 1. Create Pin Definitions (`include/boards/esp32_s3_<name>.h`)

Create a header with `#define` constants for every GPIO:

```cpp
#pragma once
// Board: Your-Board-Name
// Resolution, Panel IC, Bus type, Touch IC
// STATUS: Pins need verification

// Display bus
#define LCD_QSPI_CS    9
#define LCD_QSPI_CLK   10
#define LCD_QSPI_D0    11
#define LCD_QSPI_D1    12
#define LCD_QSPI_D2    13
#define LCD_QSPI_D3    14
#define LCD_RST        21

// Touch (I2C)
#define TOUCH_SDA      47
#define TOUCH_SCL      48

// Display metadata
#define DISPLAY_DRIVER  "YourPanelIC"
#define DISPLAY_IF      "QSPI"
#define DISPLAY_BPP     16
```

For RGB parallel boards, define `LCD_PCLK`, `LCD_HSYNC`, `LCD_VSYNC`, `LCD_DE`, and `LCD_R*/G*/B*` data pins instead. See `esp32_s3_touch_lcd_43c_box.h` for an example.

### 2. Add LGFX Class (`include/display_init.h`)

Add an `#elif` block for your board before the `#else` error:

```cpp
#elif defined(BOARD_YOUR_BOARD)
#include "boards/esp32_s3_your_board.h"

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus;       // or Bus_RGB for parallel
    lgfx::Panel_YourIC _panel;

public:
    LGFX() {
        // Configure bus
        {
            auto cfg = _bus.config();
            cfg.freq_write  = 75000000;
            cfg.pin_sclk    = LCD_QSPI_CLK;
            cfg.pin_io0     = LCD_QSPI_D0;
            cfg.pin_io1     = LCD_QSPI_D1;
            cfg.pin_io2     = LCD_QSPI_D2;
            cfg.pin_io3     = LCD_QSPI_D3;
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = SPI_MODE0;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            _bus.config(cfg);
        }
        // Configure panel
        {
            auto cfg = _panel.config();
            cfg.pin_cs       = LCD_QSPI_CS;
            cfg.pin_rst      = LCD_RST;
            cfg.panel_width  = YOUR_WIDTH;
            cfg.panel_height = YOUR_HEIGHT;
            cfg.bus_shared   = false;
            _panel.config(cfg);
        }
        _panel.setBus(&_bus);
        setPanel(&_panel);
    }
};
```

Check [LovyanGFX Panel list](https://github.com/lovyan03/LovyanGFX) for available panel drivers. If your panel IC is not supported, you may need to use a placeholder (like ST7789) or implement a custom panel driver.

### 3. Add PlatformIO Environment (`platformio.ini`)

Add a new `[env:<name>]` section:

```ini
[env:your-board]
board = esp32-s3-devkitc-1
board_build.arduino.memory_type = qio_opi
board_build.partitions = default_16MB.csv
board_upload.flash_size = 16MB
build_flags =
    ${env.build_flags}
    -DBOARD_YOUR_BOARD
    -DDISPLAY_WIDTH=XXX
    -DDISPLAY_HEIGHT=YYY
```

### 4. Update Makefile

Add the new environment name to the `BOARDS` list in the Makefile:

```makefile
BOARDS := touch-amoled-241b amoled-191m ... your-board
```

Add an entry to the `list-boards` target.

### 5. Update docs/boards.md

Add the board to the summary table and create a per-board details section.

### 6. Build and Verify

```bash
make build BOARD=your-board
```

If you have the physical board, flash and verify the display initializes:

```bash
make flash-monitor BOARD=your-board
```

Look for serial output confirming the display driver and resolution.

## Pin Verification Checklist

When a new board arrives, verify pin assignments by:

1. Check the manufacturer's wiki/schematic for the latest pin table
2. Run the manufacturer's demo code to confirm the display works
3. Update the board header with verified pins
4. Update the "Pins Verified" column in `docs/boards.md`

## Notes

- **QSPI vs SPI**: LovyanGFX handles QSPI by setting `pin_io0` through `pin_io3` on `Bus_SPI`. The same bus class works for both standard SPI and QSPI.
- **RGB parallel**: Uses `Bus_RGB` + `Panel_RGB` with explicit timing parameters (hsync/vsync porches, pclk frequency). See the 4.3C-BOX config for a working example.
- **Memory offsets**: Some panels have a larger memory buffer than the visible area (e.g., RM690B0 is 452px wide but displays 450px). Set `memory_width`, `memory_height`, `offset_x`, `offset_y` accordingly.
- **Unsupported panels**: If LovyanGFX doesn't have a driver for your panel IC, the board will compile but the display won't work. File an issue or implement a custom `Panel_*` class.
