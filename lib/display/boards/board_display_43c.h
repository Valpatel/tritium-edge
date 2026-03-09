/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file board_display_43c.h
 * @brief Display configuration for ESP32-S3-Touch-LCD-4.3C-BOX (ST7262, 800x480, RGB parallel)
 *
 * Pin definitions come from include/boards/esp32_s3_touch_lcd_43c_box.h (force-included).
 *
 * This board uses esp_lcd_new_rgb_panel() -- no custom panel driver needed.
 * The ST7262 is a "dumb" RGB panel with no command interface. Configuration is
 * done entirely through timing parameters.
 *
 * Backlight is controlled via the IO extension chip (I2C 0x24), not a direct GPIO.
 */

#pragma once

#include "esp_lcd_panel_rgb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Board identity (for runtime verification) ---- */
#define BOARD_NAME                  "ESP32-S3-Touch-LCD-4.3C-BOX"
#define BOARD_DISPLAY_DRIVER        "ST7262"
#define BOARD_LCD_EXPECTED_ID       0x000000    /* RGB panel: no chip ID readback */

/* ---- Panel dimensions ---- */
#define BOARD_LCD_H_RES             800
#define BOARD_LCD_V_RES             480
#define BOARD_LCD_QSPI              0

/* ---- Reset ---- */
#define BOARD_LCD_RST_GPIO          (-1)    /* No reset needed for RGB panel */
#define BOARD_LCD_RST_ACTIVE_HIGH   0

/* ---- Backlight ---- */
/* Backlight via IO extension chip (I2C 0x24, IO2) -- not direct GPIO */
#define BOARD_LCD_BL_GPIO           (-1)
#define BOARD_LCD_BL_ACTIVE_HIGH    1
#define BOARD_LCD_BL_PWM_CHANNEL    (-1)

/* IO extension chip for backlight */
#define BOARD_IO_EXT_I2C_ADDR       0x24
#define BOARD_IO_EXT_I2C_PORT       I2C_NUM_0
#define BOARD_IO_EXT_BL_PIN         2       /* IO_EXTENSION_IO_2 */

/* ---- Panel gap/offset ---- */
#define BOARD_LCD_X_GAP             0
#define BOARD_LCD_Y_GAP             0

/* ---- RGB panel timing configuration ---- */
/* From references/ESP32-S3-Touch-LCD-4.3C/examples/esp-idf/03_lcd/components/rgb_lcd_port/ */
#define BOARD_RGB_PCLK_HZ           (16 * 1000 * 1000)     /* 16 MHz pixel clock */
#define BOARD_RGB_HSYNC_PULSE       8
#define BOARD_RGB_HSYNC_BACK_PORCH  8
#define BOARD_RGB_HSYNC_FRONT_PORCH 4
#define BOARD_RGB_VSYNC_PULSE       8
#define BOARD_RGB_VSYNC_BACK_PORCH  8
#define BOARD_RGB_VSYNC_FRONT_PORCH 4
#define BOARD_RGB_PCLK_ACTIVE_NEG   1       /* Pixel clock active on falling edge */
#define BOARD_RGB_DATA_WIDTH        16      /* 16-bit RGB565 parallel */
#define BOARD_RGB_NUM_FBS           2       /* Double buffering */
#define BOARD_RGB_BOUNCE_BUF_SIZE   (800 * 10)  /* 10 rows bounce buffer */

/**
 * @brief Get the RGB panel configuration structure.
 *
 * Uses pin definitions from the board header (force-included).
 * Caller should pass this to esp_lcd_new_rgb_panel().
 */
static inline esp_lcd_rgb_panel_config_t board_display_43c_get_rgb_config(void)
{
    esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = BOARD_RGB_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = BOARD_RGB_HSYNC_PULSE,
            .hsync_back_porch = BOARD_RGB_HSYNC_BACK_PORCH,
            .hsync_front_porch = BOARD_RGB_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = BOARD_RGB_VSYNC_PULSE,
            .vsync_back_porch = BOARD_RGB_VSYNC_BACK_PORCH,
            .vsync_front_porch = BOARD_RGB_VSYNC_FRONT_PORCH,
            .flags = {
                .pclk_active_neg = BOARD_RGB_PCLK_ACTIVE_NEG,
            },
        },
        .data_width = BOARD_RGB_DATA_WIDTH,
        .bits_per_pixel = 16,
        .num_fbs = BOARD_RGB_NUM_FBS,
        .bounce_buffer_size_px = BOARD_RGB_BOUNCE_BUF_SIZE,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = LCD_HSYNC,
        .vsync_gpio_num = LCD_VSYNC,
        .de_gpio_num = LCD_DE,
        .pclk_gpio_num = LCD_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            LCD_B3,     /* B3 */
            LCD_B4,     /* B4 */
            LCD_B5,     /* B5 */
            LCD_B6,     /* B6 */
            LCD_B7,     /* B7 */
            LCD_G2,     /* G2 */
            LCD_G3,     /* G3 */
            LCD_G4,     /* G4 */
            LCD_G5,     /* G5 */
            LCD_G6,     /* G6 */
            LCD_G7,     /* G7 */
            LCD_R3,     /* R3 */
            LCD_R4,     /* R4 */
            LCD_R5,     /* R5 */
            LCD_R6,     /* R6 */
            LCD_R7,     /* R7 */
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };
    return config;
}

#ifdef __cplusplus
}
#endif
