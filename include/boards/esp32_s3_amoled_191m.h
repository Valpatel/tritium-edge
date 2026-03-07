#pragma once
// Waveshare ESP32-S3-AMOLED-1.91-M
// 536x240, RM67162 QSPI AMOLED, FT3168 capacitive touch
// QMI8658 IMU
// STATUS: Pin-verified from Waveshare demo code (ESP32-S3-AMOLED-1.91-Demo)

// ---- Component Presence ----
#define HAS_IMU             1
#define HAS_RTC             0
#define HAS_AUDIO_CODEC     0
#define HAS_PMIC            0
#define HAS_SDCARD          1
#define HAS_CAMERA          0
#define HAS_IO_EXPANDER     0

// ---- Display (RM67162 via QSPI) ----
#define DISPLAY_DRIVER      "RM67162"
#define DISPLAY_IF          "QSPI"
#define DISPLAY_BPP         16
#define DISPLAY_WIDTH       240
#define DISPLAY_HEIGHT      536
#define DISPLAY_ROTATION    0   // 0=portrait, 1=landscape CW, 2=portrait 180, 3=landscape CCW

#define LCD_QSPI_CS         6
#define LCD_QSPI_CLK        47
#define LCD_QSPI_D0         18
#define LCD_QSPI_D1         7
#define LCD_QSPI_D2         48
#define LCD_QSPI_D3         5
#define LCD_RST             17
#define LCD_BL              (-1)  // No dedicated backlight pin (AMOLED)

// ---- Touch (FT3168 via I2C) ----
#define TOUCH_SDA           40
#define TOUCH_SCL           39
#define TOUCH_INT           (-1)
#define TOUCH_RST           (-1)
#define TOUCH_I2C_ADDR      0x38

// ---- IMU (QMI8658 via shared I2C) ----
#define IMU_SDA             40
#define IMU_SCL             39
#define IMU_I2C_ADDR        0x6B

// ---- SD Card (SDMMC 1-bit) ----
#define SD_MMC_D0           8
#define SD_MMC_CMD          42
#define SD_MMC_CLK          9

// ---- Battery ADC ----
#define BAT_ADC_CHANNEL     0   // ADC_CHANNEL_0 = GPIO1
#define BAT_ADC_PIN         1
