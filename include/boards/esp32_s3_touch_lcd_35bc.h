#pragma once
// Waveshare ESP32-S3-Touch-LCD-3.5B-C
// 320x480, AXS15231B QSPI IPS LCD with integrated touch
// QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, ES8311 audio codec
// OV5640 camera interface (C variant includes camera module)
// STATUS: Pin-verified from Waveshare demo code (ESP32-S3-Touch-LCD-3.5B-Demo)

// ---- Component Presence ----
#define HAS_IMU             1
#define HAS_RTC             1
#define HAS_AUDIO_CODEC     1
#define HAS_PMIC            1
#define HAS_SDCARD          1
#define HAS_CAMERA          1
#define HAS_IO_EXPANDER     0

// ---- Display (AXS15231B via QSPI) ----
#define DISPLAY_DRIVER      "AXS15231B"
#define DISPLAY_IF          "QSPI"
#define DISPLAY_BPP         16
#define DISPLAY_WIDTH       320
#define DISPLAY_HEIGHT      480
#define DISPLAY_ROTATION    0   // 0=portrait, 1=landscape CW, 2=portrait 180, 3=landscape CCW

#define LCD_QSPI_CS         12
#define LCD_QSPI_CLK        5
#define LCD_QSPI_D0         1
#define LCD_QSPI_D1         2
#define LCD_QSPI_D2         3
#define LCD_QSPI_D3         4
#define LCD_BL              6

// ---- Touch (integrated in AXS15231B via I2C) ----
#define TOUCH_SDA           8
#define TOUCH_SCL           7
#define TOUCH_INT           (-1)
#define TOUCH_RST           (-1)
#define TOUCH_I2C_ADDR      0x3B
#define TOUCH_I2C_NUM       0   // Shared I2C bus with IMU/RTC/PMIC

// ---- IMU (QMI8658 via shared I2C) ----
#define IMU_SDA             8
#define IMU_SCL             7
#define IMU_I2C_ADDR        0x6B

// ---- RTC (PCF85063 via shared I2C) ----
#define RTC_SDA             8
#define RTC_SCL             7
#define RTC_I2C_ADDR        0x51

// ---- PMIC (AXP2101 via shared I2C) ----
#define PMIC_SDA            8
#define PMIC_SCL            7
#define PMIC_I2C_ADDR       0x34

// ---- Audio (ES8311 codec) ----
#define AUDIO_I2S_MCLK      44
#define AUDIO_I2S_BCLK      13
#define AUDIO_I2S_WS        15
#define AUDIO_I2S_DOUT      16
#define AUDIO_I2S_DIN       14
#define AUDIO_I2C_SDA       8
#define AUDIO_I2C_SCL       7
#define AUDIO_CODEC_ADDR    0x18  // ES8311_ADDRRES_0

// ---- SD Card (SDMMC 1-bit) ----
#define SD_MMC_D0           9
#define SD_MMC_CMD          10
#define SD_MMC_CLK          11

// ---- Camera (OV5640 DVP) ----
#define CAM_XCLK            38
#define CAM_SIOD            8   // SCCB/I2C SDA (shared)
#define CAM_SIOC            7   // SCCB/I2C SCL (shared)
#define CAM_Y2              45
#define CAM_Y3              47
#define CAM_Y4              48
#define CAM_Y5              46
#define CAM_Y6              42
#define CAM_Y7              40
#define CAM_Y8              39
#define CAM_Y9              21
#define CAM_VSYNC           17
#define CAM_HREF            18
#define CAM_PCLK            41
#define CAM_PWDN            (-1)
#define CAM_RESET           (-1)
