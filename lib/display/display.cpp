/*
 * SPDX-FileCopyrightText: 2024-2026 Valpatel
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file display.cpp
 * @brief Board-dispatch display initialization using esp_lcd framework.
 *
 * Uses compile-time board selection (#if defined(BOARD_*)) to call the correct
 * panel driver and init sequence. This is the single point of display init for
 * the entire project.
 */

/* In universal builds, display_universal.cpp provides all display.h symbols */
#if defined(BOARD_UNIVERSAL)
/* Skip this entire file — display_universal.cpp is the replacement */
#else

#include "display.h"
#include "backlight.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

/* ========================================================================== */
/* Include the correct board config header based on build flags               */
/* ========================================================================== */

#if defined(BOARD_TOUCH_LCD_349)
    #include "boards/board_display_349.h"
    #include "drivers/esp_lcd_axs15231b.h"
    #define DISPLAY_DRIVER_AXS15231B
#elif defined(BOARD_TOUCH_LCD_35BC)
    #include "boards/board_display_35bc.h"
    #include "drivers/esp_lcd_axs15231b.h"
    #define DISPLAY_DRIVER_AXS15231B
#elif defined(BOARD_TOUCH_AMOLED_241B)
    #include "boards/board_display_241b.h"
    #include "drivers/esp_lcd_sh8601.h"
    #define DISPLAY_DRIVER_SH8601
#elif defined(BOARD_TOUCH_AMOLED_18)
    #include "boards/board_display_18.h"
    #include "drivers/esp_lcd_sh8601.h"
    #define DISPLAY_DRIVER_SH8601
#elif defined(BOARD_AMOLED_191M)
    #include "boards/board_display_191m.h"
    #include "drivers/esp_lcd_sh8601.h"
    #define DISPLAY_DRIVER_SH8601
#elif defined(BOARD_TOUCH_LCD_43C_BOX)
    #include "boards/board_display_43c.h"
    #include "esp_lcd_panel_rgb.h"
    #define DISPLAY_DRIVER_RGB
#else
    #error "No board defined. Define one of: BOARD_TOUCH_LCD_349, BOARD_TOUCH_LCD_35BC, BOARD_TOUCH_AMOLED_241B, BOARD_TOUCH_AMOLED_18, BOARD_AMOLED_191M, BOARD_TOUCH_LCD_43C_BOX"
#endif

/* ========================================================================== */
/* Module state                                                               */
/* ========================================================================== */

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static bool s_initialized = false;

/* Display health tracking */
static display_health_t s_health = {
    .initialized = false,
    .verified = false,
    .board_name = BOARD_NAME,
    .driver = BOARD_DISPLAY_DRIVER,
    .width = BOARD_LCD_H_RES,
    .height = BOARD_LCD_V_RES,
    .expected_id = BOARD_LCD_EXPECTED_ID,
    .actual_id = 0,
    .frame_count = 0,
};

/* DMA completion semaphore — signaled by on_color_trans_done callback */
static SemaphoreHandle_t s_flush_done = NULL;

/* RGB vsync two-semaphore handshake (Espressif proven pattern):
 *   sem_gui_ready: LVGL signals "rendering done, ready to swap"
 *   sem_vsync_end: ISR signals "vsync just fired while LVGL was waiting"
 *
 * This ensures the swap happens at the NEXT vsync after rendering completes,
 * not on a stale vsync that fired while LVGL was still rendering.
 */
static SemaphoreHandle_t s_sem_gui_ready = NULL;
static SemaphoreHandle_t s_sem_vsync_end = NULL;
static bool s_is_rgb_panel = false;

/* QSPI panels: DMA completion callback */
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

#if defined(DISPLAY_DRIVER_RGB)
/* RGB panel: vsync callback — fires at the start of each vertical blanking interval.
 *
 * Two-semaphore handshake: only signal vsync_end if LVGL has signaled gui_ready.
 * This ensures the FB swap happens at the NEXT vsync after LVGL is done rendering,
 * not on a stale vsync that fired while rendering was still in progress. */
static bool IRAM_ATTR rgb_on_vsync(esp_lcd_panel_handle_t panel,
                                    const esp_lcd_rgb_panel_event_data_t *edata,
                                    void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_sem_gui_ready && xSemaphoreTakeFromISR(s_sem_gui_ready, &woken) == pdTRUE) {
        xSemaphoreGiveFromISR(s_sem_vsync_end, &woken);
    }
    return woken == pdTRUE;
}

/* RGB panel: draw buffer copied to internal FB. LVGL can now recycle its buffer. */
static bool IRAM_ATTR rgb_on_color_trans_done(esp_lcd_panel_handle_t panel,
                                               const esp_lcd_rgb_panel_event_data_t *edata,
                                               void *user_ctx)
{
    if (s_flush_done) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_flush_done, &woken);
        return woken == pdTRUE;
    }
    return false;
}
#endif /* DISPLAY_DRIVER_RGB */

