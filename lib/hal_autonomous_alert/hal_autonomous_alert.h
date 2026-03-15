// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0
#pragma once

// Autonomous Alert HAL — local threshold-based alerting without SC.
//
// When certain conditions are met locally (strong unknown BLE device,
// motion detected via RF monitor, acoustic event), the edge device
// immediately publishes an alert to MQTT without waiting for SC
// classification. Rules are stored in NVS and can be updated via
// MQTT commands from SC.
//
// This enables edge devices to react in real-time even when SC is
// offline or the network is congested. SC receives the autonomous
// decision and can confirm or override it later.
//
// MQTT output: tritium/{device_id}/alert/autonomous
// JSON format: {
//   "decision_id": "auto_1710403200000",
//   "device_id": "tritium-43c-001",
//   "decision_type": "alert",
//   "trigger": "unknown_ble",
//   "confidence": 0.85,
//   "action_taken": "publish_alert",
//   "target_id": "ble_AA:BB:CC:DD:EE:FF",
//   "threshold_value": -60,
//   "measured_value": -45,
//   "sc_override": "pending"
// }

#include <cstdint>
#include <cstddef>

namespace hal_autonomous_alert {

static constexpr int MAX_RULES = 16;
static constexpr float DEFAULT_BLE_RSSI_THRESHOLD = -60.0f;  // dBm
static constexpr float DEFAULT_MOTION_VARIANCE_THRESHOLD = 8.0f;
static constexpr float DEFAULT_COOLDOWN_SECONDS = 30.0f;

// Trigger types matching tritium-lib AutonomousTrigger enum
enum class TriggerType : uint8_t {
    UNKNOWN_BLE = 0,
    THREAT_FEED_MATCH = 1,
    MOTION_DETECTED = 2,
    ACOUSTIC_EVENT = 3,
    RSSI_ANOMALY = 4,
    GEOFENCE_BREACH = 5,
    SIGNAL_STRENGTH = 6,
    PATTERN_MATCH = 7,
};

// Decision types matching tritium-lib AutonomousDecisionType
enum class DecisionType : uint8_t {
    ALERT = 0,
    CLASSIFY = 1,
    ESCALATE = 2,
    LOCKDOWN = 3,
    EVADE = 4,
    REPORT = 5,
};

// A local alert rule stored in NVS
struct AlertRule {
    char     rule_id[16];
    char     name[32];
    bool     enabled;
    TriggerType trigger;
    DecisionType decision_type;
    float    threshold;         // Trigger when measured_value crosses this
    float    min_confidence;    // Minimum confidence to fire
    float    cooldown_seconds;  // Minimum time between alerts for this rule
    uint32_t last_fired_ms;     // millis() when last fired
};

// Configuration
struct AutonomousAlertConfig {
    bool     enabled = true;
    uint32_t check_interval_ms = 1000;  // How often to evaluate rules
    // Callback: publish alert JSON to MQTT
    // Returns true if publish succeeded
    typedef bool (*PublishCallback)(const char* json, size_t len);
    PublishCallback publish_fn = nullptr;
};

// Initialize the autonomous alert system. Call after MQTT and sensors are ready.
bool init(const AutonomousAlertConfig& config = {});

// Shutdown and free resources.
void deinit();

// Call from main loop. Evaluates rules against current sensor state.
void tick();

// Check if the alert system is active.
bool is_active();

// -- Rule management (NVS-backed) --

// Add a new alert rule. Returns rule index or -1 on failure.
int add_rule(const AlertRule& rule);

// Remove a rule by ID. Returns true if found and removed.
bool remove_rule(const char* rule_id);

// Enable/disable a rule by ID.
bool enable_rule(const char* rule_id, bool enabled);

// Get number of configured rules.
int get_rule_count();

// Get all rules. Returns number written to output array.
int get_rules(AlertRule* out, int max_count);

// Load rules from NVS. Called automatically during init().
bool load_rules_from_nvs();

// Save current rules to NVS.
bool save_rules_to_nvs();

// -- Sensor input feeds --
// These functions feed current sensor data to the rule evaluator.
// Call them from the appropriate HAL tick functions.

// Feed BLE sighting: unknown device with given RSSI
void feed_ble_sighting(const char* mac_str, int8_t rssi, bool is_known);

// Feed RF motion detection result
void feed_rf_motion(const char* peer_mac, float variance, bool motion);

// Feed acoustic event
void feed_acoustic_event(const char* event_type, float confidence);

// -- Stats --

// Get total autonomous decisions made since boot.
uint32_t get_total_decisions();

// Get total decisions by type.
uint32_t get_decisions_by_trigger(TriggerType trigger);

// Serialize stats to JSON for heartbeat inclusion.
int get_stats_json(char* buf, size_t size);

}  // namespace hal_autonomous_alert
