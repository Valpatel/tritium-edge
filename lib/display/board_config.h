/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file board_config.h
 * @brief Runtime board configuration for universal firmware.
 *
 * Replaces compile-time #if defined(BOARD_*) dispatch. Each board's full
 * display configuration lives in a board_config_t struct. All 6 configs
 * are compiled in simultaneously; the fingerprint selects which one to use.
 */

#pragma once

#include "board_fingerprint.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Display driver types */
typedef enum {
    DISPLAY_TYPE_AXS15231B,    /* QSPI: 349, 35BC */
    DISPLAY_TYPE_SH8601,       /* QSPI: 241B (RM690B0), 18 (SH8601Z), 191M (RM67162) */
    DISPLAY_TYPE_RGB,          /* RGB parallel: 43C-BOX (ST7262) */
} display_type_t;

/* Generic init command (same layout as axs15231b_lcd_init_cmd_t and sh8601_lcd_init_cmd_t) */
typedef struct {
    int cmd;
    const void* data;
    size_t data_bytes;
    unsigned int delay_ms;
} lcd_init_cmd_t;

/* RGB pin set (16 data pins + 4 control) */
typedef struct {
    int pclk, hsync, vsync, de;
    int data[16];   /* B3..B7, G2..G7, R3..R7 */
    int pclk_hz;
    int hsync_pulse, hsync_bp, hsync_fp;
    int vsync_pulse, vsync_bp, vsync_fp;
    bool pclk_active_neg;
    int data_width;
    int num_fbs;
    int bounce_buf_rows;
} rgb_config_t;

/* Full board display configuration */
typedef struct {
    board_id_t id;
    const char* name;
    const char* driver_name;
    display_type_t display_type;

    int width, height;

    /* QSPI transport (for AXS15231B and SH8601 boards) */
    struct {
        int host;       /* SPI2_HOST or SPI3_HOST */
        int mode;       /* SPI mode (0 or 3) */
        int clk_hz;
        int cmd_bits;
        int param_bits;
        int clk, cs, d0, d1, d2, d3;
    } qspi;

    /* Reset */
    int rst_gpio;           /* Pass to panel driver (-1 = no driver reset) */
    int rst_gpio_manual;    /* For manual toggle before init (-1 = none) */
    bool rst_active_high;

    /* Backlight */
    int bl_gpio;            /* -1 = no direct GPIO backlight */
    bool bl_active_high;
    int bl_pwm_channel;

    /* Gap/offset */
    int x_gap, y_gap;

    /* Init commands */
    const lcd_init_cmd_t* init_cmds;
    size_t init_cmds_count;

    /* Board-specific pre-init: TCA9554 display reset */
    bool needs_tca9554_reset;
    int tca9554_sda, tca9554_scl;
    uint8_t tca9554_addr;

    /* Expected panel chip ID (for QSPI readback verification) */
    uint32_t expected_chip_id;

    /* RGB-specific config (NULL for QSPI boards) */
    const rgb_config_t* rgb;
} board_config_t;

/**
 * @brief Look up board config by ID.
 *
 * @return Pointer to static config, or NULL if ID is unknown.
 */
const board_config_t* board_config_get(board_id_t id);

/**
 * @brief Get array of all known board configs.
 *
 * @param[out] count Number of configs in array.
 * @return Pointer to config array.
 */
const board_config_t* board_config_get_all(int* count);

#ifdef __cplusplus
}
#endif
