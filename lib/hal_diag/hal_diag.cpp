/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hal_diag.h"
#include "debug_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>

// ═════════════════════════════════════════════════════════════════════════════
// Simulator stubs
// ═════════════════════════════════════════════════════════════════════════════

#ifdef SIMULATOR

namespace hal_diag {

static bool _initialized = false;
static DiagConfig _cfg;
static HealthSnapshot _current = {};
static HealthSnapshot _history[8];
static int _history_count = 0;
static DiagEvent _events[16];
static int _event_count = 0;
static Anomaly _anomalies[4];
static int _anomaly_count = 0;

bool init(const DiagConfig& cfg) {
    _cfg = cfg;
    _initialized = true;
    _history_count = 0;
    _event_count = 0;
    _anomaly_count = 0;
    DBG_INFO("diag", "Simulator diagnostics initialized");
    return true;
}

void tick() {}

void set_power_provider(PowerProvider) {}
void set_camera_provider(CameraProvider) {}
void set_touch_provider(TouchProvider) {}
void set_ntp_provider(NtpProvider) {}
void set_mesh_provider(MeshProvider) {}
void report_loop_time(uint32_t) {}
void report_i2c_result(uint8_t, bool, bool, int16_t) {}
void report_display_frame(uint32_t) {}

void log(Severity sev, const char* subsystem, const char* fmt, ...) {
    if (!_initialized) return;
    char msg[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    (void)sev;
    (void)subsystem;
    (void)msg;
}

void log_value(const char* subsystem, const char* metric,
               float value, float expected_min, float expected_max) {
    (void)subsystem; (void)metric; (void)value;
    (void)expected_min; (void)expected_max;
}

HealthSnapshot take_snapshot() {
    HealthSnapshot s = {};
    s.free_heap = 256000;
    s.min_free_heap = 200000;
    s.free_psram = 4000000;
    s.largest_free_block = 128000;
    s.battery_voltage = 3.85f;
    s.battery_percent = 72.0f;
    s.wifi_rssi = -55;
    s.wifi_connected = true;
    s.uptime_s = 3600;
    s.display_initialized = true;
    return s;
}

const HealthSnapshot* get_history(int& count) {
    count = _history_count;
    return _history;
}

const Anomaly* get_anomalies(int& count) {
    count = _anomaly_count;
    return _anomalies;
}

void clear_anomalies() { _anomaly_count = 0; }

int health_to_json(char* buf, size_t size) {
    return snprintf(buf, size, "{\"simulator\":true}");
}

int events_to_json(char* buf, size_t size, int) {
    return snprintf(buf, size, "{\"events\":[]}");
}

int anomalies_to_json(char* buf, size_t size) {
    return snprintf(buf, size, "{\"anomalies\":[]}");
}

int full_report_json(char* buf, size_t size) {
    return snprintf(buf, size,
        "{\"health\":{\"simulator\":true},\"events\":[],\"anomalies\":[]}");
}

int scan_i2c_bus(uint8_t, uint8_t*, int) { return 0; }
void test_display_pattern(esp_lcd_panel_handle_t, int, int) {}
uint32_t get_reboot_count() { return 1; }
uint8_t get_last_reset_reason() { return 0; }
const char* reset_reason_str(uint8_t) { return "SIMULATOR"; }
bool has_crash_info() { return false; }
bool get_crash_info(CrashInfo&) { return false; }
void clear_crash_info() {}
void store_crash(const char*, const char*) {}

}  // namespace hal_diag

// ═════════════════════════════════════════════════════════════════════════════
// ESP32 implementation
// ═════════════════════════════════════════════════════════════════════════════

#else  // ESP32

#include <Arduino.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <Wire.h>
#include <driver/temperature_sensor.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// SD card support — optional, only if SD_MMC is available
#if __has_include(<SD_MMC.h>)
#include <SD_MMC.h>
#include <FS.h>
#define DIAG_HAS_SD 1
#else
#define DIAG_HAS_SD 0
#endif

// Display HAL — optional, for display health checks
#if __has_include("display.h")
#include "display.h"
#define DIAG_HAS_DISPLAY 1
#else
#define DIAG_HAS_DISPLAY 0
#endif

// Persistent diagnostic log — optional, writes key events to LittleFS ring buffer
#if __has_include("hal_diaglog.h")
#include "hal_diaglog.h"
#define DIAG_HAS_DIAGLOG 1
#else
#define DIAG_HAS_DIAGLOG 0
#endif

namespace hal_diag {

// ── Internal state ──────────────────────────────────────────────────────────

static bool         _initialized = false;
static DiagConfig   _cfg;

// Ring buffers allocated in PSRAM
static DiagEvent*       _events = nullptr;
static uint16_t         _event_head = 0;    // Next write position
static uint16_t         _event_count = 0;   // Total valid entries

static HealthSnapshot*  _snapshots = nullptr;
static uint16_t         _snap_head = 0;
static uint16_t         _snap_count = 0;

// Anomaly list (small, fixed-size)
static constexpr int    MAX_ANOMALIES = 8;
static Anomaly          _anomalies[MAX_ANOMALIES];
static int              _anomaly_count = 0;

// Timing
static uint32_t         _last_health_ms = 0;
static uint32_t         _last_flush_ms = 0;

// Counters (volatile — may be updated from ISR context in future)
static volatile uint32_t _wifi_disconnects = 0;
static volatile uint32_t _mqtt_disconnects = 0;
static volatile uint8_t  _i2c_errors = 0;

// Loop timing (fed externally or from watchdog HAL)
static uint32_t         _loop_time_us = 0;
static uint32_t         _max_loop_time_us = 0;
static uint32_t         _loop_start_us = 0;

// External providers
static PowerProvider    _power_provider = nullptr;
static CameraProvider   _camera_provider = nullptr;
static TouchProvider    _touch_provider = nullptr;
static NtpProvider      _ntp_provider = nullptr;
static MeshProvider     _mesh_provider = nullptr;

// NVS keys
static constexpr const char* NVS_NAMESPACE = "hal_diag";
static constexpr const char* NVS_KEY_REBOOT = "reboot_cnt";

// Reboot counter (cached from NVS)
static uint32_t         _reboot_count = 0;
static uint8_t          _reset_reason = 0;

// SD card log state
#if DIAG_HAS_SD
static bool             _sd_mounted = false;
static char             _sd_log_dir[64] = {};
#endif

// Temperature sensor handle
static temperature_sensor_handle_t _temp_handle = nullptr;

// ── Serial color codes ──────────────────────────────────────────────────────

static const char* severity_color(Severity sev) {
    switch (sev) {
        case Severity::TRACE: return "\033[90m";    // Dark gray
        case Severity::DEBUG: return "\033[36m";    // Cyan
        case Severity::INFO:  return "\033[32m";    // Green
        case Severity::WARN:  return "\033[33m";    // Yellow
        case Severity::ERROR: return "\033[31m";    // Red
        case Severity::FATAL: return "\033[35;1m";  // Bold magenta
        default:              return "\033[0m";
    }
}

static const char* severity_str(Severity sev) {
    switch (sev) {
        case Severity::TRACE: return "TRACE";
        case Severity::DEBUG: return "DEBUG";
        case Severity::INFO:  return "INFO";
        case Severity::WARN:  return "WARN";
        case Severity::ERROR: return "ERROR";
        case Severity::FATAL: return "FATAL";
        default:              return "?";
    }
}

// ── NVS helpers ─────────────────────────────────────────────────────────────

static void nvs_init_reboot_counter() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        DBG_WARN("diag", "NVS open failed: %d", err);
        return;
    }