/* ========================================================================== */
/* QSPI display init (AXS15231B and SH8601 boards)                           */
/* ========================================================================== */

#if defined(DISPLAY_DRIVER_AXS15231B) || defined(DISPLAY_DRIVER_SH8601)

static esp_err_t init_qspi_display(void)
{
    esp_err_t ret;

    /* ---- Create DMA completion semaphore ---- */
    s_flush_done = xSemaphoreCreateBinary();

    /* ---- Initialize SPI bus ---- */
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = LCD_QSPI_CLK;
    bus_cfg.data0_io_num = LCD_QSPI_D0;
    bus_cfg.data1_io_num = LCD_QSPI_D1;
    bus_cfg.data2_io_num = LCD_QSPI_D2;
    bus_cfg.data3_io_num = LCD_QSPI_D3;
    bus_cfg.max_transfer_sz = BOARD_LCD_H_RES * 64 * sizeof(uint16_t);  /* DMA chunk, not full frame */
    bus_cfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;

    ret = spi_bus_initialize((spi_host_device_t)BOARD_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    ESP_RETURN_ON_ERROR(ret, TAG, "SPI bus init failed");

    /* ---- Configure panel IO (QSPI) ---- */
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = LCD_QSPI_CS;
    io_cfg.dc_gpio_num = -1;                    /* No DC pin in QSPI mode */
    io_cfg.spi_mode = BOARD_LCD_SPI_MODE;
    io_cfg.pclk_hz = BOARD_LCD_PIXEL_CLK_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = on_color_trans_done;
    io_cfg.lcd_cmd_bits = BOARD_LCD_CMD_BITS;
    io_cfg.lcd_param_bits = BOARD_LCD_PARAM_BITS;
    io_cfg.flags.quad_mode = true;

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &s_panel_io);
    ESP_RETURN_ON_ERROR(ret, TAG, "Panel IO init failed");

    /* ---- Create panel ---- */
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = BOARD_LCD_RST_GPIO;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.flags.reset_active_high = BOARD_LCD_RST_ACTIVE_HIGH;

#if defined(DISPLAY_DRIVER_AXS15231B)
    axs15231b_vendor_config_t vendor_cfg = {};
    vendor_cfg.init_cmds = BOARD_LCD_INIT_CMDS;
    vendor_cfg.init_cmds_size = BOARD_LCD_INIT_CMDS_SIZE;
    vendor_cfg.flags.use_qspi_interface = 1;
    panel_cfg.vendor_config = &vendor_cfg;

    ret = esp_lcd_new_panel_axs15231b(s_panel_io, &panel_cfg, &s_panel);
#elif defined(DISPLAY_DRIVER_SH8601)
    sh8601_vendor_config_t vendor_cfg = {};
    vendor_cfg.init_cmds = BOARD_LCD_INIT_CMDS;
    vendor_cfg.init_cmds_size = BOARD_LCD_INIT_CMDS_SIZE;
    vendor_cfg.flags.use_qspi_interface = 1;
    panel_cfg.vendor_config = &vendor_cfg;

    ret = esp_lcd_new_panel_sh8601(s_panel_io, &panel_cfg, &s_panel);
#endif
    ESP_RETURN_ON_ERROR(ret, TAG, "Panel create failed");

    /* ---- Board-specific pre-init ---- */
#if defined(BOARD_TOUCH_LCD_35BC)
    ret = board_display_35bc_tca9554_reset();
    ESP_RETURN_ON_ERROR(ret, TAG, "TCA9554 display reset failed");
#endif

    /* ---- Manual GPIO reset (vendor timing: HIGH 30ms -> LOW 250ms -> HIGH 30ms) ---- */
#if defined(BOARD_TOUCH_LCD_349)
    {
        gpio_config_t rst_conf = {};
        rst_conf.mode = GPIO_MODE_OUTPUT;
        rst_conf.pin_bit_mask = (1ULL << BOARD_LCD_RST_GPIO_PIN);
        rst_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&rst_conf);

        gpio_set_level((gpio_num_t)BOARD_LCD_RST_GPIO_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        gpio_set_level((gpio_num_t)BOARD_LCD_RST_GPIO_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_set_level((gpio_num_t)BOARD_LCD_RST_GPIO_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        ESP_LOGI(TAG, "Manual GPIO reset complete (pin %d)", BOARD_LCD_RST_GPIO_PIN);
    }
#else
    /* Other boards: let the driver handle reset */
    ret = esp_lcd_panel_reset(s_panel);
    ESP_RETURN_ON_ERROR(ret, TAG, "Panel reset failed");
#endif

    ret = esp_lcd_panel_init(s_panel);
    ESP_RETURN_ON_ERROR(ret, TAG, "Panel init failed");

    /* ---- Set gap/offset ---- */
    esp_lcd_panel_set_gap(s_panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP);

    /* ---- Turn display on ---- */
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_LOGI(TAG, "QSPI display initialized: %dx%d", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

#endif /* QSPI display */

/* ========================================================================== */
/* RGB display init (4.3C-BOX)                                                */
/* ========================================================================== */

#if defined(DISPLAY_DRIVER_RGB)

static esp_err_t init_rgb_display(void)
{
    s_is_rgb_panel = true;

    /* Create synchronization semaphores for tear-free rendering.
     *
     * Two-semaphore vsync handshake (Espressif pattern):
     *   sem_gui_ready: LVGL gives this when rendering is done
     *   sem_vsync_end: on_vsync ISR gives this when vsync fires AND gui was ready
     *
     * Plus flush_done for color_trans_done synchronization.
     */
    s_flush_done = xSemaphoreCreateBinary();
    s_sem_gui_ready = xSemaphoreCreateBinary();
    s_sem_vsync_end = xSemaphoreCreateBinary();

    esp_lcd_rgb_panel_config_t rgb_cfg = board_display_43c_get_rgb_config();

    esp_err_t ret = esp_lcd_new_rgb_panel(&rgb_cfg, &s_panel);
    ESP_RETURN_ON_ERROR(ret, TAG, "RGB panel create failed");

    ret = esp_lcd_panel_reset(s_panel);
    ESP_RETURN_ON_ERROR(ret, TAG, "RGB panel reset failed");

    ret = esp_lcd_panel_init(s_panel);
    ESP_RETURN_ON_ERROR(ret, TAG, "RGB panel init failed");

    /* Register event callbacks for vsync-synchronized rendering.
     *
     * on_vsync: fires at vertical blanking — signals LVGL it's safe to
     *           start rendering the next frame into the draw buffer.
     * on_color_trans_done: fires when LVGL's draw buffer has been copied
     *           to the panel's internal FB — signals LVGL it can recycle
     *           the draw buffer.
     *
     * Together these prevent tearing: LVGL waits for vsync before
     * modifying the buffer, and waits for trans_done before recycling it.
     */
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_vsync = rgb_on_vsync;
    cbs.on_color_trans_done = rgb_on_color_trans_done;

    ret = esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register RGB event callbacks: %s (tearing may occur)",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "RGB vsync + color_trans_done callbacks registered");
    }

    ESP_LOGI(TAG, "RGB display initialized: %dx%d (double-buffered, vsync-synced)",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

#endif /* RGB display */

/* ========================================================================== */
/* Panel verification — read chip ID to confirm correct hardware              */
/* ========================================================================== */

#if defined(DISPLAY_DRIVER_AXS15231B) || defined(DISPLAY_DRIVER_SH8601)

/**
 * Read MIPI DCS register 0x04 (Read Display Identification Information).
 * Returns 3 bytes: manufacturer ID, driver version, driver ID.
 * Uses the QSPI read opcode specific to the panel driver.
 */
static esp_err_t display_read_panel_id(uint32_t *out_id)
{
    if (!s_panel_io || !out_id) return ESP_ERR_INVALID_STATE;

    uint8_t id_buf[3] = {0};

    /* Build QSPI read command: (read_opcode << 24) | (register << 8) */
#if defined(DISPLAY_DRIVER_AXS15231B)
    int cmd = (0x0B << 24) | (0x04 << 8);  /* AXS15231B read opcode */
#elif defined(DISPLAY_DRIVER_SH8601)
    int cmd = (0x03 << 24) | (0x04 << 8);  /* SH8601/RM690B0/RM67162 read opcode */
#endif

    esp_err_t ret = esp_lcd_panel_io_rx_param(s_panel_io, cmd, id_buf, 3);
    if (ret == ESP_OK) {
        *out_id = ((uint32_t)id_buf[0] << 16) | ((uint32_t)id_buf[1] << 8) | id_buf[2];
    }
    return ret;
}

#endif /* QSPI display */

static void display_verify(void)
{
#if defined(DISPLAY_DRIVER_AXS15231B) || defined(DISPLAY_DRIVER_SH8601)
    uint32_t actual_id = 0;
    esp_err_t ret = display_read_panel_id(&actual_id);

    s_health.actual_id = actual_id;

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Panel ID readback FAILED (%s) — cannot verify hardware",
                 esp_err_to_name(ret));
        s_health.verified = false;
    } else if (actual_id == 0x000000) {
        ESP_LOGW(TAG, "Panel ID is 0x000000 — no panel responding on QSPI bus");
        s_health.verified = false;
    } else if (actual_id == BOARD_LCD_EXPECTED_ID) {
        ESP_LOGI(TAG, "Panel ID verified: 0x%06lX (%s)",
                 (unsigned long)actual_id, BOARD_DISPLAY_DRIVER);
        s_health.verified = true;
    } else {
        ESP_LOGW(TAG, "Panel ID MISMATCH: expected 0x%06lX (%s), got 0x%06lX",
                 (unsigned long)BOARD_LCD_EXPECTED_ID, BOARD_DISPLAY_DRIVER,
                 (unsigned long)actual_id);
        s_health.verified = false;
    }
#elif defined(DISPLAY_DRIVER_RGB)
    /* RGB panels have no command interface — we can only verify that
     * esp_lcd_new_rgb_panel() succeeded (which is checked by init_rgb_display).
     * The panel handle itself is proof of successful DMA buffer allocation
     * for the correct resolution. Mark as verified if init succeeded. */
    s_health.verified = (s_panel != NULL);
    if (s_health.verified) {
        ESP_LOGI(TAG, "RGB panel verified (DMA buffer allocated for %dx%d)",
                 BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    }
#endif
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

esp_err_t display_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Display already initialized");
        return ESP_OK;
    }

    esp_err_t ret;

#if defined(DISPLAY_DRIVER_AXS15231B) || defined(DISPLAY_DRIVER_SH8601)
    ret = init_qspi_display();
#elif defined(DISPLAY_DRIVER_RGB)
    ret = init_rgb_display();
#endif

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ---- Initialize backlight ---- */
#if BOARD_LCD_BL_GPIO >= 0
    ret = backlight_init(BOARD_LCD_BL_GPIO, BOARD_LCD_BL_PWM_CHANNEL, BOARD_LCD_BL_ACTIVE_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Backlight init failed (non-fatal): %s", esp_err_to_name(ret));
    }
    /* Turn backlight on full */
    backlight_set_brightness(BOARD_LCD_BL_GPIO, BOARD_LCD_BL_PWM_CHANNEL, 255, BOARD_LCD_BL_ACTIVE_HIGH);
    ESP_LOGI(TAG, "Backlight ON (GPIO %d)", BOARD_LCD_BL_GPIO);
#else
    ESP_LOGI(TAG, "No direct backlight GPIO (AMOLED or IO expander)");
#endif

    s_initialized = true;
    s_health.initialized = true;

    /* Verify the correct panel is connected */
    display_verify();

    ESP_LOGI(TAG, "Display ready: %dx%d [%s, verified=%s]",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, BOARD_NAME,
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
    return BOARD_LCD_H_RES;
}

int display_get_height(void)
{
    return BOARD_LCD_V_RES;
}

SemaphoreHandle_t display_get_flush_semaphore(void)
{
    return s_flush_done;
}

SemaphoreHandle_t display_get_sem_gui_ready(void)
{
    return s_sem_gui_ready;
}

SemaphoreHandle_t display_get_sem_vsync_end(void)
{
    return s_sem_vsync_end;
}

bool display_is_rgb(void)
{
    return s_is_rgb_panel;
}

uint16_t* display_get_framebuffer(void)
{
#if defined(DISPLAY_DRIVER_RGB)
    if (!s_panel) return NULL;
    void* fb0 = NULL;
    esp_err_t err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb0);
    if (err != ESP_OK) return NULL;
    return (uint16_t*)fb0;
#else
    return NULL;  // QSPI panels have no persistent framebuffer
#endif
}

esp_err_t display_get_rgb_framebuffers(void** fb0, void** fb1)
{
#if defined(DISPLAY_DRIVER_RGB)
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    return esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, fb0, fb1);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
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
#if BOARD_LCD_BL_GPIO >= 0
    /* Direct GPIO PWM backlight (3.49, 3.5B-C) */
    backlight_set_brightness(BOARD_LCD_BL_GPIO, BOARD_LCD_BL_PWM_CHANNEL, brightness, BOARD_LCD_BL_ACTIVE_HIGH);
#elif defined(DISPLAY_DRIVER_SH8601) || defined(DISPLAY_DRIVER_AXS15231B)
    /* AMOLED: send brightness via 0x51 command */
    if (s_panel_io) {
        uint8_t val = brightness;
        /* Encode as QSPI command: (opcode << 24) | (cmd << 8) */
        int cmd = (0x02 << 24) | (0x51 << 8);
        esp_lcd_panel_io_tx_param(s_panel_io, cmd, &val, 1);
    }
#elif defined(DISPLAY_DRIVER_RGB)
    /* 4.3C-BOX: brightness via IO extension chip */
    /* TODO: Implement IO extension PWM control (I2C 0x24, register 0x05) */
    ESP_LOGW(TAG, "RGB backlight brightness control not yet implemented");
    (void)brightness;
#endif
}

#endif /* !BOARD_UNIVERSAL */
