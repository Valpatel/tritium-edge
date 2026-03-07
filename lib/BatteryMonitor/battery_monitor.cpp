#include "battery_monitor.h"
#include "hal_power.h"
#include "debug_log.h"

static const char* TAG = "batt";

// ── Helper: piecewise-linear interpolation on a descending table ───────

struct SOCPoint {
    float   voltage;
    uint8_t soc;
};

static int interpolateTable(const SOCPoint* table, int len, float v) {
    if (len < 2) return -1;

    // Clamp to table bounds
    if (v >= table[0].voltage) return table[0].soc;
    if (v <= table[len - 1].voltage) return table[len - 1].soc;

    // Find the bracketing pair (table is voltage-descending)
    for (int i = 0; i < len - 1; ++i) {
        float v_hi = table[i].voltage;
        float v_lo = table[i + 1].voltage;
        if (v <= v_hi && v >= v_lo) {
            float range = v_hi - v_lo;
            if (range <= 0.0f) return table[i + 1].soc;
            float t = (v - v_lo) / range;  // 0..1
            int soc_lo = table[i + 1].soc;
            int soc_hi = table[i].soc;
            return soc_lo + static_cast<int>(t * (soc_hi - soc_lo) + 0.5f);
        }
    }
    return 0;
}

// ── Built-in chemistry tables ──────────────────────────────────────────

// Standard Li-ion / LiPo  (3.0V – 4.2V)
static const SOCPoint LI_ION_TABLE[] = {
    { 4.20f, 100 },
    { 4.10f,  95 },
    { 4.00f,  80 },
    { 3.90f,  65 },
    { 3.80f,  50 },
    { 3.70f,  35 },
    { 3.60f,  20 },
    { 3.50f,  10 },
    { 3.30f,   5 },
    { 3.00f,   0 },
};
static constexpr int LI_ION_TABLE_LEN = sizeof(LI_ION_TABLE) / sizeof(LI_ION_TABLE[0]);

// High-voltage Li-ion  (3.0V – 4.35V)
static const SOCPoint LI_ION_HV_TABLE[] = {
    { 4.35f, 100 },
    { 4.25f,  95 },
    { 4.15f,  85 },
    { 4.05f,  70 },
    { 3.95f,  55 },
    { 3.85f,  40 },
    { 3.75f,  25 },
    { 3.65f,  15 },
    { 3.50f,   5 },
    { 3.00f,   0 },
};
static constexpr int LI_ION_HV_TABLE_LEN = sizeof(LI_ION_HV_TABLE) / sizeof(LI_ION_HV_TABLE[0]);

// LiFePO4  (2.5V – 3.65V)
static const SOCPoint LIFEPO4_TABLE[] = {
    { 3.65f, 100 },
    { 3.40f,  95 },
    { 3.35f,  85 },
    { 3.32f,  70 },
    { 3.30f,  55 },
    { 3.27f,  40 },
    { 3.20f,  25 },
    { 3.10f,  15 },
    { 2.80f,   5 },
    { 2.50f,   0 },
};
static constexpr int LIFEPO4_TABLE_LEN = sizeof(LIFEPO4_TABLE) / sizeof(LIFEPO4_TABLE[0]);

// ── Chemistry lookup dispatchers ───────────────────────────────────────

int BatteryMonitor::liIonSOC(float v) {
    return interpolateTable(LI_ION_TABLE, LI_ION_TABLE_LEN, v);
}

int BatteryMonitor::liIonHvSOC(float v) {
    return interpolateTable(LI_ION_HV_TABLE, LI_ION_HV_TABLE_LEN, v);
}

int BatteryMonitor::lifePO4SOC(float v) {
    return interpolateTable(LIFEPO4_TABLE, LIFEPO4_TABLE_LEN, v);
}

int BatteryMonitor::customSOC(float v) {
    if (_config.custom_table_len < 2) return -1;

    // Reinterpret BatteryConfig::VoltagePoint array as SOCPoint array.
    // The structs are layout-compatible (float, uint8_t) but we build a
    // temporary SOCPoint array to keep interpolateTable's signature clean.
    SOCPoint pts[BatteryConfig::MAX_CUSTOM_POINTS];
    for (int i = 0; i < _config.custom_table_len; ++i) {
        pts[i].voltage = _config.custom_table[i].voltage;
        pts[i].soc     = _config.custom_table[i].soc;
    }
    return interpolateTable(pts, _config.custom_table_len, v);
}

// ── Public API ─────────────────────────────────────────────────────────