    // Read current count
    uint32_t count = 0;
    err = nvs_get_u32(handle, NVS_KEY_REBOOT, &count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        count = 0;
    }

    // Increment and store
    count++;
    _reboot_count = count;
    nvs_set_u32(handle, NVS_KEY_REBOOT, count);
    nvs_commit(handle);
    nvs_close(handle);

    // Cache reset reason
    _reset_reason = (uint8_t)esp_reset_reason();
}

// ── Temperature sensor ──────────────────────────────────────────────────────

static void temp_sensor_init() {
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &_temp_handle);
    if (err != ESP_OK) {
        DBG_DEBUG("diag", "Temp sensor install failed: %d (may already be installed)", err);
        _temp_handle = nullptr;
        return;
    }
    temperature_sensor_enable(_temp_handle);
}

static float read_cpu_temp() {
    if (!_temp_handle) return 0.0f;
    float temp = 0.0f;
    if (temperature_sensor_get_celsius(_temp_handle, &temp) != ESP_OK) {
        return 0.0f;
    }
    return temp;
}

// ── SD card logging ─────────────────────────────────────────────────────────

#if DIAG_HAS_SD

static bool ensure_sd_mounted() {
    if (_sd_mounted) return true;

#if defined(SD_MMC_D0) && defined(SD_MMC_CLK) && defined(SD_MMC_CMD)
    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    if (SD_MMC.begin("/sdcard", true)) {
        _sd_mounted = true;
        // Create log directory
        SD_MMC.mkdir(_sd_log_dir);
        DBG_INFO("diag", "SD card mounted for logging: %s", _sd_log_dir);
        return true;
    }
#endif
    return false;
}

static void flush_events_to_sd() {
    if (!_cfg.log_to_sd || !ensure_sd_mounted()) return;
    if (_event_count == 0) return;

    // Build filename: /tritium/diag/YYYYMMDD.jsonl
    // Use epoch time from latest event, fallback to uptime-based name
    char filename[80];
    uint32_t epoch = 0;
    if (_event_count > 0) {
        uint16_t latest = (_event_head == 0) ? _cfg.max_events - 1 : _event_head - 1;
        epoch = _events[latest].epoch_time;
    }

    if (epoch > 1700000000) {
        // Valid epoch — convert to YYYYMMDD
        // Simple UTC date extraction (no full time library needed)
        uint32_t days = epoch / 86400;
        uint32_t y = 1970;
        while (true) {
            uint32_t days_in_year = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))
                                    ? 366 : 365;
            if (days < days_in_year) break;
            days -= days_in_year;
            y++;
        }
        // Approximate month/day
        static const uint16_t mdays[] = {
            0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365
        };
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        uint32_t m = 1;
        for (m = 1; m <= 12; m++) {
            uint32_t md = mdays[m] + ((leap && m > 2) ? 1 : 0);
            if (days < md) break;
        }
        uint32_t d = days - mdays[m - 1] - ((leap && m > 2) ? 1 : 0) + 1;
        snprintf(filename, sizeof(filename), "%s/%04lu%02lu%02lu.jsonl",
                 _sd_log_dir, (unsigned long)y, (unsigned long)m, (unsigned long)d);
    } else {
        snprintf(filename, sizeof(filename), "%s/boot_%lu.jsonl",
                 _sd_log_dir, (unsigned long)_reboot_count);
    }

    File f = SD_MMC.open(filename, FILE_APPEND);
    if (!f) {
        DBG_WARN("diag", "Cannot open SD log: %s", filename);
        return;
    }

    // Write all events since last flush — dump entire ring buffer
    // For simplicity, write the most recent batch
    char line[320];
    uint16_t start = (_event_count >= _cfg.max_events)
                     ? _event_head : 0;
    uint16_t count = (_event_count >= _cfg.max_events)
                     ? _cfg.max_events : _event_count;

    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (start + i) % _cfg.max_events;
        const DiagEvent& ev = _events[idx];
        int len = snprintf(line, sizeof(line),
            "{\"t\":%lu,\"epoch\":%lu,\"sev\":\"%s\","
            "\"sub\":\"%s\",\"msg\":\"%s\"",
            (unsigned long)ev.timestamp_ms,
            (unsigned long)ev.epoch_time,
            severity_str(ev.severity),
            ev.subsystem,
            ev.message);
        if (ev.expected_max > ev.expected_min) {
            len += snprintf(line + len, sizeof(line) - len,
                ",\"val\":%.3f,\"min\":%.3f,\"max\":%.3f",
                ev.value, ev.expected_min, ev.expected_max);
        }
        len += snprintf(line + len, sizeof(line) - len, "}\n");
        f.write((const uint8_t*)line, len);
    }

    f.close();
    DBG_DEBUG("diag", "Flushed %u events to %s", count, filename);
}

static void flush_snapshot_to_sd(const HealthSnapshot& snap) {
    if (!_cfg.log_to_sd || !ensure_sd_mounted()) return;

    char filename[80];
    snprintf(filename, sizeof(filename), "%s/health.jsonl", _sd_log_dir);

    File f = SD_MMC.open(filename, FILE_APPEND);
    if (!f) return;

    char line[512];
    int len = snprintf(line, sizeof(line),
        "{\"t\":%lu,\"epoch\":%lu,"
        "\"heap\":%lu,\"heap_min\":%lu,\"psram\":%lu,"
        "\"bat_v\":%.2f,\"bat_pct\":%.1f,"
        "\"cpu_c\":%.1f,"
        "\"wifi_rssi\":%d,\"wifi_ok\":%d,"
        "\"loop_us\":%lu,\"loop_max\":%lu,"
        "\"uptime\":%lu,\"reboots\":%lu,"
        "\"i2c_dev\":%u,\"i2c_err\":%u}\n",
        (unsigned long)snap.timestamp_ms,
        (unsigned long)snap.epoch_time,
        (unsigned long)snap.free_heap,
        (unsigned long)snap.min_free_heap,
        (unsigned long)snap.free_psram,
        snap.battery_voltage, snap.battery_percent,
        snap.cpu_temp_c,
        (int)snap.wifi_rssi, snap.wifi_connected ? 1 : 0,
        (unsigned long)snap.loop_time_us,
        (unsigned long)snap.max_loop_time_us,
        (unsigned long)snap.uptime_s,
        (unsigned long)snap.reboot_count,
        snap.i2c_devices_found, snap.i2c_errors);
    f.write((const uint8_t*)line, len);
    f.close();
}

