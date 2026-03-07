#pragma once
// Waveshare ESP32-S3-Touch-LCD-3.49
// 172x640, AXS15231B QSPI IPS LCD with integrated touch
// QMI8658 IMU, PCF85063 RTC, ES8311 + ES7210 audio, TCA9554PWR IO expander
// STATUS: Pin-verified from Waveshare GitHub repo (waveshareteam/ESP32-S3-Touch-LCD-3.49)

// ---- Component Presence ----
#define HAS_IMU             1
#define HAS_RTC             1
#define HAS_AUDIO_CODEC     1
#define HAS_PMIC            0
#define HAS_SDCARD          1
#define HAS_CAMERA          0
#define HAS_IO_EXPANDER     1

// ---- Display (AXS15231B via QSPI) ----
#define DISPLAY_DRIVER      "AXS15231B"
#define DISPLAY_IF          "QSPI"
#define DISPLAY_BPP         16
#define DISPLAY_WIDTH       172
#define DISPLAY_HEIGHT      640
#define DISPLAY_ROTATION    2   // 0=portrait, 1=landscape CW, 2=portrait 180, 3=landscape CCW

#define LCD_QSPI_CS         9
#define LCD_QSPI_CLK        10
#define LCD_QSPI_D0         11
#define LCD_QSPI_D1         12
#define LCD_QSPI_D2         13
#define LCD_QSPI_D3         14
#define LCD_SPI_HOST        3   // SPI3_HOST (SD card uses SPI2_HOST)
#define LCD_RST             21
#define LCD_BL              8   // PWM backlight

// ---- Touch (integrated in AXS15231B via I2C_NUM_1) ----
#define TOUCH_SDA           17
#define TOUCH_SCL           18
#define TOUCH_INT           (-1)
#define TOUCH_RST           (-1)
#define TOUCH_I2C_ADDR      0x3B
#define TOUCH_I2C_NUM       1   // Separate I2C bus

// ---- Sensor I2C Bus (I2C_NUM_0) ----
// IMU + RTC + IO Expander + Audio codecs share this bus
#define SENSOR_SDA          47
#define SENSOR_SCL          48

// ---- IMU (QMI8658 via sensor I2C) ----
#define IMU_SDA             47
#define IMU_SCL             48
#define IMU_I2C_ADDR        0x6B

// ---- RTC (PCF85063 via sensor I2C) ----
#define RTC_SDA             47
#define RTC_SCL             48
#define RTC_I2C_ADDR        0x51

// ---- Audio (ES8311 output + ES7210 input) ----
#define AUDIO_I2S_MCLK      7
#define AUDIO_I2S_BCLK      15
#define AUDIO_I2S_WS        46
#define AUDIO_I2S_DOUT      45
#define AUDIO_I2S_DIN       6
#define AUDIO_I2C_SDA       47
#define AUDIO_I2C_SCL       48
#define AUDIO_CODEC_ADDR    0x18  // ES8311_ADDRRES_0
// ES7210 also on same I2C bus

// ---- IO Expander (TCA9554) ----
#define IO_EXP_SDA          47
#define IO_EXP_SCL          48
#define IO_EXP_I2C_ADDR     0x20  // TCA9554_ADDRESS_000
// EXIO6 = battery power control
// EXIO7 = SD card CS

// ---- SD Card (SDMMC 1-bit) ----
#define SD_MMC_D0           40
#define SD_MMC_CMD          39
#define SD_MMC_CLK          41

// ---- Battery ----
#define BAT_STAT_PIN        16  // Battery charge status (gpio_get_level)
#define BAT_ADC_PIN         4   // ADC1_CH3 — battery voltage via 3x divider
#define BAT_ADC_DIVIDER     3.0f  // Voltage divider ratio (ref: Waveshare ADC example)
