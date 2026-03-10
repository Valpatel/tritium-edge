#include "hal_touch.h"

#ifdef SIMULATOR

// --- Simulator stubs (touch via SDL mouse) ---

bool TouchHAL::init() {
    _driver = FT3168; // pretend we have a touch controller
    return true;
}

bool TouchHAL::isTouched() {
    return false; // SDL mouse integration handled by LVGL indev
}

bool TouchHAL::read(uint16_t &x, uint16_t &y) {
    return false;
}

uint8_t TouchHAL::getPoints(TouchPoint *points, uint8_t maxPoints) {
    return 0;
}

#else // ESP32

#include "tritium_compat.h"
#include "tritium_i2c.h"

// GT911 on 43C-BOX needs reset via CH422G IO expander before I2C probe
#if defined(BOARD_TOUCH_LCD_43C_BOX) && HAS_IO_EXPANDER
static void gt911_reset_via_ch422g() {
    // CH422G config: set IO mode
    uint8_t ch422g_mode[] = { 0x01 };
    i2c0.write(0x24, ch422g_mode, 1);  // CH422G_SET_ADDR

    // Set EXIO1 (touch RST) LOW to hold GT911 in reset
    // Keep EXIO2 (backlight) HIGH
    uint8_t rst_low[] = { 0x02 };  // bit1=EXIO2(BL)=1, bit0=EXIO1(RST)=0
    i2c0.write(0x38, rst_low, 1);  // CH422G_WR_IO_ADDR
    delay(20);

    // Drive INT low during reset to select address 0x5D
    pinMode(TOUCH_INT, OUTPUT);
    digitalWrite(TOUCH_INT, LOW);
    delay(2);

    // Release reset: set EXIO1 HIGH
    uint8_t rst_high[] = { 0x03 };  // bit1=EXIO2(BL)=1, bit0=EXIO1(RST)=1
    i2c0.write(0x38, rst_high, 1);
    delay(10);

    // Release INT (let GT911 drive it)
    pinMode(TOUCH_INT, INPUT);
    delay(100);  // GT911 needs ~100ms after reset
}
#endif

bool TouchHAL::init() {

#if defined(TOUCH_INT) && TOUCH_INT >= 0
    _int_pin = TOUCH_INT;
    ::pinMode(_int_pin, INPUT);
#endif

#if defined(BOARD_TOUCH_LCD_43C_BOX)
    // Always perform a full GT911 reset via CH422G IO expander.
    // This ensures the GT911 starts in a known state with the correct I2C
    // address (0x5D) regardless of what happened during prior I2C bus scans.
#if HAS_IO_EXPANDER
    gt911_reset_via_ch422g();
#endif

    bool found = false;
    _addr = 0x5D;
    if (i2c0.probe(_addr)) { found = true; }
    if (!found) {
        _addr = 0x14;
        if (i2c0.probe(_addr)) { found = true; }
    }
    if (!found) return false;

    _driver = GT911;

    // Read existing config to verify GT911 is responsive
    uint8_t cfg_ver = gt911_readReg(0x8047);
    uint8_t x_lo = gt911_readReg(0x8048);
    uint8_t x_hi = gt911_readReg(0x8049);
    uint8_t y_lo = gt911_readReg(0x804A);
    uint8_t y_hi = gt911_readReg(0x804B);
    uint16_t cfg_x = x_lo | (x_hi << 8);
    uint16_t cfg_y = y_lo | (y_hi << 8);
    uint8_t module_sw1 = gt911_readReg(0x804D);

    Serial.printf("[GT911] Config ver=0x%02X res=%ux%u module_sw1=0x%02X addr=0x%02X\n",
                  cfg_ver, cfg_x, cfg_y, module_sw1, _addr);

    // Clear any stale status to start fresh
    gt911_writeReg(0x814E, 0);

    // Read product ID to verify communication
    uint8_t pid[5] = {};
    uint8_t pid_reg[2] = { 0x81, 0x40 };
    i2c0.writeRead(_addr, pid_reg, 2, pid, 4);
    pid[4] = 0;
    Serial.printf("[GT911] Product ID: %s\n", pid);

    // Send software reset command (0x02 to register 0x8040)
    gt911_writeReg(0x8040, 0x02);
    delay(100);

    // Wake GT911 — toggle INT low for 58ms then release (LovyanGFX pattern)
    if (_int_pin >= 0) {
        ::pinMode(_int_pin, OUTPUT);
        ::digitalWrite(_int_pin, LOW);
        delay(58);
        ::digitalWrite(_int_pin, HIGH);
        delay(2);
        // INPUT with pullup — use GPIO API directly since tritium_compat
        // doesn't define INPUT_PULLUP
        gpio_set_direction((gpio_num_t)_int_pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)_int_pin, GPIO_PULLUP_ONLY);
    }
    delay(50);

    // Send coordinate read command (0x00 to register 0x8040)
    gt911_writeReg(0x8040, 0x00);
    delay(10);

    // Clear stale status
    gt911_writeReg(0x814E, 0);

    // Clear status and command registers
    gt911_writeReg(0x8040, 0x00);  // Coordinate read mode
    gt911_writeReg(0x814E, 0x00);  // Clear stale status

    Serial.printf("[GT911] Init complete: addr=0x%02X, %ux%u, int=%d\n",
                  _addr, cfg_x, cfg_y, _int_pin);

    return true;