#endif  // DIAG_HAS_SD

// ── Anomaly detection ───────────────────────────────────────────────────────

static void add_anomaly(const char* subsystem, const char* description,
                        float score) {
    if (_anomaly_count >= MAX_ANOMALIES) {
        // Overwrite oldest (shift array)
        memmove(&_anomalies[0], &_anomalies[1],
                sizeof(Anomaly) * (MAX_ANOMALIES - 1));
        _anomaly_count = MAX_ANOMALIES - 1;
    }
    Anomaly& a = _anomalies[_anomaly_count++];
    a.timestamp_ms = millis();
    strncpy(a.subsystem, subsystem, sizeof(a.subsystem) - 1);
    a.subsystem[sizeof(a.subsystem) - 1] = '\0';
    strncpy(a.description, description, sizeof(a.description) - 1);
    a.description[sizeof(a.description) - 1] = '\0';
    a.severity_score = score;

    if (_cfg.log_to_serial) {
        Serial.printf("\033[33m[diag/anomaly] %s: %s (score=%.2f)\033[0m\n",
                      subsystem, description, score);
    }

    // Persist anomaly to diaglog for post-mortem analysis
#if DIAG_HAS_DIAGLOG
    DiagSeverity dl_sev = (score >= 0.8f) ? DiagSeverity::CRITICAL
                        : (score >= 0.5f) ? DiagSeverity::ERR
                                          : DiagSeverity::WARN;
    diaglog_write(dl_sev, subsystem, 900, description, score);
#endif
}

static void run_anomaly_detection() {
    if (!_cfg.anomaly_detection || _snap_count < 5) return;

    // Get the N most recent snapshots for trend analysis
    int window = (_snap_count < 10) ? _snap_count : 10;
    int start_idx = (_snap_head - window + _cfg.max_snapshots) % _cfg.max_snapshots;

    // ── Heap leak detection ─────────────────────────────────────────────
    // Check if free heap is steadily declining
    {
        int declining = 0;
        for (int i = 1; i < window; i++) {
            int prev = (start_idx + i - 1) % _cfg.max_snapshots;
            int curr = (start_idx + i) % _cfg.max_snapshots;
            if (_snapshots[curr].free_heap < _snapshots[prev].free_heap) {
                declining++;
            }
        }
        if (declining >= window - 2 && window >= 5) {
            uint32_t newest_heap = _snapshots[(_snap_head - 1 + _cfg.max_snapshots)
                                              % _cfg.max_snapshots].free_heap;
            uint32_t oldest_heap = _snapshots[start_idx].free_heap;
            if (oldest_heap > 0 && newest_heap < oldest_heap) {
                float loss_pct = (float)(oldest_heap - newest_heap) / (float)oldest_heap;
                if (loss_pct > 0.05f) {  // >5% decline
                    char desc[128];
                    snprintf(desc, sizeof(desc),
                        "Heap declining: %lu -> %lu (%.1f%% loss over %d samples)",
                        (unsigned long)oldest_heap, (unsigned long)newest_heap,
                        loss_pct * 100.0f, window);
                    add_anomaly("memory", desc, fminf(1.0f, loss_pct * 2.0f));
                }
            }
        }
    }

    // ── Battery drain rate ──────────────────────────────────────────────
    {
        const HealthSnapshot& oldest = _snapshots[start_idx];
        const HealthSnapshot& newest = _snapshots[(_snap_head - 1 + _cfg.max_snapshots)
                                                  % _cfg.max_snapshots];
        if (oldest.battery_voltage > 3.0f && newest.battery_voltage > 2.5f) {
            float dt_hours = (float)(newest.timestamp_ms - oldest.timestamp_ms)
                             / 3600000.0f;
            if (dt_hours > 0.01f) {
                float drain_rate = (oldest.battery_voltage - newest.battery_voltage)
                                   / dt_hours;
                // Normal LiPo drain is ~0.05-0.2V/hr. Flag if >0.5V/hr
                if (drain_rate > 0.5f) {
                    char desc[128];
                    snprintf(desc, sizeof(desc),
                        "Battery draining fast: %.2fV/hr (%.2fV -> %.2fV)",
                        drain_rate, oldest.battery_voltage, newest.battery_voltage);
                    add_anomaly("power", desc,
                                fminf(1.0f, drain_rate / 1.0f));
                }
            }
        }
    }

    // ── WiFi RSSI degradation ───────────────────────────────────────────
    {
        int degrading = 0;
        for (int i = 1; i < window; i++) {
            int prev = (start_idx + i - 1) % _cfg.max_snapshots;
            int curr = (start_idx + i) % _cfg.max_snapshots;
            if (_snapshots[curr].wifi_rssi < _snapshots[prev].wifi_rssi) {
                degrading++;
            }
        }
        if (degrading >= window - 2 && window >= 5) {
            int8_t newest_rssi = _snapshots[(_snap_head - 1 + _cfg.max_snapshots)
                                            % _cfg.max_snapshots].wifi_rssi;
            if (newest_rssi < -80) {
                char desc[128];
                snprintf(desc, sizeof(desc),
                    "WiFi RSSI degrading: %d dBm (trending down)",
                    (int)newest_rssi);
                float score = fminf(1.0f, (float)(-newest_rssi - 70) / 30.0f);
                add_anomaly("wifi", desc, score);
            }
        }
    }

    // ── Loop time increasing ────────────────────────────────────────────
    {
        int increasing = 0;
        for (int i = 1; i < window; i++) {
            int prev = (start_idx + i - 1) % _cfg.max_snapshots;
            int curr = (start_idx + i) % _cfg.max_snapshots;
            if (_snapshots[curr].loop_time_us > _snapshots[prev].loop_time_us
                + 1000) {  // 1ms tolerance
                increasing++;
            }
        }
        if (increasing >= window - 2 && window >= 5) {
            uint32_t latest_loop = _snapshots[(_snap_head - 1 + _cfg.max_snapshots)
                                              % _cfg.max_snapshots].loop_time_us;
            if (latest_loop > 50000) {  // >50ms is concerning
                char desc[128];
                snprintf(desc, sizeof(desc),
                    "Loop time increasing: %lu us (trending up)",
                    (unsigned long)latest_loop);
                add_anomaly("perf", desc,
                            fminf(1.0f, (float)latest_loop / 200000.0f));
            }
        }
    }

    // ── I2C errors increasing ───────────────────────────────────────────
    {
        const HealthSnapshot& newest = _snapshots[(_snap_head - 1 + _cfg.max_snapshots)
                                                  % _cfg.max_snapshots];
        if (newest.i2c_errors > 10) {
            char desc[128];
            snprintf(desc, sizeof(desc),
                "I2C bus errors: %u (possible chip/wiring failure)",
                newest.i2c_errors);
            add_anomaly("i2c", desc,
                        fminf(1.0f, (float)newest.i2c_errors / 50.0f));
        }
    }

    // ── Temperature warnings ────────────────────────────────────────────
    {
        const HealthSnapshot& newest = _snapshots[(_snap_head - 1 + _cfg.max_snapshots)
                                                  % _cfg.max_snapshots];
        if (newest.cpu_temp_c > 70.0f) {
            char desc[128];
            snprintf(desc, sizeof(desc),
                "CPU temperature high: %.1f C", newest.cpu_temp_c);
            add_anomaly("thermal", desc,
                        fminf(1.0f, (newest.cpu_temp_c - 60.0f) / 30.0f));
        }
    }

    // ── Display FPS drop ────────────────────────────────────────────────
    {
        if (window >= 3) {
            const HealthSnapshot& oldest = _snapshots[start_idx];
            const HealthSnapshot& newest = _snapshots[(_snap_head - 1 + _cfg.max_snapshots)
                                                      % _cfg.max_snapshots];
            if (oldest.display_fps > 10 && newest.display_fps > 0
                && newest.display_fps < oldest.display_fps / 2) {
                char desc[128];
                snprintf(desc, sizeof(desc),
                    "Display FPS dropped: %lu -> %lu",
                    (unsigned long)oldest.display_fps,
                    (unsigned long)newest.display_fps);
                add_anomaly("display", desc, 0.6f);
            }
        }
    }
}

