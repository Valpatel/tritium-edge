#pragma once
// Chemistry-aware battery monitoring library.
// Sits on top of PowerHAL and adds SOC estimation via voltage-to-SOC
// lookup tables, exponential moving average smoothing, and time-remaining
// calculation.
//
// Usage:
//   #include "battery_monitor.h"
//   BatteryMonitor batt;
//   batt.init(&power);               // default Li-ion config
//   batt.update();                    // call every 1-5 seconds
//   auto& st = batt.getStatus();
//   DBG_INFO("batt", "SOC=%d%%  %.2fV", st.soc, st.voltage);

#include <cstdint>
#include <cstddef>

class PowerHAL;

// ── Chemistry selection ────────────────────────────────────────────────

enum class BatteryChemistry : uint8_t {
    LI_ION,     // Standard Li-ion / LiPo  (3.0V – 4.2V)
    LI_ION_HV,  // High-voltage Li-ion     (3.0V – 4.35V)
    LIFEPO4,    // LiFePO4                  (2.5V – 3.65V)
    CUSTOM,     // User-provided lookup table
};

// ── Configuration ──────────────────────────────────────────────────────

struct BatteryConfig {
    BatteryChemistry chemistry = BatteryChemistry::LI_ION;
    float capacity_mah = 2600.0f;    // Battery capacity (18650 default)
    float avg_draw_ma  = 150.0f;     // Estimated average current draw
    float voltage_divider = 1.0f;    // Applied AFTER hal_power reads (1.0 = no correction)
    int   cell_count = 1;            // Series cells (for multi-cell packs)

    // Custom voltage-SOC table (used when chemistry == CUSTOM).
    // Entries must be voltage descending.  Up to 12 points.
    struct VoltagePoint {
        float   voltage;
        uint8_t soc;  // 0-100
    };
    static constexpr int MAX_CUSTOM_POINTS = 12;
    VoltagePoint custom_table[MAX_CUSTOM_POINTS] = {};
    int custom_table_len = 0;
};

// ── Status output ──────────────────────────────────────────────────────

struct BatteryStatus {
    float voltage      = 0.0f;  // Smoothed battery voltage
    float raw_voltage  = 0.0f;  // Unsmoothed voltage from hardware
    int   soc          = -1;    // State of charge 0-100, -1 if unknown
    bool  charging     = false; // Currently charging
    bool  usb_powered  = false; // USB power detected
    bool  has_voltage  = false; // Hardware can read voltage (PMIC or ADC)
    bool  has_coulomb  = false; // Has coulomb counter (fuel gauge)

    // Time estimates
    int   minutes_remaining = -1;    // -1 if unknown or charging
    float hours_remaining   = -1.0f; // -1.0 if unknown

    // Health
    float min_voltage = 0.0f;  // Lowest voltage seen this session
    float max_voltage = 0.0f;  // Highest voltage seen this session
};

// ── BatteryMonitor class ───────────────────────────────────────────────

class BatteryMonitor {
public:
    /// Initialise with a PowerHAL pointer and optional config.
    void init(PowerHAL* power, const BatteryConfig& config = {});

    /// Call periodically (every 1-5 s).  Reads hardware, updates smoothing.
    void update();

    /// Get current status (cheap — returns a reference).
    const BatteryStatus& getStatus() const;

    /// Replace the full config at runtime.
    void setConfig(const BatteryConfig& config);
    const BatteryConfig& getConfig() const;

    /// Adjust draw estimate at runtime (e.g. WiFi on → increase draw).
    void setDrawRate(float ma);

    /// Reset min/max voltage tracking.
    void resetMinMax();

private:
    PowerHAL*      _power  = nullptr;
    BatteryConfig  _config;
    BatteryStatus  _status = {};

    // Exponential moving average for voltage smoothing
    static constexpr int SMOOTH_SAMPLES = 8;
    float _voltage_buf[SMOOTH_SAMPLES] = {};
    int   _voltage_idx   = 0;
    int   _voltage_count = 0;

    float smoothVoltage(float raw);
    int   voltageToSOC(float voltage);
    void  updateTimeEstimate();

    // Built-in chemistry tables
    static int liIonSOC(float v);
    static int liIonHvSOC(float v);
    static int lifePO4SOC(float v);
    int customSOC(float v);
};
