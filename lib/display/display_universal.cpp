/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file display_universal.cpp
 * @brief Runtime display initialization for universal firmware.
 *
 * Complete replacement for display.cpp in universal builds. Implements
 * the full display.h API but dispatches at runtime via board_config_t
 * instead of compile-time #if defined(BOARD_*).
 *
 * Includes ALL display drivers (AXS15231B, SH8601, RGB) simultaneously.
 * TCA9554 reset uses bit-bang I2C to avoid driver conflicts.
 *
 * Build: define BOARD_UNIVERSAL and exclude display.cpp from build_src_filter.
 */

#if defined(BOARD_UNIVERSAL)

#include "display.h"
#include "display_universal.h"
#include "backlight.h"
#include "board_config.h"

#include "esp_rom_sys.h"
#include "drivers/esp_lcd_axs15231b.h"
#include "drivers/esp_lcd_sh8601.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_uni";

/* ========================================================================== */
/* Module state                                                               */
/* ========================================================================== */

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static SemaphoreHandle_t s_flush_done = NULL;
static bool s_initialized = false;
static const board_config_t* s_active_cfg = NULL;

static display_health_t s_health = {
    .initialized = false,
    .verified = false,
    .board_name = "Universal",
    .driver = "Unknown",
    .width = 0,
    .height = 0,
    .expected_id = 0,
    .actual_id = 0,
    .frame_count = 0,
};

/* ========================================================================== */
/* DMA completion callback                                                    */
/* ========================================================================== */

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    if (s_flush_done) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_flush_done, &woken);
        return woken == pdTRUE;
    }
    return false;
}

/* ========================================================================== */
/* Bit-bang I2C for TCA9554 reset                                             */
/* ========================================================================== */

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

static int bb_write_byte(uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        if (byte & (1 << i)) bb_sda_high(); else bb_sda_low();
        esp_rom_delay_us(BB_DELAY_US);
        bb_scl_high(); esp_rom_delay_us(BB_DELAY_US);
        bb_scl_low();  esp_rom_delay_us(BB_DELAY_US);
    }
    bb_sda_high();
    esp_rom_delay_us(BB_DELAY_US);
    bb_scl_high();
    esp_rom_delay_us(BB_DELAY_US);
    int ack = bb_sda_read();
    bb_scl_low();
    esp_rom_delay_us(BB_DELAY_US);
    return ack;
}

static int bb_read_byte(bool send_ack) {
    uint8_t byte = 0;
    bb_sda_high();
    for (int i = 7; i >= 0; i--) {
        esp_rom_delay_us(BB_DELAY_US);
        bb_scl_high();
        esp_rom_delay_us(BB_DELAY_US);
        if (bb_sda_read()) byte |= (1 << i);
        bb_scl_low();
    }
    if (send_ack) bb_sda_low(); else bb_sda_high();
    esp_rom_delay_us(BB_DELAY_US);
    bb_scl_high(); esp_rom_delay_us(BB_DELAY_US);
    bb_scl_low();  esp_rom_delay_us(BB_DELAY_US);
    bb_sda_high();
    return byte;
}

