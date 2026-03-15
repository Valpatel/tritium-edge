// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_autonomous_alert.h"
#include <Arduino.h>
#include <Preferences.h>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace hal_autonomous_alert {

static bool s_initialized = false;
static AutonomousAlertConfig s_config;
static AlertRule s_rules[MAX_RULES];
static int s_rule_count = 0;
static uint32_t s_last_check_ms = 0;

// Stats
static uint32_t s_total_decisions = 0;
static uint32_t s_decisions_by_trigger[8] = {};

// Pending sensor data for rule evaluation
struct PendingBle {
    char mac[18];
    int8_t rssi;
    bool is_known;
    bool valid;
};

struct PendingMotion {
    char peer_mac[18];
    float variance;
    bool motion;
    bool valid;
};

struct PendingAcoustic {
    char event_type[32];
    float confidence;
    bool valid;
};

static PendingBle s_pending_ble = {};
static PendingMotion s_pending_motion = {};
static PendingAcoustic s_pending_acoustic = {};

// NVS namespace
static const char* NVS_NAMESPACE = "auto_alert";
static Preferences s_prefs;

static bool publish_decision(const AlertRule& rule, const char* target_id,
                             float measured_value) {
    if (!s_config.publish_fn) return false;

    char json[512];
    uint32_t now_ms = millis();
    const char* trigger_str = "unknown_ble";
    switch (rule.trigger) {
        case TriggerType::UNKNOWN_BLE:       trigger_str = "unknown_ble"; break;
        case TriggerType::THREAT_FEED_MATCH: trigger_str = "threat_feed_match"; break;
        case TriggerType::MOTION_DETECTED:   trigger_str = "motion_detected"; break;
        case TriggerType::ACOUSTIC_EVENT:    trigger_str = "acoustic_event"; break;
        case TriggerType::RSSI_ANOMALY:      trigger_str = "rssi_anomaly"; break;
        case TriggerType::GEOFENCE_BREACH:   trigger_str = "geofence_breach"; break;
        case TriggerType::SIGNAL_STRENGTH:   trigger_str = "signal_strength"; break;
        case TriggerType::PATTERN_MATCH:     trigger_str = "pattern_match"; break;
    }

    const char* decision_str = "alert";
    switch (rule.decision_type) {
        case DecisionType::ALERT:    decision_str = "alert"; break;
        case DecisionType::CLASSIFY: decision_str = "classify"; break;
        case DecisionType::ESCALATE: decision_str = "escalate"; break;
        case DecisionType::LOCKDOWN: decision_str = "lockdown"; break;
        case DecisionType::EVADE:    decision_str = "evade"; break;
        case DecisionType::REPORT:   decision_str = "report"; break;
    }

    int len = snprintf(json, sizeof(json),
        "{\"decision_id\":\"auto_%lu\","
        "\"decision_type\":\"%s\","
        "\"trigger\":\"%s\","
        "\"confidence\":%.2f,"
        "\"action_taken\":\"publish_alert\","
        "\"target_id\":\"%s\","
        "\"threshold_value\":%.1f,"
        "\"measured_value\":%.1f,"
        "\"rule_name\":\"%s\","
        "\"sc_override\":\"pending\"}",
        (unsigned long)now_ms,
        decision_str,
        trigger_str,
        rule.min_confidence,
        target_id,
        rule.threshold,
        measured_value,
        rule.name
    );

    if (len <= 0 || (size_t)len >= sizeof(json)) return false;

    s_total_decisions++;
    s_decisions_by_trigger[static_cast<uint8_t>(rule.trigger)]++;

    return s_config.publish_fn(json, (size_t)len);
}

