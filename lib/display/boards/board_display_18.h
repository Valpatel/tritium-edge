/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file board_display_18.h
 * @brief Display configuration for ESP32-S3-Touch-AMOLED-1.8 (SH8601Z, 368x448, QSPI)
 *
 * Pin definitions come from include/boards/esp32_s3_touch_amoled_18.h (force-included).
 * Init sequence from references/ESP32-S3-Touch-AMOLED-1.8/examples/ESP-IDF-v5.3.2/
 * 05_LVGL_WITH_RAM/main/example_qspi_with_ram.c.
 *
 * NOTE: Display reset is via TCA9554 I/O expander (LCD_RST = -1).
 * The driver will perform a software reset (LCD_CMD_SWRESET) when RST GPIO is -1.
 */

#pragma once

#include "../drivers/esp_lcd_sh8601.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Transport configuration ---- */
#define BOARD_LCD_SPI_HOST          SPI2_HOST
#define BOARD_LCD_SPI_MODE          0
#define BOARD_LCD_PIXEL_CLK_HZ      (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS          32
#define BOARD_LCD_PARAM_BITS        8
#define BOARD_LCD_QSPI              1

/* ---- Panel dimensions ---- */
#define BOARD_LCD_H_RES             368
#define BOARD_LCD_V_RES             448

/* ---- Reset ---- */
/* Reset via TCA9554 IO expander, not direct GPIO */
#define BOARD_LCD_RST_GPIO          (-1)
#define BOARD_LCD_RST_ACTIVE_HIGH   0

/* ---- Backlight ---- */
/* AMOLED: no backlight GPIO. Brightness controlled via 0x51 command. */
#define BOARD_LCD_BL_GPIO           (-1)
#define BOARD_LCD_BL_ACTIVE_HIGH    1
#define BOARD_LCD_BL_PWM_CHANNEL    (-1)

/* ---- Panel gap/offset ---- */
#define BOARD_LCD_X_GAP             0
#define BOARD_LCD_Y_GAP             0

/* ---- Init commands for SH8601Z ---- */
/* From example_qspi_with_ram.c (Waveshare 1.8 reference demo) */
static const sh8601_lcd_init_cmd_t board_lcd_init_cmds_18[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},                     /* Sleep out + 120ms */
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},                 /* Set tear scan line */
    {0x35, (uint8_t[]){0x00}, 1, 0},                        /* Tearing effect on */
    {0x53, (uint8_t[]){0x20}, 1, 10},                       /* Write CTRL display */
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},     /* Column address: 0..367 */
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},     /* Row address: 0..447 */
    {0x51, (uint8_t[]){0x00}, 1, 10},                       /* Brightness = 0 */
    {0x29, (uint8_t[]){0x00}, 0, 10},                       /* Display on */
    {0x51, (uint8_t[]){0xFF}, 1, 0},                        /* Brightness = max */
};

#define BOARD_LCD_INIT_CMDS         board_lcd_init_cmds_18
#define BOARD_LCD_INIT_CMDS_SIZE    (sizeof(board_lcd_init_cmds_18) / sizeof(sh8601_lcd_init_cmd_t))

#ifdef __cplusplus
}
#endif
