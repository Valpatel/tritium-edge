#pragma once
// Waveshare ESP32-S3-Touch-LCD-4.3C-BOX
// 800x480, ST7262 RGB parallel IPS LCD, GT911 capacitive touch
// CH422G I2C IO expander, dual-MIC array
// Same board as 4.3C but in an enclosure
// STATUS: Pin-verified from Waveshare wiki

// ---- Component Presence ----
#define HAS_IMU             0
#define HAS_RTC             0
#define HAS_AUDIO_CODEC     0
#define HAS_PMIC            0
#define HAS_SDCARD          1
#define HAS_CAMERA          0
#define HAS_IO_EXPANDER     1
#define HAS_DUAL_MIC        1

// ---- Display (ST7262 RGB parallel) ----
#define DISPLAY_DRIVER      "ST7262"
#define DISPLAY_IF          "RGB"
#define DISPLAY_BPP         16
#define DISPLAY_WIDTH       800
#define DISPLAY_HEIGHT      480
#define DISPLAY_ROTATION    0   // 0=portrait (already landscape native), 1=CW, 2=180, 3=CCW

#define LCD_PCLK            7
#define LCD_HSYNC           46
#define LCD_VSYNC           3
#define LCD_DE              5

// RGB data pins (active bits only, 16-bit RGB565)
// Pin mapping from Waveshare reference: D0-D4=Blue, D5-D10=Green, D11-D15=Red
#define LCD_B3              14
#define LCD_B4              38
#define LCD_B5              18
#define LCD_B6              17
#define LCD_B7              10
#define LCD_G2              39
#define LCD_G3              0
#define LCD_G4              45
#define LCD_G5              48
#define LCD_G6              47
#define LCD_G7              21
#define LCD_R3              1
#define LCD_R4              2
#define LCD_R5              42
#define LCD_R6              41
#define LCD_R7              40

// Backlight via CH422G IO expander (EXIO2)
#define LCD_BL              (-1)  // Via IO expander

// ---- Touch (GT911 via I2C) ----
#define TOUCH_SDA           8
#define TOUCH_SCL           9
#define TOUCH_INT           4
#define TOUCH_RST           (-1)  // Via CH422G (EXIO1)

// ---- SD Card (SPI mode, CS via CH422G EXIO4) ----
#define SD_SPI_MOSI         11
#define SD_SPI_SCK          12
#define SD_SPI_MISO         13
#define SD_SPI_CS           (-1)    // CS driven by CH422G EXIO4, not direct GPIO
#define SD_USE_SPI          1       // SPI mode (not SDMMC)
#define SD_CS_VIA_EXPANDER  1       // CS routed through IO expander
#define SD_CS_EXPANDER_PIN  4       // EXIO4 on CH422G

// ---- IO Expander (CH422G) ----
#define IO_EXP_SDA          8
#define IO_EXP_SCL          9
// CH422G uses fixed I2C address
// EXIO1 = Touch RST
// EXIO2 = Backlight control
// EXIO4 = SD card CS
