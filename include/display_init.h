#pragma once
// Display initialization for each supported board using LovyanGFX.
// QSPI panels use Bus_SPI with pin_io0-3 set (LovyanGFX's QSPI mechanism).

#define LGFX_USE_V1
#define LGFX_USE_QSPI
#include <LovyanGFX.hpp>

// ============================================================================
// Board: ESP32-S3-Touch-AMOLED-2.41-B (RM690B0, QSPI, 450x600 native)
// Waveshare wiki-verified pins
// ============================================================================
#if defined(BOARD_TOUCH_AMOLED_241B)
#include "boards/esp32_s3_touch_amoled_241b.h"

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus;
    lgfx::Panel_RM690B0 _panel;
    lgfx::Touch_FT5x06 _touch;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.freq_write  = 75000000;
            cfg.freq_read   = 20000000;
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
        {
            auto cfg = _panel.config();
            cfg.pin_cs       = LCD_QSPI_CS;
            cfg.pin_rst      = LCD_RST;
            cfg.pin_busy     = -1;
            // RM690B0 native: 452x600, display area 450x600 with 16px x-offset
            cfg.panel_width  = DISPLAY_WIDTH;
            cfg.panel_height = DISPLAY_HEIGHT;
            cfg.memory_width = 452;
            cfg.memory_height = DISPLAY_HEIGHT;
            cfg.offset_x     = 16;
            cfg.offset_y     = 0;
            cfg.bus_shared   = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _touch.config();
            cfg.i2c_port = 0;
            cfg.pin_sda  = TOUCH_SDA;
            cfg.pin_scl  = TOUCH_SCL;
            cfg.freq     = 400000;
            cfg.x_min    = 0;
            cfg.x_max    = DISPLAY_WIDTH - 1;
            cfg.y_min    = 0;
            cfg.y_max    = DISPLAY_HEIGHT - 1;
            _touch.config(cfg);
        }
        _panel.setTouch(&_touch);
        _panel.setBus(&_bus);
        setPanel(&_panel);
    }
};

// ============================================================================
// Board: ESP32-S3-AMOLED-1.91-M (RM67162, QSPI, 240x536)
// Pins based on Waveshare wiki - needs verification
// ============================================================================
#elif defined(BOARD_AMOLED_191M)
#include "boards/esp32_s3_amoled_191m.h"

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus;
    lgfx::Panel_RM67162 _panel;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.freq_write  = 75000000;
            cfg.freq_read   = 20000000;
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
        {
            auto cfg = _panel.config();
            cfg.pin_cs       = LCD_QSPI_CS;
            cfg.pin_rst      = LCD_RST;
            cfg.pin_busy     = -1;
            cfg.panel_width  = DISPLAY_WIDTH;
            cfg.panel_height = DISPLAY_HEIGHT;
            cfg.readable     = true;
            cfg.bus_shared   = false;
            _panel.config(cfg);
        }
        _panel.setBus(&_bus);
        setPanel(&_panel);
    }
};

// ============================================================================
// Board: ESP32-S3-Touch-AMOLED-1.8 (SH8601Z, QSPI, 368x448)
// Pins need verification on actual hardware
// ============================================================================
#elif defined(BOARD_TOUCH_AMOLED_18)
#include "boards/esp32_s3_touch_amoled_18.h"

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus;
    lgfx::Panel_SH8601Z _panel;
    lgfx::Touch_FT5x06 _touch;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.freq_write  = 75000000;
            cfg.freq_read   = 20000000;
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
        {
            auto cfg = _panel.config();
            cfg.pin_cs       = LCD_QSPI_CS;
            cfg.pin_rst      = LCD_RST;
            cfg.pin_busy     = -1;
            cfg.panel_width  = DISPLAY_WIDTH;
            cfg.panel_height = DISPLAY_HEIGHT;
            cfg.bus_shared   = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _touch.config();
            cfg.i2c_port = 0;
            cfg.pin_sda  = TOUCH_SDA;
            cfg.pin_scl  = TOUCH_SCL;
            cfg.pin_int  = TOUCH_INT;
            cfg.i2c_addr = TOUCH_I2C_ADDR;
            cfg.freq     = 400000;
            cfg.x_min    = 0;
            cfg.x_max    = DISPLAY_WIDTH - 1;
            cfg.y_min    = 0;
            cfg.y_max    = DISPLAY_HEIGHT - 1;
            cfg.bus_shared = false;
            _touch.config(cfg);
        }
        _panel.setTouch(&_touch);
        _panel.setBus(&_bus);
        setPanel(&_panel);
    }
};

