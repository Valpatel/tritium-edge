/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file board_configs.cpp
 * @brief All 6 Waveshare board display configurations as runtime data.
 *
 * Each board's pin mapping, init sequence, and display parameters are
 * stored as board_config_t structs. The universal firmware includes all
 * of them and selects at runtime based on board_fingerprint_scan().
 */

#include "board_config.h"
#include <cstddef>

/* ========================================================================== */
/* Init command arrays — unique per board, same struct layout                  */
/* Cast-compatible with both axs15231b_lcd_init_cmd_t and sh8601_lcd_init_cmd_t */
/* ========================================================================== */

/* -- 349: AXS15231B (172x640) -- */
static const lcd_init_cmd_t init_cmds_349[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},  /* Sleep Out */
    {0x29, (uint8_t[]){0x00}, 0, 100},  /* Display On */
};

/* -- 35BC: AXS15231B (320x480) -- */
static const lcd_init_cmd_t init_cmds_35bc[] = {
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t[]){0x00, 0x30, 0x00, 0x02, 0x00, 0x00, 0x05, 0x3F, 0x30, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t[]){0x30, 0x04, 0x14, 0x50, 0x80, 0x30, 0x85, 0x80, 0xB4, 0x28, 0xFF, 0xFF, 0xFF, 0x20, 0x50, 0x10, 0x02, 0x06, 0x20, 0xD0, 0xC0, 0x01, 0x12, 0xA0, 0x91, 0xC0, 0x20, 0x7F, 0xFF, 0x00, 0x06}, 31, 0},
    {0xD0, (uint8_t[]){0x80, 0xB4, 0x21, 0x24, 0x08, 0x05, 0x10, 0x01, 0xF2, 0x02, 0xC2, 0x02, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0xC0, 0x10, 0x10, 0x40, 0x04, 0x00, 0x30, 0x10, 0x00, 0x03, 0x0D, 0x12}, 30, 0},
    {0xA3, (uint8_t[]){0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t[]){0x33, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x01, 0x01, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x04, 0x0D, 0x00, 0xFF, 0x42}, 30, 0},
    {0xC4, (uint8_t[]){0x00, 0x24, 0x33, 0x80, 0x66, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t[]){0x18, 0x00, 0x00, 0x03, 0xFE, 0xE8, 0x3B, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0xE8, 0x3B, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t[]){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x02, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t[]){0x50, 0x36, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF, 0x07, 0x11, 0x9C, 0x6F, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F}, 20, 0},
    {0xC9, (uint8_t[]){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t[]){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0xF7, 0x00, 0x66, 0x0D, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x04, 0x04, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t[]){0x30, 0x30, 0x8A, 0x00, 0x44, 0x04, 0x4A, 0xE5, 0x02, 0x4A, 0xE5, 0x02, 0x04, 0xD9, 0x02, 0x47, 0x03, 0x03, 0x03, 0x03, 0x83, 0x00, 0x00, 0x00, 0x80, 0x52, 0x53, 0x50, 0x50, 0x00}, 30, 0},
    {0xD6, (uint8_t[]){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x34, 0x02, 0x01, 0x83, 0xFF, 0x00, 0x20, 0x50, 0x00, 0x30, 0x03, 0x03, 0x50, 0x13, 0x00, 0x00, 0x00, 0x04, 0x50, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t[]){0x03, 0x01, 0x09, 0x0B, 0x0D, 0x0F, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x30, 0x30, 0x04, 0x00, 0x20, 0x20, 0x1F}, 19, 0},
    {0xD8, (uint8_t[]){0x02, 0x00, 0x08, 0x0A, 0x0C, 0x0E, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19}, 12, 0},
    {0xDF, (uint8_t[]){0x44, 0x33, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0, (uint8_t[]){0x1F, 0x20, 0x10, 0x17, 0x0D, 0x09, 0x12, 0x2A, 0x44, 0x25, 0x0C, 0x15, 0x13, 0x31, 0x36, 0x2F, 0x02}, 17, 0},
    {0xE1, (uint8_t[]){0x3F, 0x20, 0x10, 0x16, 0x0C, 0x08, 0x12, 0x29, 0x43, 0x25, 0x0C, 0x15, 0x13, 0x32, 0x36, 0x2F, 0x27}, 17, 0},
    {0xE2, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, (uint8_t[]){0x00}, 0, 0},
    {0x11, (uint8_t[]){0x00}, 0, 200},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

/* -- 241B: RM690B0 (450x600) -- */
static const lcd_init_cmd_t init_cmds_241b[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x26, (uint8_t[]){0x0A}, 1, 0},
    {0x24, (uint8_t[]){0x80}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0xC2, (uint8_t[]){0x00}, 1, 10},
    {0x35, (uint8_t[]){0x00}, 0, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x11, (uint8_t[]){0x00}, 0, 80},
    {0x2A, (uint8_t[]){0x00, 0x10, 0x01, 0xD1}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x02, 0x57}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

/* -- 18: SH8601Z (368x448) -- */
static const lcd_init_cmd_t init_cmds_18[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

/* -- 191M: RM67162 (240x536) -- */
static const lcd_init_cmd_t init_cmds_191m[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x02, 0x17}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

/* ========================================================================== */
/* RGB config for 4.3C-BOX                                                    */
/* ========================================================================== */

static const rgb_config_t rgb_43c = {
    .pclk   = 7,
    .hsync  = 46,
    .vsync  = 3,
    .de     = 5,
    .data   = {14, 38, 18, 17, 10,       /* B3-B7 */
               39,  0, 45, 48, 47, 21,   /* G2-G7 */
                1,  2, 42, 41, 40},      /* R3-R7 */
    .pclk_hz         = 16 * 1000 * 1000,
    .hsync_pulse     = 8,
    .hsync_bp        = 8,
    .hsync_fp        = 4,
    .vsync_pulse     = 8,
    .vsync_bp        = 8,
    .vsync_fp        = 4,
    .pclk_active_neg = true,
    .data_width      = 16,
    .num_fbs         = 2,
    .bounce_buf_rows = 10,
};

/* ========================================================================== */
/* Board config table                                                         */
/* ========================================================================== */

#define ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))

/* SPI host constants (ESP-IDF 5.x) */
#ifndef SPI2_HOST
#define SPI2_HOST 1
#endif
#ifndef SPI3_HOST
#define SPI3_HOST 2
#endif

static const board_config_t ALL_CONFIGS[] = {
    /* ---- ESP32-S3-Touch-LCD-3.49 (172x640 AXS15231B QSPI) ---- */
    {
        .id = BOARD_ID_TOUCH_LCD_349,
        .name = "ESP32-S3-Touch-LCD-3.49",
        .driver_name = "AXS15231B",
        .display_type = DISPLAY_TYPE_AXS15231B,
        .width = 172, .height = 640,
        .qspi = { .host = SPI3_HOST, .mode = 3, .clk_hz = 40000000,
                   .cmd_bits = 32, .param_bits = 8,
                   .clk = 10, .cs = 9, .d0 = 11, .d1 = 12, .d2 = 13, .d3 = 14 },
        .rst_gpio = -1,
        .rst_gpio_manual = 21,
        .rst_active_high = false,
        .bl_gpio = 8, .bl_active_high = false, .bl_pwm_channel = 0,
        .x_gap = 0, .y_gap = 0,
        .init_cmds = init_cmds_349, .init_cmds_count = ARRAY_SIZE(init_cmds_349),
        .needs_tca9554_reset = false, .tca9554_sda = 0, .tca9554_scl = 0, .tca9554_addr = 0,
        .expected_chip_id = 0x15231B,
        .rgb = NULL,
    },

    /* ---- ESP32-S3-Touch-LCD-3.5B-C (320x480 AXS15231B QSPI) ---- */
    {
        .id = BOARD_ID_TOUCH_LCD_35BC,
        .name = "ESP32-S3-Touch-LCD-3.5B-C",
        .driver_name = "AXS15231B",
        .display_type = DISPLAY_TYPE_AXS15231B,
        .width = 320, .height = 480,
        .qspi = { .host = SPI2_HOST, .mode = 3, .clk_hz = 40000000,
                   .cmd_bits = 32, .param_bits = 8,
                   .clk = 5, .cs = 12, .d0 = 1, .d1 = 2, .d2 = 3, .d3 = 4 },
        .rst_gpio = -1,
        .rst_gpio_manual = -1,
        .rst_active_high = false,
        .bl_gpio = 6, .bl_active_high = true, .bl_pwm_channel = 1,
        .x_gap = 0, .y_gap = 0,
        .init_cmds = init_cmds_35bc, .init_cmds_count = ARRAY_SIZE(init_cmds_35bc),
        .needs_tca9554_reset = true, .tca9554_sda = 8, .tca9554_scl = 7, .tca9554_addr = 0x20,
        .expected_chip_id = 0x15231B,
        .rgb = NULL,
    },

    /* ---- ESP32-S3-Touch-LCD-4.3C-BOX (800x480 ST7262 RGB) ---- */
    {
        .id = BOARD_ID_TOUCH_LCD_43C_BOX,
        .name = "ESP32-S3-Touch-LCD-4.3C-BOX",
        .driver_name = "ST7262",
        .display_type = DISPLAY_TYPE_RGB,
        .width = 800, .height = 480,
        .qspi = {},
        .rst_gpio = -1,
        .rst_gpio_manual = -1,
        .rst_active_high = false,
        .bl_gpio = -1, .bl_active_high = true, .bl_pwm_channel = -1,
        .x_gap = 0, .y_gap = 0,
        .init_cmds = NULL, .init_cmds_count = 0,
        .needs_tca9554_reset = false, .tca9554_sda = 0, .tca9554_scl = 0, .tca9554_addr = 0,
        .expected_chip_id = 0x000000,
        .rgb = &rgb_43c,
    },

    /* ---- ESP32-S3-Touch-AMOLED-2.41-B (450x600 RM690B0 QSPI) ---- */
    {
        .id = BOARD_ID_TOUCH_AMOLED_241B,
        .name = "ESP32-S3-Touch-AMOLED-2.41-B",
        .driver_name = "RM690B0",
        .display_type = DISPLAY_TYPE_SH8601,
        .width = 450, .height = 600,
        .qspi = { .host = SPI2_HOST, .mode = 0, .clk_hz = 40000000,
                   .cmd_bits = 32, .param_bits = 8,
                   .clk = 10, .cs = 9, .d0 = 11, .d1 = 12, .d2 = 13, .d3 = 14 },
        .rst_gpio = 21,
        .rst_gpio_manual = -1,
        .rst_active_high = false,
        .bl_gpio = -1, .bl_active_high = true, .bl_pwm_channel = -1,
        .x_gap = 16, .y_gap = 0,
        .init_cmds = init_cmds_241b, .init_cmds_count = ARRAY_SIZE(init_cmds_241b),
        .needs_tca9554_reset = false, .tca9554_sda = 0, .tca9554_scl = 0, .tca9554_addr = 0,
        .expected_chip_id = 0x690B00,
        .rgb = NULL,
    },

    /* ---- ESP32-S3-Touch-AMOLED-1.8 (368x448 SH8601Z QSPI) ---- */
    {
        .id = BOARD_ID_TOUCH_AMOLED_18,
        .name = "ESP32-S3-Touch-AMOLED-1.8",
        .driver_name = "SH8601Z",
        .display_type = DISPLAY_TYPE_SH8601,
        .width = 368, .height = 448,
        .qspi = { .host = SPI2_HOST, .mode = 0, .clk_hz = 40000000,
                   .cmd_bits = 32, .param_bits = 8,
                   .clk = 11, .cs = 12, .d0 = 4, .d1 = 5, .d2 = 6, .d3 = 7 },
        .rst_gpio = -1,
        .rst_gpio_manual = -1,
        .rst_active_high = false,
        .bl_gpio = -1, .bl_active_high = true, .bl_pwm_channel = -1,
        .x_gap = 0, .y_gap = 0,
        .init_cmds = init_cmds_18, .init_cmds_count = ARRAY_SIZE(init_cmds_18),
        .needs_tca9554_reset = true, .tca9554_sda = 15, .tca9554_scl = 14, .tca9554_addr = 0x20,
        .expected_chip_id = 0x860100,
        .rgb = NULL,
    },

    /* ---- ESP32-S3-AMOLED-1.91-M (240x536 RM67162 QSPI) ---- */
    {
        .id = BOARD_ID_AMOLED_191M,
        .name = "ESP32-S3-AMOLED-1.91-M",
        .driver_name = "RM67162",
        .display_type = DISPLAY_TYPE_SH8601,
        .width = 240, .height = 536,
        .qspi = { .host = SPI2_HOST, .mode = 0, .clk_hz = 40000000,
                   .cmd_bits = 32, .param_bits = 8,
                   .clk = 47, .cs = 6, .d0 = 18, .d1 = 7, .d2 = 48, .d3 = 5 },
        .rst_gpio = 17,
        .rst_gpio_manual = -1,
        .rst_active_high = false,
        .bl_gpio = -1, .bl_active_high = true, .bl_pwm_channel = -1,
        .x_gap = 0, .y_gap = 0,
        .init_cmds = init_cmds_191m, .init_cmds_count = ARRAY_SIZE(init_cmds_191m),
        .needs_tca9554_reset = false, .tca9554_sda = 0, .tca9554_scl = 0, .tca9554_addr = 0,
        .expected_chip_id = 0x671620,
        .rgb = NULL,
    },
};

/* ========================================================================== */
/* Lookup API                                                                 */
/* ========================================================================== */

const board_config_t* board_config_get(board_id_t id) {
    for (int i = 0; i < (int)ARRAY_SIZE(ALL_CONFIGS); i++) {
        if (ALL_CONFIGS[i].id == id) return &ALL_CONFIGS[i];
    }
    return NULL;
}

const board_config_t* board_config_get_all(int* count) {
    if (count) *count = (int)ARRAY_SIZE(ALL_CONFIGS);
    return ALL_CONFIGS;
}