static void evaluate_rules() {
    uint32_t now_ms = millis();

    for (int i = 0; i < s_rule_count; i++) {
        AlertRule& rule = s_rules[i];
        if (!rule.enabled) continue;

        // Check cooldown
        if (rule.last_fired_ms > 0 &&
            (now_ms - rule.last_fired_ms) < (uint32_t)(rule.cooldown_seconds * 1000)) {
            continue;
        }

        bool fired = false;

        switch (rule.trigger) {
            case TriggerType::UNKNOWN_BLE:
            case TriggerType::SIGNAL_STRENGTH:
                if (s_pending_ble.valid && !s_pending_ble.is_known) {
                    // RSSI threshold: trigger if signal is STRONGER than threshold
                    // (less negative = closer = more concerning)
                    if ((float)s_pending_ble.rssi > rule.threshold) {
                        fired = publish_decision(rule, s_pending_ble.mac,
                                               (float)s_pending_ble.rssi);
                    }
                }
                break;

            case TriggerType::MOTION_DETECTED:
            case TriggerType::RSSI_ANOMALY:
                if (s_pending_motion.valid && s_pending_motion.motion) {
                    if (s_pending_motion.variance > rule.threshold) {
                        fired = publish_decision(rule, s_pending_motion.peer_mac,
                                               s_pending_motion.variance);
                    }
                }
                break;

            case TriggerType::ACOUSTIC_EVENT:
                if (s_pending_acoustic.valid) {
                    if (s_pending_acoustic.confidence >= rule.min_confidence) {
                        fired = publish_decision(rule, s_pending_acoustic.event_type,
                                               s_pending_acoustic.confidence);
                    }
                }
                break;

            default:
                break;
        }

        if (fired) {
            rule.last_fired_ms = now_ms;
        }
    }

    // Clear pending data after evaluation
    s_pending_ble.valid = false;
    s_pending_motion.valid = false;
    s_pending_acoustic.valid = false;
}

bool init(const AutonomousAlertConfig& config) {
    s_config = config;
    s_rule_count = 0;
    s_total_decisions = 0;
    memset(s_decisions_by_trigger, 0, sizeof(s_decisions_by_trigger));
    s_last_check_ms = millis();

    // Load rules from NVS
    load_rules_from_nvs();

    // If no rules configured, create defaults
    if (s_rule_count == 0 && s_config.enabled) {
        AlertRule ble_rule = {};
        strncpy(ble_rule.rule_id, "ble_unknown", sizeof(ble_rule.rule_id) - 1);
        strncpy(ble_rule.name, "Strong Unknown BLE", sizeof(ble_rule.name) - 1);
        ble_rule.enabled = true;
        ble_rule.trigger = TriggerType::UNKNOWN_BLE;
        ble_rule.decision_type = DecisionType::ALERT;
        ble_rule.threshold = DEFAULT_BLE_RSSI_THRESHOLD;
        ble_rule.min_confidence = 0.7f;
        ble_rule.cooldown_seconds = DEFAULT_COOLDOWN_SECONDS;
        ble_rule.last_fired_ms = 0;
        add_rule(ble_rule);

        AlertRule motion_rule = {};
        strncpy(motion_rule.rule_id, "rf_motion", sizeof(motion_rule.rule_id) - 1);
        strncpy(motion_rule.name, "RF Motion Alert", sizeof(motion_rule.name) - 1);
        motion_rule.enabled = true;
        motion_rule.trigger = TriggerType::MOTION_DETECTED;
        motion_rule.decision_type = DecisionType::ALERT;
        motion_rule.threshold = DEFAULT_MOTION_VARIANCE_THRESHOLD;
        motion_rule.min_confidence = 0.6f;
        motion_rule.cooldown_seconds = DEFAULT_COOLDOWN_SECONDS;
        motion_rule.last_fired_ms = 0;
        add_rule(motion_rule);

        AlertRule acoustic_rule = {};
        strncpy(acoustic_rule.rule_id, "acoustic", sizeof(acoustic_rule.rule_id) - 1);
        strncpy(acoustic_rule.name, "Acoustic Event", sizeof(acoustic_rule.name) - 1);
        acoustic_rule.enabled = true;
        acoustic_rule.trigger = TriggerType::ACOUSTIC_EVENT;
        acoustic_rule.decision_type = DecisionType::ALERT;
        acoustic_rule.threshold = 0.0f;
        acoustic_rule.min_confidence = 0.8f;
        acoustic_rule.cooldown_seconds = 60.0f;
        acoustic_rule.last_fired_ms = 0;
        add_rule(acoustic_rule);

        save_rules_to_nvs();
    }

    s_initialized = true;
    return true;
}

void deinit() {
    save_rules_to_nvs();
    s_initialized = false;
}

void tick() {
    if (!s_initialized || !s_config.enabled) return;

    uint32_t now_ms = millis();
    if ((now_ms - s_last_check_ms) < s_config.check_interval_ms) return;
    s_last_check_ms = now_ms;

    evaluate_rules();
}

bool is_active() {
    return s_initialized && s_config.enabled;
}