void BatteryMonitor::init(PowerHAL* power, const BatteryConfig& config) {
    _power  = power;
    _config = config;

    // Zero smoothing buffers
    for (int i = 0; i < SMOOTH_SAMPLES; ++i)
        _voltage_buf[i] = 0.0f;
    _voltage_idx   = 0;
    _voltage_count = 0;

    // Reset status
    _status = {};

    DBG_INFO(TAG, "init  chemistry=%d  capacity=%.0f mAh  cells=%d",
             static_cast<int>(_config.chemistry),
             _config.capacity_mah,
             _config.cell_count);
}

void BatteryMonitor::update() {
    if (!_power) return;

    PowerInfo info = _power->getInfo();

    // Apply voltage divider correction
    float raw = info.voltage * _config.voltage_divider;

    _status.raw_voltage = raw;
    _status.charging    = info.is_charging;
    _status.usb_powered = info.is_usb_powered;
    _status.has_voltage = (_power->hasPMIC() || _power->hasADC());
    _status.has_coulomb = false;  // No fuel gauge support yet

    // Smooth the voltage (skip invalid readings)
    if (raw > 0.0f) {
        _status.voltage = smoothVoltage(raw);
    }

    // Min / max tracking (only for valid readings)
    if (_status.voltage > 0.0f) {
        if (_status.min_voltage <= 0.0f || _status.voltage < _status.min_voltage)
            _status.min_voltage = _status.voltage;
        if (_status.voltage > _status.max_voltage)
            _status.max_voltage = _status.voltage;
    }

    // SOC estimation
    if (_status.has_voltage && _status.voltage > 0.0f) {
        _status.soc = voltageToSOC(_status.voltage);
    } else {
        _status.soc = -1;
    }

    // Time remaining
    updateTimeEstimate();

    DBG_VERBOSE(TAG, "V=%.2f  smooth=%.2f  soc=%d%%  chrg=%d",
                raw, _status.voltage, _status.soc, _status.charging);
}

const BatteryStatus& BatteryMonitor::getStatus() const {
    return _status;
}

void BatteryMonitor::setConfig(const BatteryConfig& config) {
    _config = config;
    DBG_INFO(TAG, "config updated  chemistry=%d  capacity=%.0f mAh",
             static_cast<int>(_config.chemistry), _config.capacity_mah);
}

const BatteryConfig& BatteryMonitor::getConfig() const {
    return _config;
}

void BatteryMonitor::setDrawRate(float ma) {
    _config.avg_draw_ma = ma;
    DBG_DEBUG(TAG, "draw rate set to %.1f mA", ma);
}

void BatteryMonitor::resetMinMax() {
    _status.min_voltage = 0.0f;
    _status.max_voltage = 0.0f;
}

// ── Private helpers ────────────────────────────────────────────────────

float BatteryMonitor::smoothVoltage(float raw) {
    _voltage_buf[_voltage_idx] = raw;
    _voltage_idx = (_voltage_idx + 1) % SMOOTH_SAMPLES;
    if (_voltage_count < SMOOTH_SAMPLES)
        ++_voltage_count;

    float sum = 0.0f;
    int   count = 0;
    for (int i = 0; i < _voltage_count; ++i) {
        if (_voltage_buf[i] > 0.0f) {
            sum += _voltage_buf[i];
            ++count;
        }
    }
    return (count > 0) ? (sum / static_cast<float>(count)) : raw;
}

int BatteryMonitor::voltageToSOC(float voltage) {
    // Per-cell voltage for multi-cell packs
    float per_cell = (_config.cell_count > 1)
                     ? voltage / static_cast<float>(_config.cell_count)
                     : voltage;

    switch (_config.chemistry) {
        case BatteryChemistry::LI_ION:    return liIonSOC(per_cell);
        case BatteryChemistry::LI_ION_HV: return liIonHvSOC(per_cell);
        case BatteryChemistry::LIFEPO4:   return lifePO4SOC(per_cell);
        case BatteryChemistry::CUSTOM:    return customSOC(per_cell);
    }
    return -1;
}

void BatteryMonitor::updateTimeEstimate() {
    // Only estimate while discharging with a known SOC and positive draw
    if (_status.charging || _status.soc < 0 || _config.avg_draw_ma <= 0.0f) {
        _status.minutes_remaining = -1;
        _status.hours_remaining   = -1.0f;
        return;
    }

    float remaining_mah = _config.capacity_mah * _status.soc / 100.0f;
    float hours = remaining_mah / _config.avg_draw_ma;
    _status.hours_remaining   = hours;
    _status.minutes_remaining = static_cast<int>(hours * 60.0f + 0.5f);
}
