/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file board_display_241b.h
 * @brief Display configuration for ESP32-S3-Touch-AMOLED-2.41-B (RM690B0, 450x600, QSPI)
 *
 * Pin definitions come from include/boards/esp32_s3_touch_amoled_241b.h (force-included).
 * Init sequence from references/ESP32-S3-Touch-AMOLED-2.41-Demo/CN/Arduino/examples/09_LVGL_Test.
 *
 * IMPORTANT: RM690B0 has 452px memory width but 450px visible.
 * Column address set starts at offset 16 (0x10). Set x_gap = 16.
 * Also requires even-aligned coordinates for LVGL rounder callback.
 */

#pragma once

#include "../drivers/esp_lcd_sh8601.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Board identity (for runtime verification) ---- */
#define BOARD_NAME                  "ESP32-S3-Touch-AMOLED-2.41-B"
#define BOARD_DISPLAY_DRIVER        "RM690B0"
#define BOARD_LCD_EXPECTED_ID       0x690B00    /* RM690B0 MIPI DCS ID (reg 0x04) */

/* ---- Transport configuration ---- */
#define BOARD_LCD_SPI_HOST          SPI2_HOST
#define BOARD_LCD_SPI_MODE          0
#define BOARD_LCD_PIXEL_CLK_HZ      (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS          32
#define BOARD_LCD_PARAM_BITS        8
#define BOARD_LCD_QSPI              1

/* ---- Panel dimensions ---- */
#define BOARD_LCD_H_RES             450
#define BOARD_LCD_V_RES             600

/* ---- Reset ---- */
/* Direct GPIO reset on pin LCD_RST (21) */
#define BOARD_LCD_RST_GPIO          LCD_RST
#define BOARD_LCD_RST_ACTIVE_HIGH   0

/* ---- Backlight ---- */
/* AMOLED: no backlight GPIO. Brightness controlled via 0x51 command. */
#define BOARD_LCD_BL_GPIO           (-1)
#define BOARD_LCD_BL_ACTIVE_HIGH    1
#define BOARD_LCD_BL_PWM_CHANNEL    (-1)

/* ---- Panel gap/offset ---- */
/* RM690B0 memory offset: 452px memory width, 450px visible, offset_x=16 */
#define BOARD_LCD_X_GAP             16
#define BOARD_LCD_Y_GAP             0

/* ---- Init commands for RM690B0 ---- */
/* From 09_LVGL_Test.ino (Waveshare 2.41-B reference demo) */
static const sh8601_lcd_init_cmd_t board_lcd_init_cmds_241b[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},                   /* Enter page 0x20 */
    {0x26, (uint8_t[]){0x0A}, 1, 0},                   /* Page 0x20 config */
    {0x24, (uint8_t[]){0x80}, 1, 0},                   /* Page 0x20 config */
    {0xFE, (uint8_t[]){0x00}, 1, 0},                   /* Return to page 0x00 */
    {0x3A, (uint8_t[]){0x55}, 1, 0},                   /* Pixel format: RGB565 */
    {0xC2, (uint8_t[]){0x00}, 1, 10},                  /* Gate timing */
    {0x35, (uint8_t[]){0x00}, 0, 0},                   /* Tearing effect off */
    {0x51, (uint8_t[]){0x00}, 1, 10},                  /* Brightness = 0 */
    {0x11, (uint8_t[]){0x00}, 0, 80},                  /* Sleep out + 80ms delay */
    {0x2A, (uint8_t[]){0x00, 0x10, 0x01, 0xD1}, 4, 0}, /* Column address: 16..465 */
    {0x2B, (uint8_t[]){0x00, 0x00, 0x02, 0x57}, 4, 0}, /* Row address: 0..599 */
    {0x29, (uint8_t[]){0x00}, 0, 10},                  /* Display on */
    {0x51, (uint8_t[]){0xFF}, 1, 0},                   /* Brightness = max */
};

#define BOARD_LCD_INIT_CMDS         board_lcd_init_cmds_241b
#define BOARD_LCD_INIT_CMDS_SIZE    (sizeof(board_lcd_init_cmds_241b) / sizeof(sh8601_lcd_init_cmd_t))

#ifdef __cplusplus
}
#endif
