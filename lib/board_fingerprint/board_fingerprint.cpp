/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "board_fingerprint.h"
#include "tritium_compat.h"
#include "driver/gpio.h"

/* ========================================================================== */
/* Known I2C pin pairs for all Waveshare ESP32-S3 boards                      */
/* ========================================================================== */

struct i2c_pin_pair_t {
    uint8_t sda;
    uint8_t scl;
    const char* label;
};

static const i2c_pin_pair_t KNOWN_I2C_PINS[] = {
    { 8,  7,  "35BC main"      },  /* 35BC: touch+IMU+RTC+PMIC+audio+camera */
    { 8,  9,  "43C-BOX touch"  },  /* 43C-BOX: GT911 touch + CH422G */
    {15, 14,  "1.8 main"       },  /* 1.8: touch+IMU+RTC+PMIC+audio+TCA9554 */
    {40, 39,  "191M main"      },  /* 191M: touch+IMU */
    {47, 48,  "349/241B main"  },  /* 349 sensor bus, 241B main */
    {17, 18,  "349 touch"      },  /* 349 dedicated touch bus */
};
static const int NUM_I2C_PAIRS = sizeof(KNOWN_I2C_PINS) / sizeof(KNOWN_I2C_PINS[0]);

/* Known I2C peripheral addresses */
#define ADDR_ES8311     0x18    /* Audio codec */
#define ADDR_TCA9554    0x20    /* IO expander (349, 1.8) */
#define ADDR_AXP2101    0x34    /* PMIC (35BC, 1.8) */
#define ADDR_FT_TOUCH   0x38    /* FT3168/FT6336 touch (1.8, 191M, 241B) */
#define ADDR_AXS_TOUCH  0x3B    /* AXS15231B touch (349, 35BC) */
#define ADDR_PCF85063   0x51    /* RTC */
#define ADDR_GT911_ALT  0x5D    /* GT911 touch alt address */
#define ADDR_QMI8658    0x6B    /* IMU */

/* ========================================================================== */
/* Board name table                                                           */
/* ========================================================================== */

static const char* BOARD_NAMES[] = {
    "Unknown",
    "ESP32-S3-Touch-LCD-3.49",
    "ESP32-S3-Touch-LCD-3.5B-C",
    "ESP32-S3-Touch-LCD-4.3C-BOX",
    "ESP32-S3-Touch-AMOLED-2.41-B",
    "ESP32-S3-Touch-AMOLED-1.8",
    "ESP32-S3-AMOLED-1.91-M",
};

const char* board_id_to_name(board_id_t id) {
    if (id >= 0 && id < BOARD_ID_COUNT) return BOARD_NAMES[id];
    return "Unknown";
}

/* ========================================================================== */
/* Compiled board ID (from build flags)                                       */
/* ========================================================================== */

static board_id_t get_compiled_board_id(void) {
#if defined(BOARD_TOUCH_LCD_349)
    return BOARD_ID_TOUCH_LCD_349;
#elif defined(BOARD_TOUCH_LCD_35BC)
    return BOARD_ID_TOUCH_LCD_35BC;
#elif defined(BOARD_TOUCH_LCD_43C_BOX)
    return BOARD_ID_TOUCH_LCD_43C_BOX;
#elif defined(BOARD_TOUCH_AMOLED_241B)
    return BOARD_ID_TOUCH_AMOLED_241B;
#elif defined(BOARD_TOUCH_AMOLED_18)
    return BOARD_ID_TOUCH_AMOLED_18;
#elif defined(BOARD_AMOLED_191M)
    return BOARD_ID_AMOLED_191M;
#elif defined(BOARD_UNIVERSAL)
    return BOARD_ID_UNKNOWN;  /* Universal: no compiled board, detect at runtime */
#else
    return BOARD_ID_UNKNOWN;
#endif
}

/* ========================================================================== */
/* I2C scan helper                                                            */
/* ========================================================================== */

/* ========================================================================== */
/* Bit-bang I2C scan — zero driver dependencies, no conflicts                 */
/* ========================================================================== */

