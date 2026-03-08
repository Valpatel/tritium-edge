/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file board_display_349.h
 * @brief Display configuration for ESP32-S3-Touch-LCD-3.49 (AXS15231B, 172x640, QSPI)
 *
 * Pin definitions come from include/boards/esp32_s3_touch_lcd_349.h (force-included).
 * Init sequence is the driver's built-in default (vendor_specific_init_default in
 * esp_lcd_axs15231b.c), so no override is needed.
 */

#pragma once

#include "../drivers/esp_lcd_axs15231b.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Transport configuration ---- */
#define BOARD_LCD_SPI_HOST          LCD_SPI_HOST
#define BOARD_LCD_SPI_MODE          3
#define BOARD_LCD_PIXEL_CLK_HZ      (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS          32
#define BOARD_LCD_PARAM_BITS        8
#define BOARD_LCD_QSPI              1

/* ---- Panel dimensions ---- */
#define BOARD_LCD_H_RES             172
#define BOARD_LCD_V_RES             640

/* ---- Reset ---- */
/* Manual GPIO reset with vendor timing (30/250/30ms) — see display.cpp.
 * Pass -1 to driver so it doesn't do its own faster reset. */
#define BOARD_LCD_RST_GPIO_PIN      LCD_RST     /* Physical reset pin (GPIO 21) */
#define BOARD_LCD_RST_GPIO          (-1)        /* Tell driver: no GPIO reset */
#define BOARD_LCD_RST_ACTIVE_HIGH   0

/* ---- Backlight ---- */
#define BOARD_LCD_BL_GPIO           LCD_BL      /* GPIO 8, active-LOW (0=full, 255=off) */
#define BOARD_LCD_BL_ACTIVE_HIGH    0
#define BOARD_LCD_BL_PWM_CHANNEL    0

/* ---- Init commands ---- */
/* Override default init sequence which ends with 0x22 (All Pixels Off).
 * Match vendor reference: just Sleep Out + Display On.
 * The full register config is in ROM defaults and works without explicit programming. */
static const axs15231b_lcd_init_cmd_t board_lcd_init_cmds_349[] = {
    {0x11, (uint8_t []){0x00}, 0, 100},  // Sleep Out
    {0x29, (uint8_t []){0x00}, 0, 100},  // Display On
};
#define BOARD_LCD_INIT_CMDS         board_lcd_init_cmds_349
#define BOARD_LCD_INIT_CMDS_SIZE    (sizeof(board_lcd_init_cmds_349) / sizeof(board_lcd_init_cmds_349[0]))

/* ---- Panel gap/offset ---- */
#define BOARD_LCD_X_GAP             0
#define BOARD_LCD_Y_GAP             0

#ifdef __cplusplus
}
#endif
