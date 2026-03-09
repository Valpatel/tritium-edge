/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file board_display_191m.h
 * @brief Display configuration for ESP32-S3-AMOLED-1.91-M (RM67162, 240x536, QSPI)
 *
 * Pin definitions come from include/boards/esp32_s3_amoled_191m.h (force-included).
 * Init sequence from references/ESP32-S3-AMOLED-1.91-Demo/Arduino/examples/LVGL/LVGL.ino.
 *
 * NOTE: This board has no touch panel ("M" = module).
 */

#pragma once

#include "../drivers/esp_lcd_sh8601.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Board identity (for runtime verification) ---- */
#define BOARD_NAME                  "ESP32-S3-AMOLED-1.91-M"
#define BOARD_DISPLAY_DRIVER        "RM67162"
#define BOARD_LCD_EXPECTED_ID       0x671620    /* RM67162 MIPI DCS ID (reg 0x04) */

/* ---- Transport configuration ---- */
#define BOARD_LCD_SPI_HOST          SPI2_HOST
#define BOARD_LCD_SPI_MODE          0
#define BOARD_LCD_PIXEL_CLK_HZ      (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS          32
#define BOARD_LCD_PARAM_BITS        8
#define BOARD_LCD_QSPI              1

/* ---- Panel dimensions ---- */
#define BOARD_LCD_H_RES             240
#define BOARD_LCD_V_RES             536

/* ---- Reset ---- */
/* Direct GPIO reset on pin LCD_RST (17) */
#define BOARD_LCD_RST_GPIO          LCD_RST
#define BOARD_LCD_RST_ACTIVE_HIGH   0

/* ---- Backlight ---- */
/* AMOLED: no backlight GPIO. Brightness controlled via 0x51 command. */
#define BOARD_LCD_BL_GPIO           (-1)
#define BOARD_LCD_BL_ACTIVE_HIGH    1
#define BOARD_LCD_BL_PWM_CHANNEL    (-1)

/* ---- Panel gap/offset ---- */
#define BOARD_LCD_X_GAP             0
#define BOARD_LCD_Y_GAP             0

/* ---- Init commands for RM67162 ---- */
/* From LVGL.ino (Waveshare 1.91-M reference demo) */
static const sh8601_lcd_init_cmd_t board_lcd_init_cmds_191m[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},                      /* Sleep out + 120ms */
    {0x36, (uint8_t[]){0x00}, 1, 0},                        /* MADCTL: portrait mode (no rotation) */
    {0x3A, (uint8_t[]){0x55}, 1, 0},                        /* Pixel format: RGB565 */
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},     /* Column address: 0..239 */
    {0x2B, (uint8_t[]){0x00, 0x00, 0x02, 0x17}, 4, 0},     /* Row address: 0..535 */
    {0x51, (uint8_t[]){0x00}, 1, 10},                       /* Brightness = 0 */
    {0x29, (uint8_t[]){0x00}, 0, 10},                       /* Display on */
    {0x51, (uint8_t[]){0xFF}, 1, 0},                        /* Brightness = max */
};

#define BOARD_LCD_INIT_CMDS         board_lcd_init_cmds_191m
#define BOARD_LCD_INIT_CMDS_SIZE    (sizeof(board_lcd_init_cmds_191m) / sizeof(sh8601_lcd_init_cmd_t))

#ifdef __cplusplus
}
#endif