static void bb_init(int sda, int scl) {
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

static void bb_release(int sda, int scl) {
    gpio_reset_pin((gpio_num_t)sda);
    gpio_reset_pin((gpio_num_t)scl);
}

static esp_err_t tca9554_reset_bitbang(int sda, int scl, uint8_t addr, int rst_pin) {
    bb_init(sda, scl);

    /* Read current output register (0x01) */
    bb_start();
    if (bb_write_byte((addr << 1) | 0) != 0) {
        bb_stop(); bb_release(sda, scl);
        ESP_LOGE(TAG, "TCA9554 @ 0x%02X not responding on (%d,%d)", addr, sda, scl);
        return ESP_ERR_NOT_FOUND;
    }
    bb_write_byte(0x01);
    bb_stop();

    bb_start();
    if (bb_write_byte((addr << 1) | 1) != 0) {
        bb_stop(); bb_release(sda, scl);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t reg_val = bb_read_byte(false);
    bb_stop();

    /* Pull reset pin LOW */
    bb_start();
    bb_write_byte((addr << 1) | 0);
    bb_write_byte(0x01);
    bb_write_byte(reg_val & ~(1 << rst_pin));
    bb_stop();

    vTaskDelay(pdMS_TO_TICKS(20));

    /* Pull reset pin HIGH */
    bb_start();
    bb_write_byte((addr << 1) | 0);
    bb_write_byte(0x01);
    bb_write_byte(reg_val | (1 << rst_pin));
    bb_stop();

    vTaskDelay(pdMS_TO_TICKS(120));

    bb_release(sda, scl);
    ESP_LOGI(TAG, "TCA9554 display reset complete (SDA=%d SCL=%d)", sda, scl);
    return ESP_OK;
}

/* ========================================================================== */
/* QSPI display init                                                          */
/* ========================================================================== */

static esp_err_t init_qspi_display(const board_config_t* cfg)
{
    esp_err_t ret;

    s_flush_done = xSemaphoreCreateBinary();

    /* Initialize SPI bus */
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = cfg->qspi.clk;
    bus_cfg.data0_io_num = cfg->qspi.d0;
    bus_cfg.data1_io_num = cfg->qspi.d1;
    bus_cfg.data2_io_num = cfg->qspi.d2;
    bus_cfg.data3_io_num = cfg->qspi.d3;
    bus_cfg.max_transfer_sz = cfg->width * 64 * sizeof(uint16_t);
    bus_cfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;

    ret = spi_bus_initialize((spi_host_device_t)cfg->qspi.host, &bus_cfg, SPI_DMA_CH_AUTO);
    ESP_RETURN_ON_ERROR(ret, TAG, "SPI bus init failed");

    /* Configure panel IO */
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = cfg->qspi.cs;
    io_cfg.dc_gpio_num = -1;
    io_cfg.spi_mode = cfg->qspi.mode;
    io_cfg.pclk_hz = cfg->qspi.clk_hz;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = on_color_trans_done;
    io_cfg.lcd_cmd_bits = cfg->qspi.cmd_bits;
    io_cfg.lcd_param_bits = cfg->qspi.param_bits;
    io_cfg.flags.quad_mode = true;

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)cfg->qspi.host, &io_cfg, &s_panel_io);
    ESP_RETURN_ON_ERROR(ret, TAG, "Panel IO init failed");

    /* Create panel with the correct driver */
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = cfg->rst_gpio;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.flags.reset_active_high = cfg->rst_active_high;

    if (cfg->display_type == DISPLAY_TYPE_AXS15231B) {
        axs15231b_vendor_config_t vendor_cfg = {};
        vendor_cfg.init_cmds = (const axs15231b_lcd_init_cmd_t*)cfg->init_cmds;
        vendor_cfg.init_cmds_size = cfg->init_cmds_count;
        vendor_cfg.flags.use_qspi_interface = 1;
        panel_cfg.vendor_config = &vendor_cfg;
        ret = esp_lcd_new_panel_axs15231b(s_panel_io, &panel_cfg, &s_panel);
    } else {
        sh8601_vendor_config_t vendor_cfg = {};
        vendor_cfg.init_cmds = (const sh8601_lcd_init_cmd_t*)cfg->init_cmds;
        vendor_cfg.init_cmds_size = cfg->init_cmds_count;
        vendor_cfg.flags.use_qspi_interface = 1;
        panel_cfg.vendor_config = &vendor_cfg;
        ret = esp_lcd_new_panel_sh8601(s_panel_io, &panel_cfg, &s_panel);
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "Panel create failed (%s)", cfg->driver_name);

    /* TCA9554 display reset (35BC, 1.8) */
    if (cfg->needs_tca9554_reset) {
        esp_err_t tca_ret = tca9554_reset_bitbang(
            cfg->tca9554_sda, cfg->tca9554_scl, cfg->tca9554_addr, 1);
        if (tca_ret != ESP_OK) {
            ESP_LOGW(TAG, "TCA9554 reset failed (non-fatal): %s", esp_err_to_name(tca_ret));
        }
    }

    /* Manual GPIO reset (349) */
    if (cfg->rst_gpio_manual >= 0) {
        gpio_config_t rst_conf = {};
        rst_conf.mode = GPIO_MODE_OUTPUT;
        rst_conf.pin_bit_mask = (1ULL << cfg->rst_gpio_manual);
        rst_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&rst_conf);

        gpio_set_level((gpio_num_t)cfg->rst_gpio_manual, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        gpio_set_level((gpio_num_t)cfg->rst_gpio_manual, 0);
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_set_level((gpio_num_t)cfg->rst_gpio_manual, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        ESP_LOGI(TAG, "Manual GPIO reset complete (pin %d)", cfg->rst_gpio_manual);
    } else {
        ret = esp_lcd_panel_reset(s_panel);
        ESP_RETURN_ON_ERROR(ret, TAG, "Panel reset failed");
    }

    ret = esp_lcd_panel_init(s_panel);
    ESP_RETURN_ON_ERROR(ret, TAG, "Panel init failed");

    esp_lcd_panel_set_gap(s_panel, cfg->x_gap, cfg->y_gap);
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_LOGI(TAG, "QSPI display initialized: %dx%d (%s)", cfg->width, cfg->height, cfg->driver_name);
    return ESP_OK;
}

/* ========================================================================== */
/* RGB display init                                                           */
/* ========================================================================== */

static esp_err_t init_rgb_display(const board_config_t* cfg)
{
    const rgb_config_t* rgb = cfg->rgb;
    if (!rgb) {
        ESP_LOGE(TAG, "RGB config is NULL for board %s", cfg->name);
        return ESP_ERR_INVALID_ARG;
    }

    esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = (uint32_t)rgb->pclk_hz,
            .h_res = (uint32_t)cfg->width,
            .v_res = (uint32_t)cfg->height,
            .hsync_pulse_width = (uint32_t)rgb->hsync_pulse,
            .hsync_back_porch = (uint32_t)rgb->hsync_bp,
            .hsync_front_porch = (uint32_t)rgb->hsync_fp,
            .vsync_pulse_width = (uint32_t)rgb->vsync_pulse,
            .vsync_back_porch = (uint32_t)rgb->vsync_bp,
            .vsync_front_porch = (uint32_t)rgb->vsync_fp,
            .flags = {
                .pclk_active_neg = rgb->pclk_active_neg,
            },
        },
        .data_width = (size_t)rgb->data_width,
        .bits_per_pixel = 16,
        .num_fbs = (size_t)rgb->num_fbs,
        .bounce_buffer_size_px = (size_t)(cfg->width * rgb->bounce_buf_rows),
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = rgb->hsync,
        .vsync_gpio_num = rgb->vsync,
        .de_gpio_num = rgb->de,
        .pclk_gpio_num = rgb->pclk,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            rgb->data[0],  rgb->data[1],  rgb->data[2],  rgb->data[3],
            rgb->data[4],  rgb->data[5],  rgb->data[6],  rgb->data[7],
            rgb->data[8],  rgb->data[9],  rgb->data[10], rgb->data[11],
            rgb->data[12], rgb->data[13], rgb->data[14], rgb->data[15],
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };

    esp_err_t ret = esp_lcd_new_rgb_panel(&rgb_cfg, &s_panel);
    ESP_RETURN_ON_ERROR(ret, TAG, "RGB panel create failed");

    ret = esp_lcd_panel_reset(s_panel);
    ESP_RETURN_ON_ERROR(ret, TAG, "RGB panel reset failed");

    ret = esp_lcd_panel_init(s_panel);
    ESP_RETURN_ON_ERROR(ret, TAG, "RGB panel init failed");

    ESP_LOGI(TAG, "RGB display initialized: %dx%d (%s)", cfg->width, cfg->height, cfg->driver_name);
    return ESP_OK;
}