/* I2C timing (~100kHz): half-period ~5us */
#define BB_DELAY_US  5

static gpio_num_t _bb_sda, _bb_scl;

static inline void bb_sda_high(void) { gpio_set_direction(_bb_sda, GPIO_MODE_INPUT); }
static inline void bb_sda_low(void)  { gpio_set_direction(_bb_sda, GPIO_MODE_OUTPUT); gpio_set_level(_bb_sda, 0); }
static inline void bb_scl_high(void) { gpio_set_direction(_bb_scl, GPIO_MODE_INPUT); }
static inline void bb_scl_low(void)  { gpio_set_direction(_bb_scl, GPIO_MODE_OUTPUT); gpio_set_level(_bb_scl, 0); }
static inline int  bb_sda_read(void) { return gpio_get_level(_bb_sda); }

static void bb_start(void) {
    bb_sda_high(); esp_rom_delay_us(BB_DELAY_US);
    bb_scl_high(); esp_rom_delay_us(BB_DELAY_US);
    bb_sda_low();  esp_rom_delay_us(BB_DELAY_US);
    bb_scl_low();  esp_rom_delay_us(BB_DELAY_US);
}

static void bb_stop(void) {
    bb_sda_low();  esp_rom_delay_us(BB_DELAY_US);
    bb_scl_high(); esp_rom_delay_us(BB_DELAY_US);
    bb_sda_high(); esp_rom_delay_us(BB_DELAY_US);
}

/* Send one byte, return ACK (0) or NACK (1) */
static int bb_write_byte(uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        if (byte & (1 << i)) bb_sda_high(); else bb_sda_low();
        esp_rom_delay_us(BB_DELAY_US);
        bb_scl_high(); esp_rom_delay_us(BB_DELAY_US);
        bb_scl_low();  esp_rom_delay_us(BB_DELAY_US);
    }
    /* Read ACK */
    bb_sda_high();
    esp_rom_delay_us(BB_DELAY_US);
    bb_scl_high();
    esp_rom_delay_us(BB_DELAY_US);
    int ack = bb_sda_read();
    bb_scl_low();
    esp_rom_delay_us(BB_DELAY_US);
    return ack;  /* 0 = ACK, 1 = NACK */
}

static void bb_init_pins(uint8_t sda, uint8_t scl) {
    _bb_sda = (gpio_num_t)sda;
    _bb_scl = (gpio_num_t)scl;

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << sda) | (1ULL << scl);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
}

static void bb_release_pins(uint8_t sda, uint8_t scl) {
    gpio_reset_pin((gpio_num_t)sda);
    gpio_reset_pin((gpio_num_t)scl);
}

/**
 * Probe a single I2C address via bit-bang.
 * Sends START + (addr << 1 | 0) + STOP, returns true if ACK received.
 */
static bool bb_probe(uint8_t addr) {
    bb_start();
    int ack = bb_write_byte((addr << 1) | 0);  /* Write mode */
    bb_stop();
    return (ack == 0);
}

/**
 * Scan a single I2C bus on the given SDA/SCL pins using bit-bang.
 * No driver dependencies — uses raw GPIO only.
 */
static int scan_i2c_bus(uint8_t sda, uint8_t scl, fp_i2c_bus_t* bus) {
    bus->sda = sda;
    bus->scl = scl;
    bus->count = 0;

    bb_init_pins(sda, scl);

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (bb_probe(addr) && bus->count < FP_MAX_I2C_DEVICES) {
            bus->devices[bus->count++] = addr;
        }
    }

    bb_release_pins(sda, scl);
    return bus->count;
}

/* ========================================================================== */
/* Check if a bus has a specific address                                      */
/* ========================================================================== */

static bool bus_has_addr(const fp_i2c_bus_t* bus, uint8_t addr) {
    for (int i = 0; i < bus->count; i++) {
        if (bus->devices[i] == addr) return true;
    }
    return false;
}

/* ========================================================================== */
/* Board matching logic                                                       */
/* ========================================================================== */

