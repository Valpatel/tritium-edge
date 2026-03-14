#pragma once
// Battery and Power Management HAL
// Supports AXP2101 PMIC, ADC battery voltage, and charge status pins
//
// Usage:
//   #include "hal_power.h"
//   PowerHAL power;
//   power.init(Wire);           // ESP32 via Arduino Wire
//   power.initLgfx(0, 0x34);   // ESP32 via lgfx::i2c
//   power.init();               // simulator

#include <cstdint>
#include <cstddef>

#ifndef SIMULATOR
class TwoWire;
#endif

enum class PowerSource : uint8_t {
    UNKNOWN,
    USB,
    BATTERY
};

enum class ChargeState : uint8_t {
    UNKNOWN,
    NOT_CHARGING,
    PRE_CHARGE,      // Trickle charge for deeply discharged batteries
    CONSTANT_CURRENT, // CC phase — bulk charging
    CONSTANT_VOLTAGE, // CV phase — topping off
    CHARGE_DONE       // Fully charged, on USB
};

struct PowerInfo {
    float voltage;
    int percentage;       // 0-100, -1 if unknown
    bool is_charging;
    bool is_usb_powered;
    bool has_battery;
    PowerSource source;
    ChargeState charge_state;
    float charge_current_ma;  // Current charge current in mA (0 if not available)
    float pmic_temp_c;        // PMIC temperature in Celsius (-999 if unavailable)
};

// Battery history reading for trend analysis
struct BatteryReading {
    float voltage;
    int percentage;
    bool is_charging;
    uint32_t timestamp_ms;    // millis() when recorded
};

static constexpr int BATTERY_HISTORY_SIZE = 100;

typedef void (*LowBatteryCallback)(int percentage);

class PowerHAL {
public:
#ifdef SIMULATOR
    bool init();
#else
    bool init(TwoWire &wire);
    bool initLgfx(uint8_t i2c_port, uint8_t addr = 0x34);
#endif
    PowerInfo getInfo();
    float getBatteryVoltage();
    int getBatteryLevel();
    bool isCharging();
    bool setChargeCurrent(uint16_t mA);
    bool hasPMIC() const { return _has_pmic; }
    bool hasADC() const { return _has_adc; }
    bool available() const { return _initialized; }

    void setLowBatteryThreshold(int percent) { _low_threshold = percent; }
    void onLowBattery(LowBatteryCallback cb) { _low_cb = cb; }
    void poll();

    // Battery history for trend analysis
    int getBatteryHistory(BatteryReading* out, int max_count) const;
    int getBatteryHistoryCount() const;

    // Append battery history to a JSON buffer (for heartbeat enrichment).
    // Returns bytes written (excluding null terminator).
    int getBatteryHistoryJson(char* buf, size_t size) const;

    // Get detailed charge state (richer than isCharging bool)
    ChargeState getChargeState();

private:
    bool _initialized = false;
    bool _has_pmic = false;
    bool _has_adc = false;
    int _low_threshold = 15;
    bool _low_warned = false;
    LowBatteryCallback _low_cb = nullptr;

    // Battery history ring buffer
    BatteryReading _history[BATTERY_HISTORY_SIZE] = {};
    int _history_head = 0;      // Next write position
    int _history_count = 0;     // Number of valid entries
    uint32_t _last_history_ms = 0;
    static constexpr uint32_t HISTORY_INTERVAL_MS = 30000;  // Record every 30s

    void _recordHistory(const PowerInfo& info);

#ifndef SIMULATOR
    TwoWire *_wire = nullptr;
    bool _use_lgfx = false;
    uint8_t _lgfx_port = 0;
    uint8_t _addr = 0;

    bool initAXP2101();
    float axp_getBatteryVoltage();
    bool axp_isCharging();
    float axp_getPMICTemp();
    float axp_getChargeCurrent();
    void writeReg(uint8_t reg, uint8_t val);
    uint8_t readReg(uint8_t reg);
    float adc_getBatteryVoltage();
#endif
};
