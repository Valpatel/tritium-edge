# esp_lcd Panel Driver Research: LovyanGFX to esp_lcd Migration

## Executive Summary

All 6 boards have existing esp_lcd panel driver source code in the Waveshare reference code. The critical finding is that **Waveshare uses a single generic `esp_lcd_sh8601` driver for 3 different AMOLED ICs** (RM690B0, RM67162, SH8601Z), differentiating them only through board-specific init command arrays passed via `vendor_config`. For the AXS15231B LCD boards, Waveshare provides a dedicated `esp_lcd_axs15231b` driver. The 4.3C-BOX uses Espressif's built-in `esp_lcd_panel_rgb` with no custom panel driver needed.

**Bottom line: We need 3 driver files total (esp_lcd_sh8601, esp_lcd_axs15231b, plus RGB config), not 6.**

---

## Board 1: ESP32-S3-Touch-LCD-3.49 (AXS15231B, 172x640, QSPI)

### Driver Source Files
- **Panel driver**: `references/ESP32-S3-Touch-LCD-3.49/Examples/Arduino/09_LVGL_V8_Test/src/axs15231b/esp_lcd_axs15231b.c`
- **Panel header**: `references/ESP32-S3-Touch-LCD-3.49/Examples/Arduino/09_LVGL_V8_Test/src/axs15231b/esp_lcd_axs15231b.h`
- **Touch driver**: Built into same file (combined display+touch driver)
- **Touch data struct**: `references/ESP32-S3-Touch-LCD-3.49/Examples/Arduino/09_LVGL_V8_Test/src/touch/esp_lcd_touch.h`

### Transport Configuration (QSPI)
```c
// SPI bus config
AXS15231B_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz)

// Panel IO config (QSPI mode)
.cs_gpio_num = cs,
.dc_gpio_num = -1,          // No DC pin in QSPI mode
.spi_mode = 3,              // SPI MODE 3 (CPOL=1, CPHA=1)
.pclk_hz = 40 * 1000 * 1000, // 40 MHz pixel clock
.trans_queue_depth = 10,
.lcd_cmd_bits = 32,         // 32-bit commands in QSPI mode
.lcd_param_bits = 8,
.flags.quad_mode = true,
```

### QSPI Opcodes
```c
#define LCD_OPCODE_WRITE_CMD    (0x02ULL)
#define LCD_OPCODE_READ_CMD     (0x0BULL)
#define LCD_OPCODE_WRITE_COLOR  (0x32ULL)
```
Command encoding in QSPI mode: `(opcode << 24) | (cmd << 8)`

### Init Sequence (vendor_specific_init_default)
The init sequence is massive (~500 bytes across 32 commands). Key registers:
```c
{0xBB, {0x00,0x00,0x00,0x00,0x00,0x00,0x5A,0xA5}, 8, 0},  // Unlock vendor registers
{0xA0, {...}, 17, 0},   // Panel timing config
{0xA2, {...}, 31, 0},   // Source/gate driving
{0xD0, {...}, 30, 0},   // Power control
{0xA3, {...}, 22, 0},   // Panel config
{0xC1, {...}, 30, 0},   // Display timing
{0xC3..C9, ...},        // Various panel registers
{0xCF, {...}, 27, 0},   // Panel config
{0xD5..D9, ...},        // Gate driving sequences
{0xDF, {...}, 8, 0},    // Misc config
{0xE0..E5, ...},        // Gamma correction (6 tables, 17 bytes each)
{0xBB, {0x00,...,0x00}, 8, 0},  // Lock vendor registers
{0x13, {}, 0, 0},       // Normal display mode
{0x11, {}, 0, 200},     // Sleep out + 200ms delay
{0x29, {}, 0, 200},     // Display on + 200ms delay
{0x2C, {0x00,...}, 4, 0}, // Memory write start
{0x22, {}, 0, 200},     // All pixels off
```

