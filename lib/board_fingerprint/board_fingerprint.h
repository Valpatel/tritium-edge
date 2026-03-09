/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file board_fingerprint.h
 * @brief Runtime hardware fingerprinting for ESP32-S3 Waveshare boards.
 *
 * Scans I2C buses on all known pin combinations, probes for peripherals,
 * and matches the result against known board profiles. Runs before display
 * init — no board-specific code required.
 *
 * Use case: detect firmware/hardware mismatch, enable universal firmware
 * that selects the correct driver at runtime based on detected hardware.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Board ID enum ---- */

typedef enum {
    BOARD_ID_UNKNOWN = 0,
    BOARD_ID_TOUCH_LCD_349,       /* 172x640  AXS15231B QSPI  */
    BOARD_ID_TOUCH_LCD_35BC,      /* 320x480  AXS15231B QSPI  */
    BOARD_ID_TOUCH_LCD_43C_BOX,   /* 800x480  ST7262 RGB      */
    BOARD_ID_TOUCH_AMOLED_241B,   /* 450x600  RM690B0 QSPI    */
    BOARD_ID_TOUCH_AMOLED_18,     /* 368x448  SH8601Z QSPI    */
    BOARD_ID_AMOLED_191M,         /* 240x536  RM67162 QSPI    */
    BOARD_ID_COUNT
} board_id_t;

/* ---- I2C bus scan result ---- */

#define FP_MAX_I2C_BUSES    6
#define FP_MAX_I2C_DEVICES  16

typedef struct {
    uint8_t sda;
    uint8_t scl;
    uint8_t devices[FP_MAX_I2C_DEVICES];  /* I2C addresses that ACK'd */
    uint8_t count;                         /* number of devices found */
} fp_i2c_bus_t;

/* ---- Full fingerprint result ---- */

typedef struct {
    /* Detection result */
    board_id_t detected;           /* best-match board ID */
    board_id_t compiled;           /* what this firmware was compiled for */
    bool match;                    /* detected == compiled */
    int confidence;                /* 0-100, how certain the match is */

    /* Human-readable names */
    const char* detected_name;
    const char* compiled_name;

    /* Known peripheral flags (from I2C scan) */
    bool has_imu;                  /* QMI8658 @ 0x6B */
    bool has_rtc;                  /* PCF85063 @ 0x51 */
    bool has_pmic;                 /* AXP2101 @ 0x34 */
    bool has_touch_3b;             /* AXS15231B touch @ 0x3B */
    bool has_touch_38;             /* FT3168/FT6336 touch @ 0x38 */
    bool has_tca9554;              /* TCA9554 IO expander @ 0x20 */
    bool has_audio;                /* ES8311 codec @ 0x18 */

    /* Raw I2C scan data */
    fp_i2c_bus_t buses[FP_MAX_I2C_BUSES];
    uint8_t bus_count;             /* how many pin pairs had devices */

    uint32_t scan_time_ms;
} board_fingerprint_t;

/* ---- Public API ---- */

/**
 * @brief Scan hardware and identify the board.
 *
 * Probes all known I2C pin combinations, records which devices respond,
 * and matches against known board profiles. Takes ~200ms.
 *
 * Safe to call before display_init() or any other hardware setup.
 * Releases all GPIO/I2C resources before returning.
 *
 * @return Pointer to static fingerprint struct (never NULL, valid until
 *         next call to board_fingerprint_scan).
 */
const board_fingerprint_t* board_fingerprint_scan(void);

/**
 * @brief Get the last scan result without re-scanning.
 *
 * @return Pointer to static fingerprint struct, or NULL if never scanned.
 */
const board_fingerprint_t* board_fingerprint_get(void);

/**
 * @brief Get human-readable name for a board ID.
 */
const char* board_id_to_name(board_id_t id);

/**
 * @brief Print fingerprint summary to Serial.
 */
void board_fingerprint_print(const board_fingerprint_t* fp);

#ifdef __cplusplus
}
#endif