/* ========================================================================== */
/* Panel ID readback                                                          */
/* ========================================================================== */

static esp_err_t read_panel_id(display_type_t type, uint32_t* out_id)
{
    if (!s_panel_io || !out_id) return ESP_ERR_INVALID_STATE;

    uint8_t id_buf[3] = {0};
    int cmd;

    if (type == DISPLAY_TYPE_AXS15231B) {
        cmd = (0x0B << 24) | (0x04 << 8);
    } else {
        cmd = (0x03 << 24) | (0x04 << 8);
    }

    esp_err_t ret = esp_lcd_panel_io_rx_param(s_panel_io, cmd, id_buf, 3);
    if (ret == ESP_OK) {
        *out_id = ((uint32_t)id_buf[0] << 16) | ((uint32_t)id_buf[1] << 8) | id_buf[2];
    }
    return ret;
}

static void display_verify(const board_config_t* cfg)
{
    if (cfg->display_type != DISPLAY_TYPE_RGB) {
        uint32_t actual_id = 0;
        esp_err_t ret = read_panel_id(cfg->display_type, &actual_id);
        s_health.actual_id = actual_id;

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Panel ID readback FAILED (%s)", esp_err_to_name(ret));
            s_health.verified = false;
        } else if (actual_id == 0x000000) {
            ESP_LOGW(TAG, "Panel ID is 0x000000 — no panel on QSPI bus");
            s_health.verified = false;
        } else if (actual_id == cfg->expected_chip_id) {
            ESP_LOGI(TAG, "Panel ID verified: 0x%06lX (%s)",
                     (unsigned long)actual_id, cfg->driver_name);
            s_health.verified = true;
        } else {
            ESP_LOGW(TAG, "Panel ID MISMATCH: expected 0x%06lX (%s), got 0x%06lX",
                     (unsigned long)cfg->expected_chip_id, cfg->driver_name,
                     (unsigned long)actual_id);
            s_health.verified = false;
        }
    } else {
        s_health.verified = (s_panel != NULL);
        if (s_health.verified) {
            ESP_LOGI(TAG, "RGB panel verified (DMA buffer for %dx%d)", cfg->width, cfg->height);
        }
    }
}

