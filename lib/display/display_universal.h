/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file display_universal.h
 * @brief Runtime display initialization for universal firmware.
 *
 * When BOARD_UNIVERSAL is defined, display_universal.cpp replaces display.cpp
 * and provides the full display.h API plus this additional function:
 *
 *   const board_config_t* cfg = board_config_get(fp->detected);
 *   esp_err_t ret = display_init_universal(cfg);
 *
 * After calling display_init_universal(), all standard display.h functions
 * work normally (display_get_panel(), display_get_width(), etc.).
 */

#pragma once

#include "board_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize display from runtime board config.
 *
 * Dispatches to AXS15231B, SH8601, or RGB init based on cfg->display_type.
 * Handles TCA9554 reset (via bit-bang I2C), manual GPIO reset, backlight,
 * and panel ID verification.
 *
 * @param cfg  Board config from board_config_get()
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if cfg is NULL
 */
esp_err_t display_init_universal(const board_config_t* cfg);

#ifdef __cplusplus
}
#endif