// ── Public API ──────────────────────────────────────────────────────────────

void set_power_provider(PowerProvider provider) {
    _power_provider = provider;
}

void set_camera_provider(CameraProvider provider) {
    _camera_provider = provider;
}

void set_touch_provider(TouchProvider provider) {
    _touch_provider = provider;
}

void set_ntp_provider(NtpProvider provider) {
    _ntp_provider = provider;
}

void set_mesh_provider(MeshProvider provider) {
    _mesh_provider = provider;
}

void report_loop_time(uint32_t loop_us) {
    _loop_time_us = loop_us;
    if (loop_us > _max_loop_time_us) _max_loop_time_us = loop_us;
}

// Per-slave I2C error tracking
static HealthSnapshot::I2cSlaveHealth _i2c_slave_table[HealthSnapshot::MAX_I2C_SLAVES] = {};
static uint8_t _i2c_slave_used = 0;

void report_i2c_result(uint8_t addr, bool success, bool is_timeout, int16_t latency_us) {
    // Find or allocate a slot for this address
    int slot = -1;
    for (int i = 0; i < _i2c_slave_used; i++) {
        if (_i2c_slave_table[i].addr == addr) { slot = i; break; }
    }
    if (slot < 0 && _i2c_slave_used < HealthSnapshot::MAX_I2C_SLAVES) {
        slot = _i2c_slave_used++;
        _i2c_slave_table[slot].addr = addr;
        _i2c_slave_table[slot].nack_count = 0;
        _i2c_slave_table[slot].timeout_count = 0;
        _i2c_slave_table[slot].success_count = 0;
    }
    if (slot < 0) return;  // Table full

    auto& s = _i2c_slave_table[slot];
    if (success) {
        s.success_count++;
    } else if (is_timeout) {
        s.timeout_count++;
        _i2c_errors++;
    } else {
        s.nack_count++;
        _i2c_errors++;
    }
    s.last_latency_us = latency_us;
}

// Display frame timing tracking
static uint32_t _display_frame_us = 0;
static uint32_t _display_max_frame_us = 0;

void report_display_frame(uint32_t frame_us) {
    _display_frame_us = frame_us;
    if (frame_us > _display_max_frame_us) _display_max_frame_us = frame_us;
}