#elif defined(BOARD_TOUCH_LCD_35BC) || defined(BOARD_TOUCH_LCD_349)
#if defined(TOUCH_I2C_ADDR)
    _addr = TOUCH_I2C_ADDR;
#else
    _addr = 0x3B;
#endif
    if (!i2c0.probe(_addr)) return false;
    _driver = AXS15231B_TOUCH;
    return true;

#elif defined(BOARD_TOUCH_AMOLED_241B)
#if defined(TOUCH_I2C_ADDR)
    _addr = TOUCH_I2C_ADDR;
#else
    _addr = 0x38;
#endif
    if (!i2c0.probe(_addr)) return false;
    _driver = FT6336;
    return true;

#elif defined(BOARD_AMOLED_191M) || defined(BOARD_TOUCH_AMOLED_18)
#if defined(TOUCH_I2C_ADDR)
    _addr = TOUCH_I2C_ADDR;
#else
    _addr = 0x38;
#endif
    if (!i2c0.probe(_addr)) return false;
    _driver = FT3168;
    return true;

#else
    return false;
#endif
}

bool TouchHAL::isTouched() {
    if (_driver == NONE) return false;

    // GT911 must always use register polling — INT pin behavior depends on
    // configured mode (rising/falling/low/high pulse) and is unreliable as
    // a simple digitalRead check.
    if (_driver == GT911) {
        uint8_t status = gt911_readReg(0x814E);
        return (status & 0x80) && (status & 0x0F) > 0;
    }

    // For FocalTech drivers, INT low = touch active
    if (_int_pin >= 0) {
        return ::digitalRead(_int_pin) == LOW;
    }

    uint8_t numPoints = readReg8(0x02);
    return (numPoints & 0x0F) > 0;
}

