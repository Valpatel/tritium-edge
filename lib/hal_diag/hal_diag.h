#pragma once
// Remote diagnostics and data logging HAL for fleet management.
//
// Collects health snapshots, logs diagnostic events, detects anomalies,
// and exports everything as JSON for web API and fleet server aggregation.
//
// Usage:
//   #include "hal_diag.h"
//
//   // In services_init():
//   hal_diag::DiagConfig cfg;
//   cfg.health_interval_ms = 30000;
//   hal_diag::init(cfg);
//
//   // In services_tick():
//   hal_diag::tick();
//
//   // From any subsystem:
//   hal_diag::log(hal_diag::Severity::WARN, "power", "Battery low: %.2fV", voltage);
//   hal_diag::log_value("imu", "accel_x", ax, -2.0f, 2.0f);
//
// Web API endpoints (register via hal_webserver):
//   GET /api/diag/health    -> current health snapshot as JSON
//   GET /api/diag/events    -> recent diagnostic events as JSON
//   GET /api/diag/anomalies -> active anomalies as JSON
//   GET /api/diag/report    -> full diagnostic report as JSON
//
// Fleet server aggregation:
//   Poll /api/diag/report from each node periodically. Compare heap trends,
//   battery curves, WiFi RSSI, and anomaly lists across the fleet to identify
//   hardware failures, deployment issues, and environmental problems.

#include <cstdint>
#include <cstddef>

// Forward-declare esp_lcd_panel_handle_t to avoid pulling in esp_lcd headers
// in every compilation unit that includes hal_diag.h.
typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;