bool init(const DiagConfig& cfg) {
    if (_initialized) {
        DBG_WARN("diag", "Already initialized");
        return true;
    }

    _cfg = cfg;

    // Allocate ring buffers in PSRAM
    size_t events_size = sizeof(DiagEvent) * cfg.max_events;
    size_t snaps_size = sizeof(HealthSnapshot) * cfg.max_snapshots;

    _events = (DiagEvent*)heap_caps_calloc(cfg.max_events, sizeof(DiagEvent),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _snapshots = (HealthSnapshot*)heap_caps_calloc(cfg.max_snapshots,
                                                    sizeof(HealthSnapshot),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!_events || !_snapshots) {
        // Fallback to internal heap with smaller buffers
        DBG_WARN("diag", "PSRAM alloc failed, using internal heap (reduced)");
        uint16_t reduced_events = 64;
        uint16_t reduced_snaps = 32;
        if (_events) heap_caps_free(_events);
        if (_snapshots) heap_caps_free(_snapshots);

        _events = (DiagEvent*)calloc(reduced_events, sizeof(DiagEvent));
        _snapshots = (HealthSnapshot*)calloc(reduced_snaps, sizeof(HealthSnapshot));
        _cfg.max_events = reduced_events;
        _cfg.max_snapshots = reduced_snaps;

        if (!_events || !_snapshots) {
            DBG_ERROR("diag", "Memory allocation failed");
            return false;
        }
    }

    _event_head = 0;
    _event_count = 0;
    _snap_head = 0;
    _snap_count = 0;
    _anomaly_count = 0;

    // Init NVS reboot counter
    nvs_init_reboot_counter();

    // Init CPU temperature sensor
    temp_sensor_init();

    // Setup SD log directory
#if DIAG_HAS_SD
    strncpy(_sd_log_dir, cfg.sd_log_path, sizeof(_sd_log_dir) - 1);
    _sd_log_dir[sizeof(_sd_log_dir) - 1] = '\0';
#endif

    _last_health_ms = millis();
    _last_flush_ms = millis();
    _initialized = true;

    DBG_INFO("diag", "Initialized: events=%u snapshots=%u (%.1fKB PSRAM)",
             _cfg.max_events, _cfg.max_snapshots,
             (float)(events_size + snaps_size) / 1024.0f);
    DBG_INFO("diag", "Reboot #%lu, reset reason: %s",
             (unsigned long)_reboot_count,
             reset_reason_str(_reset_reason));

    // Log the boot event
    log(Severity::INFO, "system", "Boot #%lu, reset: %s",
        (unsigned long)_reboot_count, reset_reason_str(_reset_reason));

    // Check for crash info from previous boot
    if (has_crash_info()) {
        CrashInfo crash = {};
        if (get_crash_info(crash)) {
            log(Severity::FATAL, "system",
                "Previous crash: %s (task=%s, heap=%lu, uptime=%lums)",
                crash.message, crash.task_name,
                (unsigned long)crash.free_heap,
                (unsigned long)crash.uptime_ms);
#if DIAG_HAS_DIAGLOG
            // Also write to persistent log for fleet server collection
            diaglog_write(
                DiagSeverity::CRITICAL, "crash", 999,
                crash.message, (float)crash.free_heap);
#endif
            clear_crash_info();
        }
    }

    return true;
}

void tick() {
    if (!_initialized) return;

    uint32_t now = millis();

    // Periodic health snapshot
    if (now - _last_health_ms >= _cfg.health_interval_ms) {
        _last_health_ms = now;
        HealthSnapshot snap = take_snapshot();

        // Store in ring buffer
        _snapshots[_snap_head] = snap;
        _snap_head = (_snap_head + 1) % _cfg.max_snapshots;
        if (_snap_count < _cfg.max_snapshots) _snap_count++;

#if DIAG_HAS_SD
        flush_snapshot_to_sd(snap);
#endif

        // Persist threshold-crossing snapshots to diaglog
#if DIAG_HAS_DIAGLOG
        if (snap.free_heap < 20000) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Heap critically low: %lu bytes",
                     (unsigned long)snap.free_heap);
            diaglog_write(DiagSeverity::WARN, "memory", 100, msg,
                          (float)snap.free_heap);
        }
        if (snap.i2c_errors > 10) {
            char msg[80];
            snprintf(msg, sizeof(msg), "I2C errors: %u", snap.i2c_errors);
            diaglog_write(DiagSeverity::WARN, "i2c", 200, msg,
                          (float)snap.i2c_errors);
        }
        if (!snap.wifi_connected && snap.wifi_disconnects > 0) {
            char msg[80];
            snprintf(msg, sizeof(msg), "WiFi disconnected (%lu total)",
                     (unsigned long)snap.wifi_disconnects);
            diaglog_write(DiagSeverity::WARN, "wifi", 300, msg,
                          (float)snap.wifi_disconnects);
        }
        if (snap.cpu_temp_c > 70.0f) {
            char msg[80];
            snprintf(msg, sizeof(msg), "CPU temp high: %.1f C", snap.cpu_temp_c);
            diaglog_write(DiagSeverity::WARN, "thermal", 400, msg,
                          snap.cpu_temp_c);
        }
#endif

        // Run anomaly detection after accumulating enough data
        if (_snap_count >= 5) {
            run_anomaly_detection();
        }
    }

    // Periodic SD flush
#if DIAG_HAS_SD
    if (_cfg.log_to_sd && (now - _last_flush_ms >= _cfg.log_flush_interval_ms)) {
        _last_flush_ms = now;
        flush_events_to_sd();
    }
#endif
}

void log(Severity sev, const char* subsystem, const char* fmt, ...) {
    if (!_initialized) return;

    DiagEvent& ev = _events[_event_head];
    ev.timestamp_ms = millis();
    ev.epoch_time = 0;  // Set by NTP if available
    ev.severity = sev;
    ev.value = 0;
    ev.expected_min = 0;
    ev.expected_max = 0;

    strncpy(ev.subsystem, subsystem, sizeof(ev.subsystem) - 1);
    ev.subsystem[sizeof(ev.subsystem) - 1] = '\0';

    va_list args;
    va_start(args, fmt);
    vsnprintf(ev.message, sizeof(ev.message), fmt, args);
    va_end(args);

    // Try to get epoch time from millis-to-epoch offset
    // (set if NTP has synced — check via time())
    time_t now;
    time(&now);
    if (now > 1700000000) {
        ev.epoch_time = (uint32_t)now;
    }

    _event_head = (_event_head + 1) % _cfg.max_events;
    if (_event_count < _cfg.max_events) _event_count++;

    // Serial output with color
    if (_cfg.log_to_serial) {
        Serial.printf("%s[%s] [%s] %s\033[0m\n",
                      severity_color(sev),
                      severity_str(sev),
                      subsystem,
                      ev.message);
    }

    // Persist WARN+ events to diaglog for reboot-surviving storage
#if DIAG_HAS_DIAGLOG
    if (sev >= Severity::WARN) {
        DiagSeverity dl_sev = (sev >= Severity::FATAL)  ? DiagSeverity::CRITICAL
                            : (sev >= Severity::ERROR)  ? DiagSeverity::ERR
                                                        : DiagSeverity::WARN;
        diaglog_write(dl_sev, subsystem, 0, ev.message);
    }
#endif
}

void log_value(const char* subsystem, const char* metric,
               float value, float expected_min, float expected_max) {
    if (!_initialized) return;

    // Check if value is out of range
    bool out_of_range = false;
    if (expected_max > expected_min) {
        out_of_range = (value < expected_min || value > expected_max);
    }

    Severity sev = out_of_range ? Severity::WARN : Severity::DEBUG;
    log(sev, subsystem, "%s=%.3f%s",
        metric, value,
        out_of_range ? " [OUT OF RANGE]" : "");

    // Set the numeric context on the event we just logged
    uint16_t last = (_event_head == 0) ? _cfg.max_events - 1 : _event_head - 1;
    _events[last].value = value;
    _events[last].expected_min = expected_min;
    _events[last].expected_max = expected_max;

    // Auto-flag as anomaly if out of range
    if (out_of_range) {
        char desc[128];
        snprintf(desc, sizeof(desc), "%s=%.3f outside [%.3f, %.3f]",
                 metric, value, expected_min, expected_max);
        float score = 0.5f;
        if (expected_max > expected_min) {
            float range = expected_max - expected_min;
            float deviation = fmaxf(fabsf(value - expected_min),
                                    fabsf(value - expected_max));
            score = fminf(1.0f, deviation / (range * 2.0f));
        }
        add_anomaly(subsystem, desc, score);
    }
}