### Reset Sequence
```c
gpio_set_level(rst, !reset_level);  // Assert reset
vTaskDelay(10ms);
gpio_set_level(rst, reset_level);   // De-assert
vTaskDelay(10ms);
gpio_set_level(rst, !reset_level);  // Assert again
vTaskDelay(120ms);                  // Wait for panel ready
```

### Pin Definitions (from user_config.h)
```c
#define EXAMPLE_PIN_NUM_LCD_CS     GPIO_NUM_9
#define EXAMPLE_PIN_NUM_LCD_PCLK   GPIO_NUM_10
#define EXAMPLE_PIN_NUM_LCD_DATA0  GPIO_NUM_11
#define EXAMPLE_PIN_NUM_LCD_DATA1  GPIO_NUM_12
#define EXAMPLE_PIN_NUM_LCD_DATA2  GPIO_NUM_13
#define EXAMPLE_PIN_NUM_LCD_DATA3  GPIO_NUM_14
#define EXAMPLE_PIN_NUM_LCD_RST    GPIO_NUM_21
#define EXAMPLE_PIN_NUM_BK_LIGHT   GPIO_NUM_8   // Active-high, PWM capable
```

### Backlight
- GPIO 8, active-high
- Uses LEDC PWM timer for brightness control

### Touch Driver
- **I2C address**: 0x3B
- **Handshake**: Writes 11-byte command `{0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, len_h, len_l, 0x00, 0x00, 0x00}` then reads touch data
- **Touch I2C**: Separate I2C port (I2C_NUM_1, SDA=17, SCL=18 on our board)
- **Max touch points**: 1
- **I2C IO config**:
  ```c
  .dev_addr = 0x3B,
  .control_phase_bytes = 1,
  .dc_bit_offset = 0,
  .lcd_cmd_bits = 8,
  .flags.disable_control_phase = 1,
  ```

### draw_bitmap Quirk
The AXS15231B QSPI driver has a special optimization: when `y_start == 0`, it sends `LCD_CMD_RAMWR (0x2C)`, otherwise it sends `LCD_CMD_RAMWRC (0x3C)` (Memory Write Continue). In QSPI mode, it skips the RASET command entirely, only sending CASET. This is a critical implementation detail for correct rendering.

---

## Board 2: ESP32-S3-Touch-LCD-3.5B-C (AXS15231B, 320x480, QSPI)

### Driver Source Files
**Same `esp_lcd_axs15231b` driver as the 3.49 board.** Found in:
- `references/WaveshareRtkServer/WSRtkServer/.pio/libdeps/Waveshare-S3-35/esp_lcd_touch_axs15231b/`

### Key Differences from 3.49
1. **Resolution**: 320x480 (vs 172x640)
2. **TCA9554 I/O Expander**: Display reset is routed through TCA9554 (I2C 0x20, pin 1). Must toggle before display init.
3. **Init sequence**: Needs a DIFFERENT `vendor_specific_init_default` array tuned for the 320x480 panel. The 3.49 sequence is for the 172x640 panel. The init sequences are NOT interchangeable.
4. **All I2C on port 0**: SDA=8, SCL=7 shared with touch (0x3B), PMIC (0x34), IMU (0x6B), RTC (0x51), audio codec (0x18)

### Transport Configuration
Identical QSPI config to the 3.49:
- `spi_mode = 3`, `pclk_hz = 40MHz`, `lcd_cmd_bits = 32`, `quad_mode = true`

### Pin Definitions (from our existing board header)
```c
// QSPI display
LCD_CS   = 45
LCD_CLK  = 47
LCD_D0   = 21
LCD_D1   = 48
LCD_D2   = 40
LCD_D3   = 39
LCD_BL   = 1   // Backlight, active-high
LCD_RST  = -1  // Reset via TCA9554, not direct GPIO
```