bool TouchHAL::read(uint16_t &x, uint16_t &y) {
    switch (_driver) {
        case FT3168:
        case FT6336:    return ft_read(x, y);
        case GT911:     return gt911_read(x, y);
        case AXS15231B_TOUCH: return axs_read(x, y);
        default:        return false;
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
    uint8_t numPoints = readReg8(0x02) & 0x0F;
    if (numPoints == 0) return false;
    uint8_t xH = readReg8(0x03) & 0x0F;
    uint8_t xL = readReg8(0x04);
    uint8_t yH = readReg8(0x05) & 0x0F;
    uint8_t yL = readReg8(0x06);
    x = (xH << 8) | xL;
    y = (yH << 8) | yL;
    return true;
}

bool TouchHAL::gt911_read(uint16_t &x, uint16_t &y) {
    uint8_t status = gt911_readReg(0x814E);

    // Periodic debug: log status register every ~5 seconds
    static uint32_t s_gt911_dbg_count = 0;
    static uint32_t s_gt911_dbg_time = 0;
    static uint32_t s_gt911_nonzero = 0;
    s_gt911_dbg_count++;
    if (status != 0) s_gt911_nonzero++;
    uint32_t now = ::millis();
    if (now - s_gt911_dbg_time > 5000) {
        Serial.printf("[GT911] poll: %u reads, %u non-zero status, last=0x%02X\n",
                      (unsigned)s_gt911_dbg_count, (unsigned)s_gt911_nonzero, status);
        s_gt911_dbg_count = 0;
        s_gt911_nonzero = 0;
        s_gt911_dbg_time = now;
    }

    if (!(status & 0x80)) return false;
    uint8_t numPoints = status & 0x0F;
    if (numPoints == 0) {
        gt911_writeReg(0x814E, 0);
        return false;
    }

    uint8_t buf[4];
    uint8_t reg[2] = { 0x81, 0x50 };
    i2c0.writeRead(_addr, reg, 2, buf, 4);
    x = buf[0] | (buf[1] << 8);
    y = buf[2] | (buf[3] << 8);
    gt911_writeReg(0x814E, 0);

    return true;
}

bool TouchHAL::axs_read(uint16_t &x, uint16_t &y) {
    uint8_t buf[8];
    uint8_t reg = 0x00;
    i2c0.writeRead(_addr, &reg, 1, buf, 8);
    if (buf[0] == 0xFF || (buf[1] == 0xFF && buf[2] == 0xFF)) {
        return false;
    }
    x = ((buf[2] & 0x0F) << 8) | buf[3];
    y = ((buf[4] & 0x0F) << 8) | buf[5];
    return true;
}

uint8_t TouchHAL::readReg8(uint8_t reg) {
    uint8_t val = 0;
    i2c0.readReg(_addr, reg, &val);
    return val;
}

uint16_t TouchHAL::readReg16(uint8_t regH, uint8_t regL) {
    return ((uint16_t)readReg8(regH) << 8) | readReg8(regL);
}

void TouchHAL::writeReg8(uint8_t reg, uint8_t val) {
    i2c0.writeReg(_addr, reg, val);
}

uint8_t TouchHAL::gt911_readReg(uint16_t reg) {
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    uint8_t val = 0;
    i2c0.writeRead(_addr, reg_buf, 2, &val, 1);
    return val;
}

void TouchHAL::gt911_writeReg(uint16_t reg, uint8_t val) {
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    i2c0.write(_addr, buf, 3);
}

int TouchHAL::dumpDiag(char* buf, size_t size) {
    if (_driver != GT911) {
        return snprintf(buf, size, "{\"error\":\"not GT911\"}");
    }

    // Read key registers
    uint8_t status = gt911_readReg(0x814E);
    uint8_t cfg_ver = gt911_readReg(0x8047);
    uint8_t x_lo = gt911_readReg(0x8048);
    uint8_t x_hi = gt911_readReg(0x8049);
    uint8_t y_lo = gt911_readReg(0x804A);
    uint8_t y_hi = gt911_readReg(0x804B);
    uint8_t touch_num = gt911_readReg(0x804C);
    uint8_t mod_sw1 = gt911_readReg(0x804D);
    uint8_t chksum = gt911_readReg(0x80FF);
    uint8_t fresh = gt911_readReg(0x8100);

    // Product ID (4 ASCII bytes at 0x8140)
    uint8_t pid[5] = {};
    uint8_t pid_reg[2] = { 0x81, 0x40 };
    i2c0.writeRead(_addr, pid_reg, 2, pid, 4);
    pid[4] = 0;

    // Firmware version (2 bytes at 0x8144)
    uint8_t fw_lo = gt911_readReg(0x8144);
    uint8_t fw_hi = gt911_readReg(0x8145);

    // INT pin state
    int int_state = (_int_pin >= 0) ? ::digitalRead(_int_pin) : -1;

    // Point data (first 8 bytes at 0x8150)
    uint8_t pt[8] = {};
    uint8_t pt_reg[2] = { 0x81, 0x50 };
    i2c0.writeRead(_addr, pt_reg, 2, pt, 8);

    return snprintf(buf, size,
        "{\"addr\":\"0x%02X\",\"product_id\":\"%s\",\"fw_ver\":%u,"
        "\"status\":\"0x%02X\",\"cfg_ver\":\"0x%02X\","
        "\"res_x\":%u,\"res_y\":%u,\"touch_num\":%u,"
        "\"mod_sw1\":\"0x%02X\",\"chksum\":\"0x%02X\",\"fresh\":%u,"
        "\"int_pin\":%d,\"int_state\":%d,"
        "\"pt_data\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
        _addr, pid, (fw_hi << 8) | fw_lo,
        status, cfg_ver,
        x_lo | (x_hi << 8), y_lo | (y_hi << 8), touch_num,
        mod_sw1, chksum, fresh,
        _int_pin, int_state,
        pt[0], pt[1], pt[2], pt[3], pt[4], pt[5], pt[6], pt[7]);
}

#endif // SIMULATOR