HealthSnapshot take_snapshot() {
    HealthSnapshot snap = {};

    snap.timestamp_ms = millis();

    // Epoch time
    time_t now;
    time(&now);
    snap.epoch_time = (now > 1700000000) ? (uint32_t)now : 0;

    // Memory
    snap.free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snap.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    snap.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snap.largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    // CPU temperature
    snap.cpu_temp_c = read_cpu_temp();

    // WiFi
    snap.wifi_connected = WiFi.isConnected();
    snap.wifi_rssi = snap.wifi_connected ? WiFi.RSSI() : 0;
    snap.wifi_disconnects = _wifi_disconnects;
    snap.mqtt_disconnects = _mqtt_disconnects;

    // Display
#if DIAG_HAS_DISPLAY
    snap.display_initialized = (display_get_panel() != nullptr);
#else
    snap.display_initialized = false;
#endif
    snap.display_fps = 0;       // Updated by app if it tracks FPS
    snap.display_brightness = 0; // Could read from display HAL

    // I2C health
    snap.i2c_errors = _i2c_errors;
    snap.i2c_devices_found = 0;  // Updated on demand via scan_i2c_bus()
    // Copy per-slave I2C error data
    snap.i2c_slave_count = _i2c_slave_used;
    for (int i = 0; i < _i2c_slave_used && i < HealthSnapshot::MAX_I2C_SLAVES; i++) {
        snap.i2c_slaves[i] = _i2c_slave_table[i];
    }

    // Display timing
    snap.display_frame_time_us = _display_frame_us;
    snap.display_max_frame_us = _display_max_frame_us;

    // Task timing
    snap.loop_time_us = _loop_time_us;
    snap.max_loop_time_us = _max_loop_time_us;

    // System
    snap.uptime_s = millis() / 1000;
    snap.reboot_count = _reboot_count;
    snap.last_reset_reason = _reset_reason;

    // Power — pull from provider if registered
    if (_power_provider) {
        PowerInfo pwr = {};
        if (_power_provider(pwr)) {
            snap.battery_voltage = pwr.battery_voltage;
            snap.battery_percent = pwr.battery_percent;
            snap.charge_current_ma = pwr.charge_current_ma;
            snap.power_source = pwr.power_source;
            snap.pmic_temp_c = pwr.pmic_temp_c;
        }
    } else {
        snap.battery_voltage = 0.0f;
        snap.battery_percent = 0.0f;
        snap.charge_current_ma = 0.0f;
        snap.power_source = 0;
        snap.pmic_temp_c = 0.0f;
    }

    // Touch — pull from provider if registered
    if (_touch_provider) {
        bool avail = false;
        if (_touch_provider(avail)) {
            snap.touch_available = avail;
        }
    }

    // NTP — pull from provider if registered
    if (_ntp_provider) {
        NtpInfo ntp = {};
        if (_ntp_provider(ntp)) {
            snap.ntp_synced = ntp.synced;
            snap.ntp_last_sync_age_s = ntp.last_sync_age_s;
        }
    }

    // Mesh — pull from provider if registered
    if (_mesh_provider) {
        MeshInfo mesh = {};
        if (_mesh_provider(mesh)) {
            snap.mesh_peers = mesh.peer_count;
            snap.mesh_routes = mesh.route_count;
            snap.mesh_tx = mesh.tx_count;
            snap.mesh_rx = mesh.rx_count;
            snap.mesh_tx_fail = mesh.tx_fail;
            snap.mesh_relayed = mesh.relay_count;
        }
    }

    // Camera — pull from provider if registered
    if (_camera_provider) {
        CameraInfo cam = {};
        if (_camera_provider(cam)) {
            snap.camera_available = cam.available;
            snap.camera_frames = cam.frame_count;
            snap.camera_fails = cam.fail_count;
            snap.camera_last_us = cam.last_capture_us;
            snap.camera_max_us = cam.max_capture_us;
            snap.camera_avg_fps = cam.avg_fps;
        }
    }

    return snap;
}

const HealthSnapshot* get_history(int& count) {
    count = _snap_count;
    return _snapshots;
}

const Anomaly* get_anomalies(int& count) {
    count = _anomaly_count;
    return _anomalies;
}

void clear_anomalies() {
    _anomaly_count = 0;
    DBG_DEBUG("diag", "Anomalies cleared");
}

// ── JSON serialization ──────────────────────────────────────────────────────