### Critical: TCA9554 Reset Procedure
```c
// Must be called BEFORE esp_lcd_panel_init()
// I2C address 0x20, register 0x03 (output), pin 1
tca9554_write(0x03, tca9554_read(0x03) & ~0x02);  // Pull pin 1 low (reset)
vTaskDelay(20ms);
tca9554_write(0x03, tca9554_read(0x03) | 0x02);   // Release pin 1 (run)
vTaskDelay(120ms);
```

### Touch
Same AXS15231B touch IC at I2C 0x3B, same handshake protocol as 3.49.

### Init Sequence
**NOT FOUND in references for the 3.5B-C specifically.** The 3.49 reference code's `vendor_specific_init_default` is for the 172x640 panel. For 320x480, the init sequence exists in our custom `lib/Panel_AXS15231B/Panel_AXS15231B.hpp` (converted from the original Waveshare Arduino_GFX demo). This must be converted to `axs15231b_lcd_init_cmd_t` format.

---

## Board 3: ESP32-S3-Touch-AMOLED-2.41-B (RM690B0, 450x600, QSPI)

### Driver Source Files
- **Panel driver**: `references/ESP32-S3-Touch-AMOLED-2.41-Demo/CN/Arduino/examples/09_LVGL_Test/esp_lcd_sh8601.c`
- **Panel header**: `references/ESP32-S3-Touch-AMOLED-2.41-Demo/CN/Arduino/examples/09_LVGL_Test/esp_lcd_sh8601.h`

**KEY FINDING**: Despite the panel IC being RM690B0, Waveshare uses the generic `esp_lcd_sh8601` driver with board-specific init commands. The SH8601 driver is a generic QSPI AMOLED driver that works with multiple panel ICs via the init_cmds override mechanism.

### Transport Configuration (QSPI)
```c
// SPI bus config
SH8601_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz)

// Panel IO config (QSPI mode)
.cs_gpio_num = cs,
.dc_gpio_num = -1,
.spi_mode = 0,              // SPI MODE 0 (CPOL=0, CPHA=0) -- DIFFERENT from AXS15231B!
.pclk_hz = 40 * 1000 * 1000,
.trans_queue_depth = 10,
.lcd_cmd_bits = 32,
.lcd_param_bits = 8,
.flags.quad_mode = true,
```

### QSPI Opcodes
```c
#define LCD_OPCODE_WRITE_CMD    (0x02ULL)
#define LCD_OPCODE_READ_CMD     (0x03ULL)  // Different from AXS15231B (0x0B)
#define LCD_OPCODE_WRITE_COLOR  (0x32ULL)
```

### Init Sequence (board-specific, from 09_LVGL_Test.ino)
```c
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, {0x20}, 1, 0},              // Enter page 0x20
    {0x26, {0x0A}, 1, 0},              // Page 0x20 config
    {0x24, {0x80}, 1, 0},              // Page 0x20 config
    {0xFE, {0x00}, 1, 0},              // Return to page 0x00
    {0x3A, {0x55}, 1, 0},              // Pixel format: RGB565
    {0xC2, {0x00}, 1, 10},             // Gate timing
    {0x35, {0x00}, 0, 0},              // Tearing effect off
    {0x51, {0x00}, 1, 10},             // Brightness = 0
    {0x11, {0x00}, 0, 80},             // Sleep out + 80ms delay
    {0x2A, {0x00,0x10,0x01,0xD1}, 4, 0}, // Column address: 16..465 (offset_x=16, width=450)
    {0x2B, {0x00,0x00,0x02,0x57}, 4, 0}, // Row address: 0..599
    {0x29, {0x00}, 0, 10},             // Display on
    {0x36, {0x30}, 1, 0},              // MADCTL: 90-degree rotation (if Rotate_90)
    {0x51, {0xFF}, 1, 0},              // Brightness = max
};
```

### Reset Sequence
```c
gpio_set_level(rst, reset_level);     // Assert reset (active-low default)
vTaskDelay(10ms);
gpio_set_level(rst, !reset_level);    // Release reset
vTaskDelay(150ms);                    // Wait for panel ready
```