/**
 * Match fingerprint against known board profiles.
 *
 * Decision tree based on which I2C pin pairs have devices and which
 * addresses respond. Each board has a unique combination:
 *
 *   349:     Devices on (47,48) AND (17,18) — only board with 2 active buses
 *   35BC:    Devices on (8,7) with PMIC @ 0x34
 *   43C-BOX: Devices on (8,9) — unique SCL=9
 *   241B:    Devices on (47,48) only, touch @ 0x38, no PMIC, no TCA9554
 *   1.8:     Devices on (15,14) with PMIC @ 0x34
 *   191M:    Devices on (40,39) — unique pin pair, minimal peripherals
 */
static void match_board(board_fingerprint_t* fp) {
    fp->detected = BOARD_ID_UNKNOWN;
    fp->confidence = 0;

    /* Index buses by their pin pair for easy lookup */
    const fp_i2c_bus_t* bus_8_7  = NULL;
    const fp_i2c_bus_t* bus_8_9  = NULL;
    const fp_i2c_bus_t* bus_15_14 = NULL;
    const fp_i2c_bus_t* bus_40_39 = NULL;
    const fp_i2c_bus_t* bus_47_48 = NULL;
    const fp_i2c_bus_t* bus_17_18 = NULL;

    for (int i = 0; i < fp->bus_count; i++) {
        fp_i2c_bus_t* b = &fp->buses[i];
        if (b->count == 0) continue;
        if      (b->sda == 8  && b->scl == 7)  bus_8_7  = b;
        else if (b->sda == 8  && b->scl == 9)  bus_8_9  = b;
        else if (b->sda == 15 && b->scl == 14) bus_15_14 = b;
        else if (b->sda == 40 && b->scl == 39) bus_40_39 = b;
        else if (b->sda == 47 && b->scl == 48) bus_47_48 = b;
        else if (b->sda == 17 && b->scl == 18) bus_17_18 = b;
    }

    /* ---- Match by unique pin pair + peripheral combination ---- */

    /* 349: Only board with devices on BOTH (47,48) and (17,18) */
    if (bus_47_48 && bus_17_18) {
        fp->detected = BOARD_ID_TOUCH_LCD_349;
        fp->confidence = 95;
        if (bus_has_addr(bus_17_18, ADDR_AXS_TOUCH)) fp->confidence = 100;
        return;
    }

    /* 35BC: Devices on (8,7) — distinguished from 43C-BOX by SCL=7 vs 9 */
    if (bus_8_7 && bus_8_7->count >= 2) {
        fp->detected = BOARD_ID_TOUCH_LCD_35BC;
        fp->confidence = 85;
        if (bus_has_addr(bus_8_7, ADDR_AXP2101)) fp->confidence = 95;
        if (bus_has_addr(bus_8_7, ADDR_AXS_TOUCH)) fp->confidence = 100;
        return;
    }

    /* 43C-BOX: Devices on (8,9) — unique SCL pin */
    if (bus_8_9 && bus_8_9->count >= 1) {
        fp->detected = BOARD_ID_TOUCH_LCD_43C_BOX;
        fp->confidence = 90;
        if (bus_8_9->count >= 2) fp->confidence = 100;
        return;
    }

    /* 1.8: Devices on (15,14) with PMIC */
    if (bus_15_14 && bus_15_14->count >= 2) {
        fp->detected = BOARD_ID_TOUCH_AMOLED_18;
        fp->confidence = 85;
        if (bus_has_addr(bus_15_14, ADDR_AXP2101)) fp->confidence = 95;
        if (bus_has_addr(bus_15_14, ADDR_TCA9554)) fp->confidence = 100;
        return;
    }

    /* 191M: Devices on (40,39) — unique pin pair */
    if (bus_40_39 && bus_40_39->count >= 1) {
        fp->detected = BOARD_ID_AMOLED_191M;
        fp->confidence = 90;
        if (bus_has_addr(bus_40_39, ADDR_QMI8658)) fp->confidence = 100;
        return;
    }

    /* 241B: Devices on (47,48) only, no second bus */
    if (bus_47_48 && !bus_17_18) {
        fp->detected = BOARD_ID_TOUCH_AMOLED_241B;
        fp->confidence = 80;
        if (bus_has_addr(bus_47_48, ADDR_QMI8658)) fp->confidence = 90;
        if (bus_has_addr(bus_47_48, ADDR_PCF85063)) fp->confidence = 95;
        return;
    }
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

static board_fingerprint_t s_fp = {};
static bool s_scanned = false;

const board_fingerprint_t* board_fingerprint_scan(void) {
    uint32_t start = millis();

    memset(&s_fp, 0, sizeof(s_fp));
    s_fp.compiled = get_compiled_board_id();
    s_fp.compiled_name = board_id_to_name(s_fp.compiled);

    /* Scan all known I2C pin pairs */
    s_fp.bus_count = 0;
    for (int i = 0; i < NUM_I2C_PAIRS && s_fp.bus_count < FP_MAX_I2C_BUSES; i++) {
        fp_i2c_bus_t* bus = &s_fp.buses[s_fp.bus_count];
        int found = scan_i2c_bus(KNOWN_I2C_PINS[i].sda, KNOWN_I2C_PINS[i].scl, bus);

        /* Extract peripheral flags from any bus that has devices */
        if (found > 0) {
            if (bus_has_addr(bus, ADDR_QMI8658))    s_fp.has_imu = true;
            if (bus_has_addr(bus, ADDR_PCF85063))   s_fp.has_rtc = true;
            if (bus_has_addr(bus, ADDR_AXP2101))    s_fp.has_pmic = true;
            if (bus_has_addr(bus, ADDR_AXS_TOUCH))  s_fp.has_touch_3b = true;
            if (bus_has_addr(bus, ADDR_FT_TOUCH))   s_fp.has_touch_38 = true;
            if (bus_has_addr(bus, ADDR_TCA9554))    s_fp.has_tca9554 = true;
            if (bus_has_addr(bus, ADDR_ES8311))      s_fp.has_audio = true;
        }

        s_fp.bus_count++;
    }

    /* Match against known board profiles */
    match_board(&s_fp);

    s_fp.detected_name = board_id_to_name(s_fp.detected);
    s_fp.match = (s_fp.detected == s_fp.compiled);

    s_fp.scan_time_ms = millis() - start;
    s_scanned = true;

    return &s_fp;
}

const board_fingerprint_t* board_fingerprint_get(void) {
    return s_scanned ? &s_fp : NULL;
}

void board_fingerprint_print(const board_fingerprint_t* fp) {
    if (!fp) return;

    Serial.printf("[fingerprint] Scan completed in %lums\n", (unsigned long)fp->scan_time_ms);
    Serial.printf("[fingerprint] Detected: %s (confidence %d%%)\n",
                  fp->detected_name, fp->confidence);
    Serial.printf("[fingerprint] Compiled: %s\n", fp->compiled_name);

    if (!fp->match) {
        Serial.printf("[fingerprint] *** MISMATCH: firmware is for %s but hardware is %s ***\n",
                      fp->compiled_name, fp->detected_name);
    }

    /* Print peripheral summary */
    Serial.printf("[fingerprint] Peripherals:");
    if (fp->has_imu)      Serial.printf(" IMU");
    if (fp->has_rtc)      Serial.printf(" RTC");
    if (fp->has_pmic)     Serial.printf(" PMIC");
    if (fp->has_touch_3b) Serial.printf(" Touch(0x3B)");
    if (fp->has_touch_38) Serial.printf(" Touch(0x38)");
    if (fp->has_tca9554)  Serial.printf(" TCA9554");
    if (fp->has_audio)    Serial.printf(" Audio");
    Serial.printf("\n");

    /* Print raw I2C scan data */
    for (int i = 0; i < fp->bus_count; i++) {
        const fp_i2c_bus_t* b = &fp->buses[i];
        if (b->count == 0) continue;
        Serial.printf("[fingerprint] I2C(%d,%d): ", b->sda, b->scl);
        for (int j = 0; j < b->count; j++) {
            Serial.printf("0x%02X ", b->devices[j]);
        }
        Serial.printf("(%d devices)\n", b->count);
    }
}
