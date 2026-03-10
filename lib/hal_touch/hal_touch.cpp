// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.
//
// Unified Touch HAL — uses legacy I2C driver (compatible with WiFi stack).
#include "hal_touch.h"

#ifdef SIMULATOR

bool TouchHAL::init() {
    _driver = FT3168;
    return true;
}

bool TouchHAL::isTouched() { return false; }
bool TouchHAL::read(uint16_t &x, uint16_t &y) { return false; }
uint8_t TouchHAL::getPoints(TouchPoint *points, uint8_t maxPoints) { return 0; }

#else // ESP32

#include "driver/i2c.h"
#include <esp_log.h>
#include <driver/gpio.h>

#if __has_include("hal_diag.h")
#include "hal_diag.h"
#define TOUCH_HAS_DIAG 1
#else
#define TOUCH_HAS_DIAG 0
#endif

static const char* TAG = "hal_touch";

// I2C port used for touch (legacy driver)
static i2c_port_t s_i2c_port = I2C_NUM_0;
static bool s_i2c_initialized = false;

static bool ensure_i2c_bus() {
    if (s_i2c_initialized) return true;

#if defined(TOUCH_SDA) && defined(TOUCH_SCL)
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = (gpio_num_t)TOUCH_SDA;
    conf.scl_io_num = (gpio_num_t)TOUCH_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 400000;

    esp_err_t ret = i2c_param_config(s_i2c_port, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = i2c_driver_install(s_i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return false;
    }
    s_i2c_initialized = true;
    return true;
#else
    return false;
#endif
}

static bool probe_addr(uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret == ESP_OK;
}

bool TouchHAL::init() {
    if (!ensure_i2c_bus()) return false;

#if defined(TOUCH_INT) && TOUCH_INT >= 0
    _int_pin = TOUCH_INT;
    gpio_set_direction((gpio_num_t)_int_pin, GPIO_MODE_INPUT);
#endif

#if defined(BOARD_TOUCH_LCD_43C_BOX)
    static const uint8_t gt_addrs[] = {0x5D, 0x14};
    for (int i = 0; i < 2; i++) {
        if (probe_addr(gt_addrs[i])) {
            _addr = gt_addrs[i];
            break;
        }
    }
    if (_addr == 0) return false;
    _driver = GT911;
    ESP_LOGI(TAG, "GT911 at 0x%02X", _addr);
    return true;

#elif defined(BOARD_TOUCH_LCD_35BC) || defined(BOARD_TOUCH_LCD_349)
#if defined(TOUCH_I2C_ADDR)
    _addr = TOUCH_I2C_ADDR;
#else
    _addr = 0x3B;
#endif
    if (!probe_addr(_addr)) return false;
    _driver = AXS15231B_TOUCH;
    ESP_LOGI(TAG, "AXS15231B at 0x%02X", _addr);
    return true;

#elif defined(BOARD_TOUCH_AMOLED_241B)
#if defined(TOUCH_I2C_ADDR)
    _addr = TOUCH_I2C_ADDR;
#else
    _addr = 0x38;
#endif
    if (!probe_addr(_addr)) return false;
    _driver = FT6336;
    ESP_LOGI(TAG, "FT6336 at 0x%02X", _addr);
    return true;

#elif defined(BOARD_AMOLED_191M) || defined(BOARD_TOUCH_AMOLED_18)
#if defined(TOUCH_I2C_ADDR)
    _addr = TOUCH_I2C_ADDR;
#else
    _addr = 0x38;
#endif
    if (!probe_addr(_addr)) return false;
    _driver = FT3168;
    ESP_LOGI(TAG, "FT3168 at 0x%02X", _addr);
    return true;

#else
    ESP_LOGW(TAG, "No touch driver for this board");
    return false;
#endif
}

bool TouchHAL::isTouched() {
    if (_driver == NONE) return false;
    if (_int_pin >= 0) {
        return gpio_get_level((gpio_num_t)_int_pin) == 0;
    }
    if (_driver == GT911) {
        uint8_t status = 0;
        i2c_read_reg16(0x814E, &status, 1);
        return (status & 0x80) && (status & 0x0F) > 0;
    }
    uint8_t numPoints = 0;
    i2c_read_reg(0x02, &numPoints, 1);
    return (numPoints & 0x0F) > 0;
}

bool TouchHAL::read(uint16_t &x, uint16_t &y) {
    switch (_driver) {
        case FT3168:
        case FT6336:         return ft_read(x, y);
        case GT911:          return gt911_read(x, y);
        case AXS15231B_TOUCH: return axs_read(x, y);
        default:             return false;
    }
}

uint8_t TouchHAL::getPoints(TouchPoint *points, uint8_t maxPoints) {
    if (_driver == NONE || maxPoints == 0) return 0;
    uint16_t x, y;
    if (read(x, y)) {
        points[0].x = x;
        points[0].y = y;
        return 1;
    }
    return 0;
}

bool TouchHAL::ft_read(uint16_t &x, uint16_t &y) {
    uint8_t buf[5];
    if (!i2c_read_reg(0x02, buf, 5)) return false;
    uint8_t numPoints = buf[0] & 0x0F;
    if (numPoints == 0) return false;
    x = ((buf[1] & 0x0F) << 8) | buf[2];
    y = ((buf[3] & 0x0F) << 8) | buf[4];
    return true;
}

bool TouchHAL::gt911_read(uint16_t &x, uint16_t &y) {
    uint8_t status = 0;
    if (!i2c_read_reg16(0x814E, &status, 1)) return false;
    if (!(status & 0x80)) return false;
    uint8_t numPoints = status & 0x0F;
    if (numPoints == 0) {
        i2c_write_reg16(0x814E, 0);
        return false;
    }
    uint8_t buf[4];
    if (!i2c_read_reg16(0x8150, buf, 4)) return false;
    x = buf[0] | (buf[1] << 8);
    y = buf[2] | (buf[3] << 8);
    i2c_write_reg16(0x814E, 0);
    return true;
}

bool TouchHAL::axs_read(uint16_t &x, uint16_t &y) {
    uint8_t buf[8];
    if (!i2c_read_reg(0x00, buf, 8)) return false;
    if (buf[0] == 0xFF || (buf[1] == 0xFF && buf[2] == 0xFF)) return false;
    x = ((buf[2] & 0x0F) << 8) | buf[3];
    y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

// --- I2C helpers using legacy driver API ---

bool TouchHAL::i2c_read_reg(uint8_t reg, uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret == ESP_OK;
}

bool TouchHAL::i2c_write_reg(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret == ESP_OK;
}

bool TouchHAL::i2c_read_reg16(uint16_t reg, uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (uint8_t)(reg >> 8), true);
    i2c_master_write_byte(cmd, (uint8_t)(reg & 0xFF), true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret == ESP_OK;
}

bool TouchHAL::i2c_write_reg16(uint16_t reg, uint8_t val) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (uint8_t)(reg >> 8), true);
    i2c_master_write_byte(cmd, (uint8_t)(reg & 0xFF), true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret == ESP_OK;
}

#endif // SIMULATOR