### Pin Definitions (from 09_LVGL_Test.ino)
```c
#define EXAMPLE_PIN_NUM_LCD_CS     GPIO_NUM_9
#define EXAMPLE_PIN_NUM_LCD_PCLK   GPIO_NUM_10
#define EXAMPLE_PIN_NUM_LCD_DATA0  GPIO_NUM_11
#define EXAMPLE_PIN_NUM_LCD_DATA1  GPIO_NUM_12
#define EXAMPLE_PIN_NUM_LCD_DATA2  GPIO_NUM_13
#define EXAMPLE_PIN_NUM_LCD_DATA3  GPIO_NUM_14
#define EXAMPLE_PIN_NUM_LCD_RST    GPIO_NUM_21
#define EXAMPLE_PIN_NUM_BK_LIGHT   -1          // No backlight GPIO (AMOLED self-lit)
```

### Memory Offset
The RM690B0 has 452px memory width but 450px visible. Column address set starts at 0x10 (16):
- `{0x2A, {0x00,0x10,0x01,0xD1}, 4, 0}` = columns 16..465

### Touch Driver
- **IC**: FT5x06 (FocalTech)
- **I2C address**: 0x38 (standard FT5x06)
- **Driver**: `esp_lcd_touch_ft5x06` (Espressif component registry standard)
- **I2C pins**: SCL=GPIO_48, SDA=GPIO_47
- **Config**:
  ```c
  ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG()
  // Touch max: x=600-1, y=450-1 (when rotated 90)
  // flags: swap_xy=1, mirror_x=0, mirror_y=1
  ```

### Rounder Callback
Required for RM690B0 -- coordinates must be aligned to even boundaries:
```c
area->x1 = (x1 >> 1) << 1;
area->y1 = (y1 >> 1) << 1;
area->x2 = ((x2 >> 1) << 1) + 1;
area->y2 = ((y2 >> 1) << 1) + 1;
```

---

## Board 4: ESP32-S3-Touch-AMOLED-1.8 (SH8601Z, 368x448, QSPI)

### Driver Source Files
- **Panel driver**: `references/ESP32-S3-Touch-AMOLED-1.8/examples/ESP-IDF-v5.3.2/05_LVGL_WITH_RAM/components/esp_lcd_sh8601/esp_lcd_sh8601.c`
- **Panel header**: `references/ESP32-S3-Touch-AMOLED-1.8/examples/ESP-IDF-v5.3.2/05_LVGL_WITH_RAM/components/esp_lcd_sh8601/include/esp_lcd_sh8601.h`
- Also in Waveshare components: `references/Waveshare-ESP32-components/display/lcd/esp_lcd_sh8601/`

**Same generic `esp_lcd_sh8601` driver** as the 2.41-B and 1.91-M boards.

### Transport Configuration (QSPI)
Identical to 2.41-B:
- `spi_mode = 0`, `pclk_hz = 40MHz`, `lcd_cmd_bits = 32`, `quad_mode = true`

### Init Sequence (board-specific, from example_qspi_with_ram.c)
```c
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, {0x00}, 0, 120},                     // Sleep out + 120ms
    {0x44, {0x01, 0xD1}, 2, 0},                 // Set tear scan line
    {0x35, {0x00}, 1, 0},                        // Tearing effect on
    {0x53, {0x20}, 1, 10},                       // Write CTRL display
    {0x2A, {0x00,0x00,0x01,0x6F}, 4, 0},        // Column address: 0..367
    {0x2B, {0x00,0x00,0x01,0xBF}, 4, 0},        // Row address: 0..447
    {0x51, {0x00}, 1, 10},                       // Brightness = 0
    {0x29, {0x00}, 0, 10},                       // Display on
    {0x51, {0xFF}, 1, 0},                        // Brightness = max
};
```