int add_rule(const AlertRule& rule) {
    if (s_rule_count >= MAX_RULES) return -1;
    s_rules[s_rule_count] = rule;
    return s_rule_count++;
}

bool remove_rule(const char* rule_id) {
    for (int i = 0; i < s_rule_count; i++) {
        if (strncmp(s_rules[i].rule_id, rule_id, sizeof(s_rules[i].rule_id)) == 0) {
            // Shift remaining rules down
            for (int j = i; j < s_rule_count - 1; j++) {
                s_rules[j] = s_rules[j + 1];
            }
            s_rule_count--;
            return true;
        }
    }
    return false;
}

bool enable_rule(const char* rule_id, bool enabled) {
    for (int i = 0; i < s_rule_count; i++) {
        if (strncmp(s_rules[i].rule_id, rule_id, sizeof(s_rules[i].rule_id)) == 0) {
            s_rules[i].enabled = enabled;
            return true;
        }
    }
    return false;
}

int get_rule_count() {
    return s_rule_count;
}

int get_rules(AlertRule* out, int max_count) {
    int count = (s_rule_count < max_count) ? s_rule_count : max_count;
    memcpy(out, s_rules, count * sizeof(AlertRule));
    return count;
}

bool load_rules_from_nvs() {
    s_prefs.begin(NVS_NAMESPACE, true);
    s_rule_count = s_prefs.getInt("count", 0);
    if (s_rule_count > MAX_RULES) s_rule_count = MAX_RULES;

    for (int i = 0; i < s_rule_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "rule_%d", i);
        size_t len = s_prefs.getBytes(key, &s_rules[i], sizeof(AlertRule));
        if (len != sizeof(AlertRule)) {
            s_rule_count = i;
            break;
        }
    }
    s_prefs.end();
    return true;
}

bool save_rules_to_nvs() {
    s_prefs.begin(NVS_NAMESPACE, false);
    s_prefs.putInt("count", s_rule_count);
    for (int i = 0; i < s_rule_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "rule_%d", i);
        s_prefs.putBytes(key, &s_rules[i], sizeof(AlertRule));
    }
    s_prefs.end();
    return true;
}

void feed_ble_sighting(const char* mac_str, int8_t rssi, bool is_known) {
    strncpy(s_pending_ble.mac, mac_str, sizeof(s_pending_ble.mac) - 1);
    s_pending_ble.mac[sizeof(s_pending_ble.mac) - 1] = '\0';
    s_pending_ble.rssi = rssi;
    s_pending_ble.is_known = is_known;
    s_pending_ble.valid = true;
}

void feed_rf_motion(const char* peer_mac, float variance, bool motion) {
    strncpy(s_pending_motion.peer_mac, peer_mac, sizeof(s_pending_motion.peer_mac) - 1);
    s_pending_motion.peer_mac[sizeof(s_pending_motion.peer_mac) - 1] = '\0';
    s_pending_motion.variance = variance;
    s_pending_motion.motion = motion;
    s_pending_motion.valid = true;
}

void feed_acoustic_event(const char* event_type, float confidence) {
    strncpy(s_pending_acoustic.event_type, event_type,
            sizeof(s_pending_acoustic.event_type) - 1);
    s_pending_acoustic.event_type[sizeof(s_pending_acoustic.event_type) - 1] = '\0';
    s_pending_acoustic.confidence = confidence;
    s_pending_acoustic.valid = true;
}

uint32_t get_total_decisions() {
    return s_total_decisions;
}

uint32_t get_decisions_by_trigger(TriggerType trigger) {
    uint8_t idx = static_cast<uint8_t>(trigger);
    if (idx >= 8) return 0;
    return s_decisions_by_trigger[idx];
}

int get_stats_json(char* buf, size_t size) {
    return snprintf(buf, size,
        "{\"autonomous_alerts\":{\"total\":%lu,\"rules\":%d,"
        "\"active\":%s,"
        "\"ble_alerts\":%lu,\"motion_alerts\":%lu,\"acoustic_alerts\":%lu}}",
        (unsigned long)s_total_decisions,
        s_rule_count,
        s_initialized ? "true" : "false",
        (unsigned long)s_decisions_by_trigger[0],
        (unsigned long)s_decisions_by_trigger[2],
        (unsigned long)s_decisions_by_trigger[3]
    );
}

}  // namespace hal_autonomous_alert