// ============================================================================
// Board: ESP32-S3-Touch-LCD-3.5B-C (AXS15231B, QSPI, 320x480)
// The AXS15231B reset is controlled by TCA9554 I/O expander (0x20) pin 1.
// Must toggle reset via I2C before display.init() will work.
// ============================================================================
#elif defined(BOARD_TOUCH_LCD_35BC)
#include "boards/esp32_s3_touch_lcd_35bc.h"
#include <Panel_AXS15231B.hpp>
#include <Touch_AXS15231B.hpp>
#include <Wire.h>

// TCA9554 I/O expander for display reset (address 0x20 on shared I2C bus)
static void tca9554_reset_display() {
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    // Set TCA9554 pin 1 as output (config register 0x03, clear bit 1)
    Wire.beginTransmission(0x20);
    Wire.write(0x03);  // Configuration register
    Wire.write(0xFD);  // Pin 1 = output (bit 1 = 0), rest inputs
    Wire.endTransmission();
    // Pin 1 HIGH
    Wire.beginTransmission(0x20);
    Wire.write(0x01);  // Output register
    Wire.write(0x02);  // Pin 1 = HIGH
    Wire.endTransmission();
    delay(10);
    // Pin 1 LOW (active reset)
    Wire.beginTransmission(0x20);
    Wire.write(0x01);
    Wire.write(0x00);  // Pin 1 = LOW
    Wire.endTransmission();
    delay(10);
    // Pin 1 HIGH (release reset)
    Wire.beginTransmission(0x20);
    Wire.write(0x01);
    Wire.write(0x02);  // Pin 1 = HIGH
    Wire.endTransmission();
    delay(200);
    Wire.end();  // Release I2C so LovyanGFX can manage it
}

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus;
    lgfx::Panel_AXS15231B _panel;
    lgfx::Light_PWM _backlight;
    lgfx::Touch_AXS15231B _touch;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 20000000;
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
        {
            _panel.setInitMode(lgfx::Panel_AXS15231B::INIT_320x480);
            auto cfg = _panel.config();
            cfg.pin_cs       = LCD_QSPI_CS;
            cfg.pin_rst      = -1;
            cfg.pin_busy     = -1;
            cfg.panel_width  = DISPLAY_WIDTH;
            cfg.panel_height = DISPLAY_HEIGHT;
            cfg.memory_width = 360;
            cfg.memory_height = 640;
            cfg.bus_shared   = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _backlight.config();
            cfg.pin_bl = LCD_BL;
            cfg.invert = false;
            cfg.freq   = 44100;
            cfg.pwm_channel = 1;
            _backlight.config(cfg);
            _panel.setLight(&_backlight);
        }
        {
            auto cfg = _touch.config();
            cfg.i2c_port = TOUCH_I2C_NUM;
            cfg.pin_sda  = TOUCH_SDA;
            cfg.pin_scl  = TOUCH_SCL;
            cfg.i2c_addr = TOUCH_I2C_ADDR;
            cfg.freq     = 400000;
            cfg.x_min    = 0;
            cfg.x_max    = DISPLAY_WIDTH - 1;
            cfg.y_min    = 0;
            cfg.y_max    = DISPLAY_HEIGHT - 1;
            cfg.bus_shared = false;
            _touch.config(cfg);
        }
        _panel.setTouch(&_touch);
        _panel.setBus(&_bus);
        setPanel(&_panel);
    }
};

// ============================================================================
// Board: ESP32-S3-Touch-LCD-4.3C-BOX (ST7262, RGB parallel, 800x480)
// Waveshare wiki-verified pins
// ============================================================================
#elif defined(BOARD_TOUCH_LCD_43C_BOX)
#include "boards/esp32_s3_touch_lcd_43c_box.h"
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_RGB _bus;
    lgfx::Panel_RGB _panel;