### Pin Definitions (from pin_config.h and ESP-IDF example)
```c
// QSPI display (ESP-IDF example)
#define EXAMPLE_PIN_NUM_LCD_CS     GPIO_NUM_12
#define EXAMPLE_PIN_NUM_LCD_PCLK   GPIO_NUM_11
#define EXAMPLE_PIN_NUM_LCD_DATA0  GPIO_NUM_4
#define EXAMPLE_PIN_NUM_LCD_DATA1  GPIO_NUM_5
#define EXAMPLE_PIN_NUM_LCD_DATA2  GPIO_NUM_6
#define EXAMPLE_PIN_NUM_LCD_DATA3  GPIO_NUM_7
#define EXAMPLE_PIN_NUM_LCD_RST    -1       // Reset via TCA9554 IO expander
#define EXAMPLE_PIN_NUM_BK_LIGHT   -1       // No backlight GPIO (AMOLED)

// Touch (FT5x06 / FT3168)
#define EXAMPLE_PIN_NUM_TOUCH_SCL  GPIO_NUM_14
#define EXAMPLE_PIN_NUM_TOUCH_SDA  GPIO_NUM_15
#define EXAMPLE_PIN_NUM_TOUCH_INT  GPIO_NUM_21
```

### IO Expander (TCA9554 / XCA9554)
- The 1.8 board uses a TCA9554/XCA9554 I/O expander for display reset
- Library: `Adafruit_XCA9554` (Arduino) or `esp_io_expander_tca9554` (ESP-IDF)
- Display reset pin routed through the expander

### Touch Driver
- **IC**: FT5x06 / FT3168 (FocalTech family)
- **Driver**: `esp_lcd_touch_ft5x06` (from Espressif component registry)
- **I2C pins**: SCL=14, SDA=15

### Reset Sequence
Software-only (RST pin = -1, routed through TCA9554):
```c
ESP_RETURN_ON_ERROR(tx_param(sh8601, io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
vTaskDelay(80ms);
```

---

## Board 5: ESP32-S3-AMOLED-1.91-M (RM67162, 240x536, QSPI)

### Driver Source Files
- **Panel driver**: `references/ESP32-S3-AMOLED-1.91-Demo/ESP32-S3-AMOLED-1.91-Demo/ESP-IDF/FactoryProgram/components/esp_lcd_sh8601/esp_lcd_sh8601.c`
- **Panel header**: `references/ESP32-S3-AMOLED-1.91-Demo/ESP32-S3-AMOLED-1.91-Demo/ESP-IDF/FactoryProgram/components/esp_lcd_sh8601/include/esp_lcd_sh8601.h`
- **Arduino LVGL example**: `references/ESP32-S3-AMOLED-1.91-Demo/ESP32-S3-AMOLED-1.91-Demo/Arduino/examples/LVGL/`
- **Low-level driver**: `references/ESP32-S3-AMOLED-1.91-Arduino_Playablity/.../rm67162.cpp` (raw SPI driver, not esp_lcd)

**Same generic `esp_lcd_sh8601` driver**, differentiated by init commands.

### Transport Configuration (QSPI)
Identical to 2.41-B and 1.8:
- `spi_mode = 0`, `pclk_hz = 40MHz`, `lcd_cmd_bits = 32`, `quad_mode = true`

### Init Sequence (board-specific, from LVGL.ino)
```c
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, {0x00}, 0, 120},                      // Sleep out + 120ms
    {0x36, {0xF0}, 1, 0},                        // MADCTL: MY|MX|MV|ML (landscape + flip)
    {0x3A, {0x55}, 1, 0},                        // Pixel format: RGB565
    {0x2A, {0x00,0x00,0x02,0x17}, 4, 0},         // Column address: 0..535
    {0x2B, {0x00,0x00,0x00,0xEF}, 4, 0},         // Row address: 0..239
    {0x51, {0x00}, 1, 10},                        // Brightness = 0
    {0x29, {0x00}, 0, 10},                        // Display on
    {0x51, {0xFF}, 1, 0},                         // Brightness = max
};
```

