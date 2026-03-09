#pragma once
// Waveshare ESP32-S3-Touch-AMOLED-2.41-B
// 450x600, RM690B0 QSPI AMOLED, FT6336 capacitive touch
// QMI8658 IMU, PCF85063 RTC
// STATUS: Pin-verified from Waveshare wiki

// ---- Component Presence ----
#define HAS_IMU             1
#define HAS_RTC             1
#define HAS_AUDIO_CODEC     0
#define HAS_PMIC            0
#define HAS_SDCARD          1
#define HAS_CAMERA          0
#define HAS_IO_EXPANDER     1

// ---- Display (RM690B0 via QSPI) ----
#define DISPLAY_DRIVER      "RM690B0"
#define DISPLAY_IF          "QSPI"
#define DISPLAY_BPP         16
#define DISPLAY_WIDTH       450
#define DISPLAY_HEIGHT      600
#define DISPLAY_ROTATION    0   // 0=portrait, 1=landscape CW, 2=portrait 180, 3=landscape CCW

#define LCD_QSPI_CS         9
#define LCD_QSPI_CLK        10
#define LCD_QSPI_D0         11
#define LCD_QSPI_D1         12
#define LCD_QSPI_D2         13
#define LCD_QSPI_D3         14
#define LCD_RST             21
#define LCD_BL              (-1)  // AMOLED, no backlight pin
// TE pin is on IO expander (EXIO0)

// ---- Touch (FT6336 via I2C) ----
#define TOUCH_SDA           47
#define TOUCH_SCL           48
#define TOUCH_INT           (-1)  // On IO expander (EXIO2)
#define TOUCH_RST           (-1)

// ---- IMU (QMI8658 via shared I2C) ----
#define IMU_SDA             47
#define IMU_SCL             48
#define IMU_I2C_ADDR        0x6B
// INT1 on IO expander (EXIO3), INT2 on (EXIO4)

// ---- RTC (PCF85063 via shared I2C) ----
#define RTC_SDA             47
#define RTC_SCL             48
#define RTC_I2C_ADDR        0x51

// ---- IO Expander ----
// Connected via I2C on GPIO 47/48
// EXIO0 = LCD TE
// EXIO2 = Touch INT
// EXIO3 = IMU INT1
// EXIO4 = IMU INT2

// ---- SD Card (SPI/SDMMC, direct GPIO) ----
#define SD_MMC_D0           6   // MISO / DAT0
#define SD_MMC_CMD          5   // MOSI / CMD
#define SD_MMC_CLK          4   // SCK / CLK
#define SD_SPI_CS           2   // CS (SPI mode)

// ---- Battery ----
#define BAT_PWR_EN          16  // Battery power enable