/* ========================================================================== */
/* Public API — implements display.h                                          */
/* ========================================================================== */

esp_err_t display_init(void)
{
    /* In universal mode, display_init() is a no-op.
     * Use display_init_universal(cfg) instead. */
    ESP_LOGW(TAG, "display_init() called in universal mode — use display_init_universal()");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t display_init_universal(const board_config_t* cfg)
{
    if (!cfg) {
        ESP_LOGE(TAG, "Board config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        ESP_LOGW(TAG, "Display already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing %s (%s, %dx%d)",
             cfg->name, cfg->driver_name, cfg->width, cfg->height);

    s_active_cfg = cfg;
    esp_err_t ret;

    if (cfg->display_type == DISPLAY_TYPE_RGB) {
        ret = init_rgb_display(cfg);
    } else {
        ret = init_qspi_display(cfg);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Backlight */
    if (cfg->bl_gpio >= 0) {
        ret = backlight_init(cfg->bl_gpio, cfg->bl_pwm_channel, cfg->bl_active_high);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Backlight init failed (non-fatal): %s", esp_err_to_name(ret));
        }
        backlight_set_brightness(cfg->bl_gpio, cfg->bl_pwm_channel, 255, cfg->bl_active_high);
        ESP_LOGI(TAG, "Backlight ON (GPIO %d)", cfg->bl_gpio);
    } else {
        ESP_LOGI(TAG, "No direct backlight GPIO (AMOLED or IO expander)");
    }

    s_initialized = true;

    /* Populate health */
    s_health.initialized = true;
    s_health.board_name = cfg->name;
    s_health.driver = cfg->driver_name;
    s_health.width = cfg->width;
    s_health.height = cfg->height;
    s_health.expected_id = cfg->expected_chip_id;

    display_verify(cfg);

    ESP_LOGI(TAG, "Display ready: %dx%d [%s, verified=%s]",
             cfg->width, cfg->height, cfg->name,
             s_health.verified ? "YES" : "NO");
    return ESP_OK;
}

esp_lcd_panel_handle_t display_get_panel(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t display_get_panel_io(void)
{
    return s_panel_io;
}

int display_get_width(void)
{
    return s_active_cfg ? s_active_cfg->width : 0;
}

int display_get_height(void)
{
    return s_active_cfg ? s_active_cfg->height : 0;
}

SemaphoreHandle_t display_get_flush_semaphore(void)
{
    return s_flush_done;
}

const display_health_t* display_get_health(void)
{
    return &s_health;
}

void display_count_frame(void)
{
    s_health.frame_count++;
}

void display_set_brightness(uint8_t brightness)
{
    if (!s_active_cfg) return;

    if (s_active_cfg->bl_gpio >= 0) {
        backlight_set_brightness(s_active_cfg->bl_gpio, s_active_cfg->bl_pwm_channel,
                                 brightness, s_active_cfg->bl_active_high);
    } else if (s_active_cfg->display_type != DISPLAY_TYPE_RGB && s_panel_io) {
        /* AMOLED: send brightness via 0x51 command */
        uint8_t val = brightness;
        int cmd = (0x02 << 24) | (0x51 << 8);
        esp_lcd_panel_io_tx_param(s_panel_io, cmd, &val, 1);
    }
}

#endif /* BOARD_UNIVERSAL */