### Low-Level RM67162 QSPI Init (from rm67162.cpp, alternative reference)
```c
const static lcd_cmd_t rm67162_qspi_init[] = {
    {0x11, {0x00}, 0x80},       // Sleep Out + 120ms delay (0x80 flag)
    {0x3A, {0x55}, 0x01},       // Pixel format: RGB565
    {0x51, {0x00}, 0x01},       // Brightness = 0
    {0x29, {0x00}, 0x80},       // Display on + 120ms delay
    {0x51, {0xD0}, 0x01},       // Brightness = 0xD0
};
```
Note: The rm67162.cpp uses raw SPI with `command_bits=8, address_bits=24, mode=SPI_MODE0` and `SPI_DEVICE_HALFDUPLEX`. The QSPI color write uses opcode `0x32` with address `0x002C00`.

### Pin Definitions (from LVGL.ino)
```c
#define EXAMPLE_PIN_NUM_LCD_CS     GPIO_NUM_6
#define EXAMPLE_PIN_NUM_LCD_PCLK   GPIO_NUM_47
#define EXAMPLE_PIN_NUM_LCD_DATA0  GPIO_NUM_18
#define EXAMPLE_PIN_NUM_LCD_DATA1  GPIO_NUM_7
#define EXAMPLE_PIN_NUM_LCD_DATA2  GPIO_NUM_48
#define EXAMPLE_PIN_NUM_LCD_DATA3  GPIO_NUM_5
#define EXAMPLE_PIN_NUM_LCD_RST    GPIO_NUM_17
#define EXAMPLE_PIN_NUM_BK_LIGHT   -1          // No backlight GPIO (AMOLED)
```

### RM67162 SPI Frequency
From pins_config.h: `SPI_FREQUENCY = 75000000` (75 MHz) for the raw driver. The esp_lcd config uses 40 MHz.