public:
    LGFX() {
        {
            auto cfg = _panel.config();
            cfg.memory_width  = DISPLAY_WIDTH;
            cfg.memory_height = DISPLAY_HEIGHT;
            cfg.panel_width   = DISPLAY_WIDTH;
            cfg.panel_height  = DISPLAY_HEIGHT;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            _panel.config(cfg);
        }
        {
            auto cfg = _panel.config_detail();
            cfg.use_psram = 1;
            _panel.config_detail(cfg);
        }
        {
            auto cfg = _bus.config();
            cfg.panel       = &_panel;
            cfg.freq_write  = 14000000;
            cfg.pin_pclk    = LCD_PCLK;
            cfg.pin_hsync   = LCD_HSYNC;
            cfg.pin_vsync   = LCD_VSYNC;
            cfg.pin_henable = LCD_DE;

            cfg.pin_d0  = LCD_B3;
            cfg.pin_d1  = LCD_B4;
            cfg.pin_d2  = LCD_B5;
            cfg.pin_d3  = LCD_B6;
            cfg.pin_d4  = LCD_B7;
            cfg.pin_d5  = LCD_G2;
            cfg.pin_d6  = LCD_G3;
            cfg.pin_d7  = LCD_G4;
            cfg.pin_d8  = LCD_G5;
            cfg.pin_d9  = LCD_G6;
            cfg.pin_d10 = LCD_G7;
            cfg.pin_d11 = LCD_R3;
            cfg.pin_d12 = LCD_R4;
            cfg.pin_d13 = LCD_R5;
            cfg.pin_d14 = LCD_R6;
            cfg.pin_d15 = LCD_R7;

            cfg.hsync_polarity    = 0;
            cfg.hsync_front_porch = 8;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch  = 16;
            cfg.vsync_polarity    = 0;
            cfg.vsync_front_porch = 4;
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch  = 4;
            cfg.pclk_idle_high    = 1;

            _bus.config(cfg);
        }
        _panel.setBus(&_bus);
        setPanel(&_panel);
    }
};

// ============================================================================
// Board: ESP32-S3-Touch-LCD-3.49 (AXS15231B, QSPI, 172x640)
// ============================================================================
#elif defined(BOARD_TOUCH_LCD_349)
#include "boards/esp32_s3_touch_lcd_349.h"
#include <Panel_AXS15231B.hpp>
#include <Touch_AXS15231B.hpp>

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus;
    lgfx::Panel_AXS15231B _panel;
    lgfx::Light_PWM _backlight;
    lgfx::Touch_AXS15231B _touch;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 20000000;
            cfg.pin_sclk    = LCD_QSPI_CLK;
            cfg.pin_io0     = LCD_QSPI_D0;
            cfg.pin_io1     = LCD_QSPI_D1;
            cfg.pin_io2     = LCD_QSPI_D2;
            cfg.pin_io3     = LCD_QSPI_D3;
            cfg.spi_host    = SPI3_HOST;
            cfg.spi_mode    = SPI_MODE0;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            _bus.config(cfg);
        }
        {
            _panel.setInitMode(lgfx::Panel_AXS15231B::INIT_172x640);
            auto cfg = _panel.config();
            cfg.pin_cs       = LCD_QSPI_CS;
            cfg.pin_rst      = LCD_RST;
            cfg.pin_busy     = -1;
            cfg.panel_width  = DISPLAY_WIDTH;
            cfg.panel_height = DISPLAY_HEIGHT;
            cfg.memory_width = 360;
            cfg.memory_height = DISPLAY_HEIGHT;
            cfg.bus_shared   = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _backlight.config();
            cfg.pin_bl = LCD_BL;
            cfg.invert = false;
            cfg.freq   = 44100;
            cfg.pwm_channel = 0;
            _backlight.config(cfg);
            _panel.setLight(&_backlight);
        }
        {
            auto cfg = _touch.config();
            cfg.i2c_port = TOUCH_I2C_NUM;
            cfg.pin_sda  = TOUCH_SDA;
            cfg.pin_scl  = TOUCH_SCL;
            cfg.i2c_addr = TOUCH_I2C_ADDR;
            cfg.freq     = 400000;
            cfg.x_min    = 0;
            cfg.x_max    = DISPLAY_WIDTH - 1;
            cfg.y_min    = 0;
            cfg.y_max    = DISPLAY_HEIGHT - 1;
            cfg.bus_shared = false;
            _touch.config(cfg);
        }
        _panel.setTouch(&_touch);
        _panel.setBus(&_bus);
        setPanel(&_panel);
    }
};

#else
#error "No board selected! Define one of: BOARD_TOUCH_AMOLED_241B, BOARD_AMOLED_191M, BOARD_TOUCH_AMOLED_18, BOARD_TOUCH_LCD_35BC, BOARD_TOUCH_LCD_43C_BOX, BOARD_TOUCH_LCD_349"
#endif
