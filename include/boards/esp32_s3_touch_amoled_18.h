#pragma once
// Waveshare ESP32-S3-Touch-AMOLED-1.8
// 368x448, SH8601Z QSPI AMOLED, FT3168 capacitive touch
// QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, ES8311 audio codec
// TCA9554 IO expander
// STATUS: Pin-verified from Waveshare BSP + pin_config.h + demo code

// ---- Component Presence ----
#define HAS_IMU             1
#define HAS_RTC             1
#define HAS_AUDIO_CODEC     1
#define HAS_PMIC            1
#define HAS_SDCARD          1
#define HAS_CAMERA          0
#define HAS_IO_EXPANDER     1

// ---- Display (SH8601Z via QSPI) ----
#define DISPLAY_DRIVER      "SH8601"
#define DISPLAY_IF          "QSPI"
#define DISPLAY_BPP         16
#define DISPLAY_WIDTH       368
#define DISPLAY_HEIGHT      448
#define DISPLAY_ROTATION    0   // 0=portrait, 1=landscape CW, 2=portrait 180, 3=landscape CCW

#define LCD_QSPI_CS         12
#define LCD_QSPI_CLK        11
#define LCD_QSPI_D0         4
#define LCD_QSPI_D1         5
#define LCD_QSPI_D2         6
#define LCD_QSPI_D3         7
#define LCD_RST             (-1)  // Via IO expander
#define LCD_BL              (-1)  // AMOLED, no backlight pin

// ---- Touch (FT3168 via I2C) ----
#define TOUCH_SDA           15
#define TOUCH_SCL           14
#define TOUCH_INT           21
#define TOUCH_RST           (-1)
#define TOUCH_I2C_ADDR      0x38

// ---- IMU (QMI8658 via shared I2C) ----
#define IMU_SDA             15
#define IMU_SCL             14
#define IMU_I2C_ADDR        0x6B

// ---- RTC (PCF85063 via shared I2C) ----
#define RTC_SDA             15
#define RTC_SCL             14
#define RTC_I2C_ADDR        0x51

// ---- PMIC (AXP2101 via shared I2C) ----
#define PMIC_SDA            15
#define PMIC_SCL            14
#define PMIC_I2C_ADDR       0x34

// ---- Audio (ES8311 codec) ----
#define AUDIO_I2S_MCLK      16
#define AUDIO_I2S_BCLK      9
#define AUDIO_I2S_WS        45
#define AUDIO_I2S_DOUT      8
#define AUDIO_I2S_DIN       10
#define AUDIO_PA_EN         46
#define AUDIO_I2C_SDA       15
#define AUDIO_I2C_SCL       14
#define AUDIO_CODEC_ADDR    0x18  // ES8311_ADDRRES_0

// ---- IO Expander (TCA9554) ----
#define IO_EXP_SDA          15
#define IO_EXP_SCL          14
#define IO_EXP_I2C_ADDR     0x20  // TCA9554_ADDRESS_000

// ---- SD Card (SDMMC 1-bit) ----
#define SD_MMC_D0           3
#define SD_MMC_CMD          1
#define SD_MMC_CLK          2