### Touch
- **No touch on the 1.91-M board** (it's labeled "M" = module, no touch panel)
- The LVGL.ino uses a custom `touch_bsp.h` with `Touch_Init()` and `getTouch()` -- likely capacitive button or external touch

### Reset Sequence
```c
// Hardware reset
TFT_RES_L;          // GPIO 17 low
delay(300);
TFT_RES_H;          // GPIO 17 high
delay(200);
```

---

## Board 6: ESP32-S3-Touch-LCD-4.3C-BOX (ST7262, 800x480, RGB Parallel)

### Driver Source Files
- **Panel config**: `references/ESP32-S3-Touch-LCD-4.3C/examples/esp-idf/03_lcd/components/rgb_lcd_port/rgb_lcd_port.c`
- **Panel header**: `references/ESP32-S3-Touch-LCD-4.3C/examples/esp-idf/03_lcd/components/rgb_lcd_port/rgb_lcd_port.h`
- **Touch driver**: `references/ESP32-S3-Touch-LCD-4.3C/examples/esp-idf/06_touch/components/touch/gt911.c`
- **IO expander**: `references/ESP32-S3-Touch-LCD-4.3C/examples/esp-idf/06_touch/components/io_extension/io_extension.c`

**No custom panel driver needed.** Uses Espressif's built-in `esp_lcd_new_rgb_panel()`.

### RGB Panel Configuration
```c
esp_lcd_rgb_panel_config_t panel_config = {
    .clk_src = LCD_CLK_SRC_DEFAULT,
    .timings = {
        .pclk_hz = 16 * 1000 * 1000,    // 16 MHz pixel clock
        .h_res = 800,
        .v_res = 480,
        .hsync_pulse_width = 8,
        .hsync_back_porch = 8,
        .hsync_front_porch = 4,
        .vsync_pulse_width = 8,
        .vsync_back_porch = 8,
        .vsync_front_porch = 4,
        .flags.pclk_active_neg = 1,      // Pixel clock active on falling edge
    },
    .data_width = 16,                     // 16-bit RGB565 parallel
    .bits_per_pixel = 16,
    .num_fbs = 2,                         // Double buffering
    .bounce_buffer_size_px = 800 * 10,    // 10 rows bounce buffer
    .sram_trans_align = 4,
    .psram_trans_align = 64,
    .hsync_gpio_num = GPIO_NUM_46,
    .vsync_gpio_num = GPIO_NUM_3,
    .de_gpio_num = GPIO_NUM_5,
    .pclk_gpio_num = GPIO_NUM_7,
    .disp_gpio_num = -1,
    .data_gpio_nums = {
        GPIO_NUM_14,  // B3
        GPIO_NUM_38,  // B4
        GPIO_NUM_18,  // B5
        GPIO_NUM_17,  // B6
        GPIO_NUM_10,  // B7
        GPIO_NUM_39,  // G2
        GPIO_NUM_0,   // G3
        GPIO_NUM_45,  // G4
        GPIO_NUM_48,  // G5
        GPIO_NUM_47,  // G6
        GPIO_NUM_21,  // G7
        GPIO_NUM_1,   // R3
        GPIO_NUM_2,   // R4
        GPIO_NUM_42,  // R5
        GPIO_NUM_41,  // R6
        GPIO_NUM_40,  // R7
    },
    .flags.fb_in_psram = 1,              // Framebuffer in PSRAM
};
```

### No Init Sequence Required
The ST7262 is a dumb RGB panel -- no command interface. All configuration is done through timing parameters.

### Reset/Backlight
- **Reset**: Not needed (`EXAMPLE_LCD_IO_RST = -1`)
- **Backlight**: Controlled via IO extension chip (I2C), NOT direct GPIO
  - `EXAMPLE_PIN_NUM_BK_LIGHT = -1`
  - Backlight on: `IO_EXTENSION_Output(IO_EXTENSION_IO_2, 1)`
  - Backlight off: `IO_EXTENSION_Output(IO_EXTENSION_IO_2, 0)`
  - PWM brightness: `IO_EXTENSION_Pwm_Output(value)` (0-97 range, maps to 0-255)

### IO Extension Chip
- **Custom Waveshare chip** at I2C address 0x24 (NOT CH422G, NOT TCA9554)
- Controls: backlight (IO2), touch reset (IO1), PA amplifier (IO3), SD CS (IO4), USB/CAN select (IO5)
- Mode register: 0x02, Output register: 0x03, Input register: 0x04, PWM register: 0x05, ADC register: 0x06

### Touch Driver
- **IC**: GT911 (Goodix)
- **I2C address**: 0x5D (default) or 0x14 (backup)
- **Touch interrupt**: GPIO_NUM_4
- **Reset**: Via IO extension (IO_EXTENSION_IO_1)
- **Custom driver** (NOT using esp_lcd_touch_gt911 from component registry)
- **I2C command bits**: 16 (GT911 uses 16-bit register addresses)

---

## Summary: Driver Architecture

### Shared esp_lcd_sh8601 Driver (3 AMOLED boards)
The `esp_lcd_sh8601` driver is generic and works with multiple AMOLED ICs:

| Board | Panel IC | Init via | SPI Mode | Notes |
|---|---|---|---|---|
| 2.41-B | RM690B0 | vendor_config init_cmds | 0 | offset_x=16, rounder needed |
| 1.8 | SH8601Z | vendor_config init_cmds | 0 | TCA9554 reset |
| 1.91-M | RM67162 | vendor_config init_cmds | 0 | No touch, 75MHz capable |

All three use:
- QSPI mode with `lcd_cmd_bits=32`, `lcd_param_bits=8`, `quad_mode=true`
- SPI MODE 0
- 40 MHz pixel clock (in esp_lcd config)
- Write CMD opcode: 0x02, Write COLOR opcode: 0x32

### esp_lcd_axs15231b Driver (2 LCD boards)
| Board | Resolution | Init Sequence | Notes |
|---|---|---|---|
| 3.49 | 172x640 | vendor_specific_init_default (32 commands, ~500 bytes) | Direct GPIO reset |
| 3.5B-C | 320x480 | DIFFERENT init sequence needed | TCA9554 reset |

Both use:
- QSPI mode with `lcd_cmd_bits=32`, `lcd_param_bits=8`, `quad_mode=true`
- **SPI MODE 3** (different from AMOLED boards!)
- 40 MHz pixel clock
- Write CMD opcode: 0x02, Write COLOR opcode: 0x32
- Special `draw_bitmap`: skips RASET in QSPI mode, uses RAMWR/RAMWRC based on y_start

### esp_lcd_panel_rgb (1 RGB board)
| Board | Panel IC | Notes |
|---|---|---|
| 4.3C-BOX | ST7262 | No command interface, config via timing params only |

---

## Espressif Component Registry Status

### Available from Espressif
- `esp_lcd_touch_ft5x06` -- Used by 2.41-B and 1.8 boards (FT5x06 family touch)
- `esp_lcd_touch_gt911` -- Could be used for 4.3C-BOX (but Waveshare uses custom driver)
- `esp_lcd_panel_rgb` -- Built into ESP-IDF, used by 4.3C-BOX

### NOT Available from Espressif (must use Waveshare code)
- `esp_lcd_sh8601` -- Waveshare's generic QSPI AMOLED driver. NOT in Espressif registry. Must be included as local component.
- `esp_lcd_axs15231b` -- Waveshare's AXS15231B driver. NOT in Espressif registry. Must be included as local component.
- AXS15231B touch driver -- Custom, combined with panel driver in Waveshare code
- GT911 touch driver -- Waveshare has custom implementation; Espressif has `esp_lcd_touch_gt911` in registry that could potentially substitute
- IO extension driver -- Waveshare proprietary for 4.3C-BOX

### No idf_component.yml Files Found
No `idf_component.yml` files were found in the reference directories, confirming these are standalone components, not published to the Espressif component registry.

---

## Migration Plan: Files to Create/Port

### Must port from reference code (3 files):
1. **`esp_lcd_sh8601.c` + `esp_lcd_sh8601.h`** -- Generic QSPI AMOLED driver. Copy from `references/Waveshare-ESP32-components/display/lcd/esp_lcd_sh8601/`. Handles RM690B0, RM67162, SH8601Z via init_cmds override.
2. **`esp_lcd_axs15231b.c` + `esp_lcd_axs15231b.h`** -- AXS15231B QSPI LCD driver. Copy from `references/ESP32-S3-Touch-LCD-3.49/Examples/Arduino/09_LVGL_V8_Test/src/axs15231b/`.

### Must create (board-specific init arrays):
3. **Board init command arrays** -- One `static const xxx_lcd_init_cmd_t[]` per board, stored in board config headers:
   - 3.49: Use `vendor_specific_init_default` from the axs15231b driver (already embedded)
   - 3.5B-C: Extract from `lib/Panel_AXS15231B/Panel_AXS15231B.hpp` and convert to `axs15231b_lcd_init_cmd_t` format
   - 2.41-B: Copy from `09_LVGL_Test.ino` (13 commands)
   - 1.8: Copy from `example_qspi_with_ram.c` (9 commands)
   - 1.91-M: Copy from `LVGL.ino` (8 commands)

### Must port (touch drivers):
4. **AXS15231B touch** -- Extract from `esp_lcd_axs15231b.c` (already combined in the driver file)
5. **FT5x06 touch** -- Use `esp_lcd_touch_ft5x06` from Espressif component registry
6. **GT911 touch** -- Use `esp_lcd_touch_gt911` from Espressif component registry, or port Waveshare's custom version

### Must port (IO expanders):
7. **TCA9554 reset** -- For 3.5B-C and 1.8 boards
8. **IO extension** -- For 4.3C-BOX (I2C addr 0x24, custom Waveshare chip)

### No driver needed:
9. **4.3C-BOX display** -- Uses `esp_lcd_new_rgb_panel()` built into ESP-IDF