namespace hal_diag {

// ── Severity levels ─────────────────────────────────────────────────────────

enum class Severity : uint8_t {
    TRACE  = 0,
    DEBUG  = 1,
    INFO   = 2,
    WARN   = 3,
    ERROR  = 4,
    FATAL  = 5
};

// ── Diagnostic event ────────────────────────────────────────────────────────

struct DiagEvent {
    uint32_t timestamp_ms;      // millis()
    uint32_t epoch_time;        // UTC seconds (from NTP if available)
    Severity severity;
    char     subsystem[16];     // "display", "power", "imu", "wifi", "ble", etc.
    char     message[128];
    // Numeric context for anomaly detection
    float    value;             // Optional measured value
    float    expected_min;      // Expected range lower bound
    float    expected_max;      // Expected range upper bound
};

// ── Hardware health snapshot (taken periodically) ───────────────────────────

struct HealthSnapshot {
    uint32_t timestamp_ms;
    uint32_t epoch_time;
    // Memory
    uint32_t free_heap;
    uint32_t min_free_heap;     // Watermark since boot
    uint32_t free_psram;
    uint32_t largest_free_block;
    // Power
    float    battery_voltage;
    float    battery_percent;
    float    charge_current_ma;
    uint8_t  power_source;      // 0=unknown, 1=USB, 2=battery
    // Temperature
    float    cpu_temp_c;        // ESP32 internal temp sensor
    float    pmic_temp_c;       // From AXP2101 if available
    // Display
    bool     display_initialized;
    uint32_t display_fps;
    uint8_t  display_brightness;
    // Connectivity
    int8_t   wifi_rssi;
    bool     wifi_connected;
    uint32_t wifi_disconnects;  // Counter since boot
    uint32_t mqtt_disconnects;
    // I2C bus health
    uint8_t  i2c_devices_found;
    uint16_t i2c_errors;        // Total NACK/timeout counter (wider than uint8_t)
    // Per-slave I2C error tracking (up to 8 monitored slaves)
    static constexpr int MAX_I2C_SLAVES = 8;
    struct I2cSlaveHealth {
        uint8_t  addr;           // I2C address (0 = unused slot)
        uint16_t nack_count;     // NACK errors
        uint16_t timeout_count;  // Timeout errors
        uint16_t success_count;  // Successful transactions
        int16_t  last_latency_us; // Last transaction latency
    };
    I2cSlaveHealth i2c_slaves[MAX_I2C_SLAVES] = {};
    uint8_t  i2c_slave_count;    // Number of monitored slaves
    // Display timing
    uint32_t display_frame_time_us; // Last frame push latency
    uint32_t display_max_frame_us;  // Worst frame push latency since boot
    // Camera
    bool     camera_available;
    uint32_t camera_frames;         // Total frames captured
    uint32_t camera_fails;          // Total capture failures
    uint32_t camera_last_us;        // Last capture latency
    uint32_t camera_max_us;         // Worst capture latency
    float    camera_avg_fps;        // Average FPS since init
    // Task stats
    uint32_t loop_time_us;      // Last loop() duration
    uint32_t max_loop_time_us;  // Worst case since boot
    uint32_t uptime_s;
    uint32_t reboot_count;      // Stored in NVS
    uint8_t  last_reset_reason; // ESP reset reason code
};

// ── Anomaly — auto-detected from health trends ─────────────────────────────

struct Anomaly {
    uint32_t timestamp_ms;
    char     subsystem[16];
    char     description[128];
    float    severity_score;    // 0.0 (minor) to 1.0 (critical)
};

// ── Configuration ───────────────────────────────────────────────────────────

struct DiagConfig {
    uint32_t    health_interval_ms      = 30000;    // Snapshot every 30s
    uint32_t    log_flush_interval_ms   = 60000;    // Flush to SD every 60s
    uint16_t    max_events              = 1000;     // Ring buffer size
    uint16_t    max_snapshots           = 500;      // Health history
    bool        log_to_sd               = true;     // Persist to SD card
    bool        log_to_serial           = true;     // Echo to serial
    bool        anomaly_detection       = true;     // Auto-detect anomalies
    const char* sd_log_path             = "/tritium/diag";
};

// ── Power data provider (optional — wired from main.cpp) ────────────────────

struct PowerInfo {
    float battery_voltage;
    float battery_percent;
    float charge_current_ma;
    uint8_t power_source;   // 0=unknown, 1=USB, 2=battery
    float pmic_temp_c;
};

/// Callback type: fill PowerInfo and return true, or return false if unavailable.
using PowerProvider = bool (*)(PowerInfo& out);

/// Register a power data provider so take_snapshot() includes battery/PMIC data.
void set_power_provider(PowerProvider provider);

// ── Camera data provider (optional — wired from main.cpp) ────────────────────

struct CameraInfo {
    bool     available;
    uint32_t frame_count;
    uint32_t fail_count;
    uint32_t last_capture_us;
    uint32_t max_capture_us;
    float    avg_fps;
};

/// Callback type: fill CameraInfo and return true, or return false if unavailable.
using CameraProvider = bool (*)(CameraInfo& out);

/// Register a camera data provider so take_snapshot() includes camera metrics.
void set_camera_provider(CameraProvider provider);

// ── Loop timing provider (optional — wired from watchdog or main loop) ──────

/// Feed loop timing data to the diagnostics system.
void report_loop_time(uint32_t loop_us);

// ── Lifecycle ───────────────────────────────────────────────────────────────

/// Initialize diagnostics. Call once from services_init().
bool init(const DiagConfig& cfg = DiagConfig{});

/// Call from loop() / services_tick(). Takes snapshots, flushes logs,
/// runs anomaly detection on schedule.
void tick();

// ── Logging API (call from any subsystem) ───────────────────────────────────

/// Log a diagnostic event with printf-style formatting.
void log(Severity sev, const char* subsystem, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

/// Log a named metric value with optional expected range.
/// Out-of-range values are automatically flagged as anomalies.
void log_value(const char* subsystem, const char* metric,
               float value, float expected_min = 0, float expected_max = 0);

// ── Health ──────────────────────────────────────────────────────────────────

/// Take an immediate health snapshot (also called automatically by tick()).
HealthSnapshot take_snapshot();

/// Get read-only pointer to the health history ring buffer.
/// @param count [out] Number of valid entries in the buffer.
/// @return Pointer to the oldest entry. Entries are in chronological order
///         from index 0 to count-1.
const HealthSnapshot* get_history(int& count);

// ── Anomaly detection ───────────────────────────────────────────────────────

/// Get active anomaly list.
/// @param count [out] Number of active anomalies.
/// @return Pointer to anomaly array.
const Anomaly* get_anomalies(int& count);

/// Clear all active anomalies.
void clear_anomalies();

// ── Data export (for web API and fleet server) ──────────────────────────────

/// Serialize current health snapshot to JSON.
/// @return Number of bytes written, or -1 on error.
int health_to_json(char* buf, size_t size);

/// Serialize recent events to JSON.
/// @param last_n Number of most recent events to include.
/// @return Number of bytes written, or -1 on error.
int events_to_json(char* buf, size_t size, int last_n = 50);

/// Serialize active anomalies to JSON.
/// @return Number of bytes written, or -1 on error.
int anomalies_to_json(char* buf, size_t size);

/// Full diagnostic report: health + events + anomalies combined.
/// @return Number of bytes written, or -1 on error.
int full_report_json(char* buf, size_t size);

// ── Diagnostic tools ────────────────────────────────────────────────────────

/// Scan an I2C bus for responsive devices.
/// @param port I2C port number (0 or 1).
/// @param found_addrs [out] Array to fill with discovered 7-bit addresses.
/// @param max_addrs Size of found_addrs array.
/// @return Number of devices found.
int scan_i2c_bus(uint8_t port, uint8_t* found_addrs, int max_addrs);

/// Display test pattern: red/green/blue/white bars + checkerboard.
/// Useful for dead pixel detection and display driver verification.
/// @param panel esp_lcd panel handle.
/// @param w Display width in pixels.
/// @param h Display height in pixels.
void test_display_pattern(esp_lcd_panel_handle_t panel, int w, int h);

// ── Reboot tracking (NVS-backed) ───────────────────────────────────────────

/// Get the total reboot count (persisted across reboots in NVS).
uint32_t get_reboot_count();

/// Get the ESP32 reset reason code for the most recent boot.
uint8_t get_last_reset_reason();

/// Convert a reset reason code to a human-readable string.
const char* reset_reason_str(uint8_t reason);

// ── I2C per-slave error reporting ────────────────────────────────────────

/// Report an I2C transaction result for a specific slave address.
/// Call from any HAL that uses I2C (power, imu, rtc, touch, etc.)
/// @param addr I2C 7-bit slave address.
/// @param success True if transaction succeeded, false on NACK/timeout.
/// @param is_timeout True if failure was a timeout (vs NACK).
/// @param latency_us Transaction duration in microseconds.
void report_i2c_result(uint8_t addr, bool success, bool is_timeout = false,
                       int16_t latency_us = 0);

// ── Display timing reporting ─────────────────────────────────────────────

/// Report a display frame push duration.
/// Call after pushSprite() or pushImage() completes.
/// @param frame_us Duration of the display write in microseconds.
void report_display_frame(uint32_t frame_us);

// ── Crash / panic tracking (NVS-backed) ──────────────────────────────────

/// Crash info stored in NVS for post-mortem analysis.
struct CrashInfo {
    uint32_t epoch_time;         // UTC timestamp of crash (0 if NTP unavailable)
    uint32_t uptime_ms;          // Uptime at crash
    uint32_t free_heap;          // Heap at crash
    uint8_t  reset_reason;       // ESP reset reason
    char     message[64];        // Crash description
    char     task_name[16];      // FreeRTOS task that crashed
};

/// Check if a crash was recorded from the previous boot.
/// @return true if crash info is available.
bool has_crash_info();

/// Get the crash info from the previous boot.
/// @param out [out] Filled with crash data if available.
/// @return true if crash info was retrieved.
bool get_crash_info(CrashInfo& out);

/// Clear the stored crash info (called after it has been reported to server).
void clear_crash_info();

/// Store crash info to NVS. Called from panic handler or watchdog timeout.
/// This is safe to call from ISR context (uses NVS directly, no heap alloc).
void store_crash(const char* message, const char* task_name = nullptr);

}  // namespace hal_diag