static int json_escape(char* dst, size_t dst_size,
                       const char* src) {
    // Minimal JSON string escaping for diagnostic messages
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 2; si++) {
        char c = src[si];
        if (c == '"' || c == '\\') {
            if (di + 2 >= dst_size) break;
            dst[di++] = '\\';
            dst[di++] = c;
        } else if (c == '\n') {
            if (di + 2 >= dst_size) break;
            dst[di++] = '\\';
            dst[di++] = 'n';
        } else if (c >= 0x20) {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
    return (int)di;
}

int health_to_json(char* buf, size_t size) {
    HealthSnapshot snap = take_snapshot();

    int pos = snprintf(buf, size,
        "{"
        "\"timestamp_ms\":%lu,"
        "\"epoch\":%lu,"
        "\"memory\":{"
            "\"free_heap\":%lu,"
            "\"min_free_heap\":%lu,"
            "\"free_psram\":%lu,"
            "\"largest_block\":%lu"
        "},"
        "\"power\":{"
            "\"battery_v\":%.2f,"
            "\"battery_pct\":%.1f,"
            "\"charge_ma\":%.1f,"
            "\"source\":%u"
        "},"
        "\"thermal\":{"
            "\"cpu_c\":%.1f,"
            "\"pmic_c\":%.1f"
        "},"
        "\"display\":{"
            "\"initialized\":%s,"
            "\"fps\":%lu,"
            "\"brightness\":%u,"
            "\"frame_us\":%lu,"
            "\"max_frame_us\":%lu"
        "},"
        "\"wifi\":{"
            "\"connected\":%s,"
            "\"rssi\":%d,"
            "\"disconnects\":%lu"
        "},"
        "\"i2c\":{"
            "\"devices\":%u,"
            "\"errors\":%u,"
            "\"slave_count\":%u"
        "},"
        "\"perf\":{"
            "\"loop_us\":%lu,"
            "\"max_loop_us\":%lu"
        "},"
        "\"touch\":{\"available\":%s},"
        "\"ntp\":{\"synced\":%s,\"age_s\":%lu},"
        "\"mesh\":{\"peers\":%u,\"routes\":%u,\"tx\":%lu,\"rx\":%lu,\"tx_fail\":%lu,\"relayed\":%lu},"
        "\"system\":{"
            "\"uptime_s\":%lu,"
            "\"reboot_count\":%lu,"
            "\"reset_reason\":\"%s\""
        "}"
        "}",
        (unsigned long)snap.timestamp_ms,
        (unsigned long)snap.epoch_time,
        (unsigned long)snap.free_heap,
        (unsigned long)snap.min_free_heap,
        (unsigned long)snap.free_psram,
        (unsigned long)snap.largest_free_block,
        snap.battery_voltage, snap.battery_percent,
        snap.charge_current_ma, snap.power_source,
        snap.cpu_temp_c, snap.pmic_temp_c,
        snap.display_initialized ? "true" : "false",
        (unsigned long)snap.display_fps, snap.display_brightness,
        (unsigned long)snap.display_frame_time_us,
        (unsigned long)snap.display_max_frame_us,
        snap.wifi_connected ? "true" : "false",
        (int)snap.wifi_rssi,
        (unsigned long)snap.wifi_disconnects,
        snap.i2c_devices_found, (unsigned)snap.i2c_errors, snap.i2c_slave_count,
        snap.touch_available ? "true" : "false",
        snap.ntp_synced ? "true" : "false",
        (unsigned long)snap.ntp_last_sync_age_s,
        snap.mesh_peers, snap.mesh_routes,
        (unsigned long)snap.mesh_tx, (unsigned long)snap.mesh_rx,
        (unsigned long)snap.mesh_tx_fail, (unsigned long)snap.mesh_relayed,
        (unsigned long)snap.loop_time_us,
        (unsigned long)snap.max_loop_time_us,
        (unsigned long)snap.uptime_s,
        (unsigned long)snap.reboot_count,
        reset_reason_str(snap.last_reset_reason));

    // Append camera metrics if available
    if (snap.camera_available && pos > 0 && pos < (int)size - 2) {
        pos--;  // back up over final '}'
        pos += snprintf(buf + pos, size - pos,
            ",\"camera\":{"
                "\"frames\":%lu,"
                "\"fails\":%lu,"
                "\"last_us\":%lu,"
                "\"max_us\":%lu,"
                "\"avg_fps\":%.1f"
            "}}",
            (unsigned long)snap.camera_frames,
            (unsigned long)snap.camera_fails,
            (unsigned long)snap.camera_last_us,
            (unsigned long)snap.camera_max_us,
            snap.camera_avg_fps);
    }

    // Append mesh peer list if available
    if (_mesh_provider && snap.mesh_peers > 0 && pos > 0 && pos < (int)size - 2) {
        MeshInfo mesh = {};
        if (_mesh_provider(mesh) && mesh.peer_list_count > 0) {
            pos--;  // back up over final '}'
            pos += snprintf(buf + pos, size - pos, ",\"mesh_peers\":[");
            for (int i = 0; i < mesh.peer_list_count && i < MeshInfo::MAX_PEERS
                 && pos < (int)size - 60; i++) {
                if (i > 0) buf[pos++] = ',';
                auto& p = mesh.peers[i];
                pos += snprintf(buf + pos, size - pos,
                    "{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d,\"hops\":%u}",
                    p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5],
                    (int)p.rssi, (unsigned)p.hops);
            }
            pos += snprintf(buf + pos, size - pos, "]}");
        }
    }

    // Append per-slave I2C details if any are tracked
    if (snap.i2c_slave_count > 0 && pos > 0 && pos < (int)size - 2) {
        // Replace trailing '}' with ',i2c_slaves:[...]}'
        pos--;  // back up over final '}'
        pos += snprintf(buf + pos, size - pos, ",\"i2c_slaves\":[");
        for (int i = 0; i < snap.i2c_slave_count && pos < (int)size - 80; i++) {
            if (i > 0) buf[pos++] = ',';
            auto& s = snap.i2c_slaves[i];
            pos += snprintf(buf + pos, size - pos,
                "{\"addr\":\"0x%02X\",\"nack\":%u,\"timeout\":%u,"
                "\"ok\":%u,\"lat_us\":%d}",
                s.addr, s.nack_count, s.timeout_count,
                s.success_count, (int)s.last_latency_us);
        }
        pos += snprintf(buf + pos, size - pos, "]}");
    }

    return pos;
}

int events_to_json(char* buf, size_t size, int last_n) {
    int pos = snprintf(buf, size, "{\"count\":%u,\"events\":[", _event_count);

    if (_event_count == 0 || last_n <= 0) {
        pos += snprintf(buf + pos, size - pos, "]}");
        return pos;
    }

    // Determine range of events to serialize
    uint16_t total = (_event_count < _cfg.max_events) ? _event_count : _cfg.max_events;
    uint16_t n = (last_n < total) ? last_n : total;
    uint16_t start = (_event_head - n + _cfg.max_events) % _cfg.max_events;

    char escaped_msg[256];
    char escaped_sub[32];

    for (uint16_t i = 0; i < n && pos < (int)size - 300; i++) {
        uint16_t idx = (start + i) % _cfg.max_events;
        const DiagEvent& ev = _events[idx];

        json_escape(escaped_msg, sizeof(escaped_msg), ev.message);
        json_escape(escaped_sub, sizeof(escaped_sub), ev.subsystem);

        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, size - pos,
            "{\"t\":%lu,\"epoch\":%lu,\"sev\":\"%s\","
            "\"sub\":\"%s\",\"msg\":\"%s\"",
            (unsigned long)ev.timestamp_ms,
            (unsigned long)ev.epoch_time,
            severity_str(ev.severity),
            escaped_sub,
            escaped_msg);

        if (ev.expected_max > ev.expected_min) {
            pos += snprintf(buf + pos, size - pos,
                ",\"val\":%.3f,\"min\":%.3f,\"max\":%.3f",
                ev.value, ev.expected_min, ev.expected_max);
        }
        pos += snprintf(buf + pos, size - pos, "}");
    }

    pos += snprintf(buf + pos, size - pos, "]}");
    return pos;
}

int anomalies_to_json(char* buf, size_t size) {
    int pos = snprintf(buf, size, "{\"count\":%d,\"anomalies\":[", _anomaly_count);

    char escaped_desc[256];
    char escaped_sub[32];

    for (int i = 0; i < _anomaly_count && pos < (int)size - 300; i++) {
        const Anomaly& a = _anomalies[i];

        json_escape(escaped_desc, sizeof(escaped_desc), a.description);
        json_escape(escaped_sub, sizeof(escaped_sub), a.subsystem);

        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, size - pos,
            "{\"t\":%lu,\"sub\":\"%s\",\"desc\":\"%s\",\"score\":%.2f}",
            (unsigned long)a.timestamp_ms,
            escaped_sub,
            escaped_desc,
            a.severity_score);
    }

    pos += snprintf(buf + pos, size - pos, "]}");
    return pos;
}

int full_report_json(char* buf, size_t size) {
    int pos = snprintf(buf, size, "{\"health\":");

    // Health section
    pos += health_to_json(buf + pos, size - pos);

    // Events section
    pos += snprintf(buf + pos, size - pos, ",\"events\":");
    pos += events_to_json(buf + pos, size - pos, 50);

    // Anomalies section
    pos += snprintf(buf + pos, size - pos, ",\"anomalies\":");
    pos += anomalies_to_json(buf + pos, size - pos);

    // Close the outer object — strip the redundant wrapper keys from
    // events_to_json and anomalies_to_json by embedding them directly.
    // (The sub-calls already produce valid JSON objects, so this nests cleanly.)
    pos += snprintf(buf + pos, size - pos, "}");

    return pos;
}

// ── I2C bus scan ────────────────────────────────────────────────────────────

int scan_i2c_bus(uint8_t port, uint8_t* found_addrs, int max_addrs) {
    int count = 0;

    // Use Arduino Wire for I2C scanning (compatible with ESP-IDF 5.x new driver)
    TwoWire& wire = (port == 1) ? Wire1 : Wire;

    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        wire.beginTransmission(addr);
        uint8_t err = wire.endTransmission();

        if (err == 0) {
            if (count < max_addrs) {
                found_addrs[count] = addr;
            }
            count++;
            DBG_DEBUG("diag", "I2C%d: device at 0x%02X", port, addr);
        }
    }

    DBG_INFO("diag", "I2C%d scan: %d devices found", port, count);
    return count;
}

// ── Display test pattern ────────────────────────────────────────────────────

void test_display_pattern(esp_lcd_panel_handle_t panel, int w, int h) {
    if (!panel || w <= 0 || h <= 0) return;

    // Allocate a single-row buffer for DMA transfer
    // Use internal SRAM for DMA compatibility
    size_t row_bytes = w * sizeof(uint16_t);
    uint16_t* row_buf = (uint16_t*)heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
    if (!row_buf) {
        DBG_ERROR("diag", "Cannot allocate display test row buffer");
        return;
    }

    // RGB565 color constants (byte-swapped for QSPI transport)
    auto swap16 = [](uint16_t c) -> uint16_t { return (c >> 8) | (c << 8); };
    uint16_t red   = swap16(0xF800);
    uint16_t green = swap16(0x07E0);
    uint16_t blue  = swap16(0x001F);
    uint16_t white = swap16(0xFFFF);
    uint16_t black = swap16(0x0000);

    int bar_height = h / 5;

    DBG_INFO("diag", "Display test pattern: %dx%d", w, h);

    // Phase 1: Color bars (red, green, blue, white, black)
    uint16_t colors[] = {red, green, blue, white, black};
    for (int bar = 0; bar < 5; bar++) {
        // Fill row buffer with bar color
        for (int x = 0; x < w; x++) {
            row_buf[x] = colors[bar];
        }
        int y_start = bar * bar_height;
        int y_end = (bar == 4) ? h : y_start + bar_height;
        for (int y = y_start; y < y_end; y++) {
            esp_lcd_panel_draw_bitmap(panel, 0, y, w, y + 1, row_buf);
            vTaskDelay(1);  // Yield for DMA
        }
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    // Phase 2: Checkerboard (4x4 pixel blocks)
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            bool checker = ((x / 4) + (y / 4)) & 1;
            row_buf[x] = checker ? white : black;
        }
        esp_lcd_panel_draw_bitmap(panel, 0, y, w, y + 1, row_buf);
        vTaskDelay(1);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    heap_caps_free(row_buf);
    DBG_INFO("diag", "Display test pattern complete");
}

// ── Reboot tracking ─────────────────────────────────────────────────────────

uint32_t get_reboot_count() {
    return _reboot_count;
}

uint8_t get_last_reset_reason() {
    return _reset_reason;
}

const char* reset_reason_str(uint8_t reason) {
    switch ((esp_reset_reason_t)reason) {
        case ESP_RST_UNKNOWN:   return "UNKNOWN";
        case ESP_RST_POWERON:   return "POWER_ON";
        case ESP_RST_EXT:       return "EXTERNAL";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB";
        default:                return "UNKNOWN";
    }
}

// ── Crash / panic tracking ────────────────────────────────────────────────

static constexpr const char* NVS_KEY_CRASH_VALID = "crash_ok";
static constexpr const char* NVS_KEY_CRASH_EPOCH = "crash_epoch";
static constexpr const char* NVS_KEY_CRASH_UPTIME = "crash_up";
static constexpr const char* NVS_KEY_CRASH_HEAP = "crash_heap";
static constexpr const char* NVS_KEY_CRASH_REASON = "crash_rsn";
static constexpr const char* NVS_KEY_CRASH_MSG = "crash_msg";
static constexpr const char* NVS_KEY_CRASH_TASK = "crash_task";

bool has_crash_info() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t valid = 0;
    esp_err_t err = nvs_get_u8(handle, NVS_KEY_CRASH_VALID, &valid);
    nvs_close(handle);
    return (err == ESP_OK && valid == 1);
}

bool get_crash_info(CrashInfo& out) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    uint8_t valid = 0;
    if (nvs_get_u8(handle, NVS_KEY_CRASH_VALID, &valid) != ESP_OK || valid != 1) {
        nvs_close(handle);
        return false;
    }

    memset(&out, 0, sizeof(out));
    nvs_get_u32(handle, NVS_KEY_CRASH_EPOCH, &out.epoch_time);
    nvs_get_u32(handle, NVS_KEY_CRASH_UPTIME, &out.uptime_ms);
    nvs_get_u32(handle, NVS_KEY_CRASH_HEAP, &out.free_heap);
    nvs_get_u8(handle, NVS_KEY_CRASH_REASON, &out.reset_reason);

    size_t msg_len = sizeof(out.message);
    nvs_get_str(handle, NVS_KEY_CRASH_MSG, out.message, &msg_len);
    size_t task_len = sizeof(out.task_name);
    nvs_get_str(handle, NVS_KEY_CRASH_TASK, out.task_name, &task_len);

    nvs_close(handle);
    return true;
}

void clear_crash_info() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, NVS_KEY_CRASH_VALID, 0);
    nvs_commit(handle);
    nvs_close(handle);
}

void store_crash(const char* message, const char* task_name) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;

    // Get current time if available
    time_t now;
    time(&now);
    uint32_t epoch = (now > 1700000000) ? (uint32_t)now : 0;

    nvs_set_u8(handle, NVS_KEY_CRASH_VALID, 1);
    nvs_set_u32(handle, NVS_KEY_CRASH_EPOCH, epoch);
    nvs_set_u32(handle, NVS_KEY_CRASH_UPTIME, millis());
    nvs_set_u32(handle, NVS_KEY_CRASH_HEAP,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    nvs_set_u8(handle, NVS_KEY_CRASH_REASON, (uint8_t)esp_reset_reason());

    if (message) {
        nvs_set_str(handle, NVS_KEY_CRASH_MSG, message);
    }
    if (task_name) {
        nvs_set_str(handle, NVS_KEY_CRASH_TASK, task_name);
    } else {
        // Try to get current task name
        TaskHandle_t th = xTaskGetCurrentTaskHandle();
        if (th) {
            nvs_set_str(handle, NVS_KEY_CRASH_TASK, pcTaskGetName(th));
        }
    }

    nvs_commit(handle);
    nvs_close(handle);
}

}  // namespace hal_diag

#endif  // SIMULATOR
