// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_cot.h"
#include "debug_log.h"

#include <cstring>
#include <cstdio>
#include <ctime>

static constexpr const char* TAG = "cot";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace hal_cot {

static bool _active = false;
static uint32_t _interval_ms = 60000;

bool init(const CotConfig& config) {
    (void)config;
    DBG_INFO(TAG, "CoT init (simulator stub)");
    return false;
}

void set_position(double lat, double lon, float hae) {
    (void)lat; (void)lon; (void)hae;
}

bool tick() { return false; }
bool is_active() { return false; }

int build_position_event(char* buf, size_t buf_size) {
    (void)buf; (void)buf_size;
    return 0;
}

int build_sensor_event(char* buf, size_t buf_size,
                       const char* sensor_type, float value, const char* unit) {
    (void)buf; (void)buf_size; (void)sensor_type; (void)value; (void)unit;
    return 0;
}

int build_status_event(char* buf, size_t buf_size) {
    (void)buf; (void)buf_size;
    return 0;
}

bool send_multicast(const char* xml, size_t len) {
    (void)xml; (void)len;
    return false;
}

bool send_tcp(const char* host, uint16_t port, const char* xml, size_t len) {
    (void)host; (void)port; (void)xml; (void)len;
    return false;
}

const char* format_iso8601(char* buf, size_t buf_size, uint32_t epoch) {
    time_t t = (time_t)epoch;
    struct tm* tm = gmtime(&t);
    if (!tm || buf_size < 25) return nullptr;
    snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
             1900 + tm->tm_year, 1 + tm->tm_mon, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    return buf;
}

uint32_t get_epoch() {
    return (uint32_t)time(nullptr);
}

}  // namespace hal_cot

// ============================================================================
// ESP32 — real CoT implementation
// ============================================================================
#else

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

namespace hal_cot {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static bool _initialized = false;
static bool _active = false;
static uint32_t _interval_ms = 60000;
static uint32_t _stale_seconds = 300;
static uint32_t _last_send_ms = 0;

// Device identity
static char _device_id[64] = {};
static char _callsign[32] = {};
static char _cot_type[32] = {};
static char _team[16] = {};
static char _role[16] = {};
static char _how[8] = {};
static float _ce = 10.0f;
static float _le = 10.0f;

// Current position
static double _lat = 0.0;
static double _lon = 0.0;
static float _hae = 0.0f;
static bool _has_position = false;

// UDP socket (reused across sends)
static WiFiUDP _udp;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* format_iso8601(char* buf, size_t buf_size, uint32_t epoch) {
    if (buf_size < 25) return nullptr;
    time_t t = (time_t)epoch;
    struct tm timeinfo;
    gmtime_r(&t, &timeinfo);
    snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
             1900 + timeinfo.tm_year, 1 + timeinfo.tm_mon, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return buf;
}

uint32_t get_epoch() {
    time_t now;
    time(&now);
    // Sanity check: if year < 2025, time is not synced
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    if (timeinfo.tm_year + 1900 < 2025) return 0;
    return (uint32_t)now;
}

static void derive_identity_from_mac() {
    uint8_t mac[6];
    WiFi.macAddress(mac);

    if (_device_id[0] == '\0') {
        snprintf(_device_id, sizeof(_device_id),
                 "esp32-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    if (_callsign[0] == '\0') {
        snprintf(_callsign, sizeof(_callsign),
                 "TRITIUM-%02X%02X", mac[4], mac[5]);
    }
}

// XML-escape a string into buf. Returns bytes written.
static int xml_escape(char* buf, size_t buf_size, const char* src) {
    int pos = 0;
    for (const char* p = src; *p && pos < (int)buf_size - 6; p++) {
        switch (*p) {
            case '&':  pos += snprintf(buf + pos, buf_size - pos, "&amp;"); break;
            case '<':  pos += snprintf(buf + pos, buf_size - pos, "&lt;"); break;
            case '>':  pos += snprintf(buf + pos, buf_size - pos, "&gt;"); break;
            case '"':  pos += snprintf(buf + pos, buf_size - pos, "&quot;"); break;
            case '\'': pos += snprintf(buf + pos, buf_size - pos, "&apos;"); break;
            default:   buf[pos++] = *p; break;
        }
    }
    if (pos < (int)buf_size) buf[pos] = '\0';
    return pos;
}

// ---------------------------------------------------------------------------
// XML Builders
// ---------------------------------------------------------------------------

int build_position_event(char* buf, size_t buf_size) {
    if (!_initialized) return 0;

    uint32_t epoch = get_epoch();
    if (epoch == 0) {
        DBG_WARN(TAG, "Time not synced, cannot build CoT");
        return 0;
    }

    char time_str[32], start_str[32], stale_str[32];
    format_iso8601(time_str, sizeof(time_str), epoch);
    format_iso8601(start_str, sizeof(start_str), epoch);
    format_iso8601(stale_str, sizeof(stale_str), epoch + _stale_seconds);

    // Escape callsign for XML safety
    char safe_callsign[64];
    xml_escape(safe_callsign, sizeof(safe_callsign), _callsign);

    // Build CoT XML — matches tritium-lib format exactly:
    //   <event version="2.0" uid="tritium-edge-{id}" type="{type}" how="{how}"
    //          time="{t}" start="{t}" stale="{stale}">
    //     <point lat="{lat}" lon="{lon}" hae="{alt}" ce="{ce}" le="{le}"/>
    //     <detail>
    //       <contact callsign="{callsign}"/>
    //       <__group name="{team}" role="{role}"/>
    //       <status battery="{bat}" />
    //       <tritium_edge device_id="{id}" role="{role}"/>
    //     </detail>
    //   </event>
    int n = snprintf(buf, buf_size,
        "<?xml version='1.0' encoding='UTF-8'?>"
        "<event version=\"2.0\""
        " uid=\"tritium-edge-%s\""
        " type=\"%s\""
        " how=\"%s\""
        " time=\"%s\""
        " start=\"%s\""
        " stale=\"%s\">"
        "<point lat=\"%.7f\" lon=\"%.7f\" hae=\"%.1f\" ce=\"%.1f\" le=\"%.1f\"/>"
        "<detail>"
        "<contact callsign=\"%s\"/>"
        "<__group name=\"%s\" role=\"%s\"/>"
        "<tritium_edge device_id=\"%s\" role=\"sensor\"/>"
        "</detail>"
        "</event>",
        _device_id, _cot_type, _how,
        time_str, start_str, stale_str,
        _lat, _lon, _hae, _ce, _le,
        safe_callsign,
        _team, _role,
        _device_id);

    if (n < 0 || n >= (int)buf_size) {
        DBG_ERROR(TAG, "Position event buffer overflow (%d >= %u)", n, (unsigned)buf_size);
        return 0;
    }
    return n;
}

int build_sensor_event(char* buf, size_t buf_size,
                       const char* sensor_type, float value,
                       const char* unit) {
    if (!_initialized) return 0;

    uint32_t epoch = get_epoch();
    if (epoch == 0) return 0;

    char time_str[32], start_str[32], stale_str[32];
    format_iso8601(time_str, sizeof(time_str), epoch);
    format_iso8601(start_str, sizeof(start_str), epoch);
    format_iso8601(stale_str, sizeof(stale_str), epoch + 120);  // 2 min stale for sensors

    // Escape sensor type and callsign
    char safe_type[32];
    xml_escape(safe_type, sizeof(safe_type), sensor_type);

    char safe_callsign[64];
    snprintf(safe_callsign, sizeof(safe_callsign), "%s/%s", _device_id, safe_type);

    // Remark text: "temperature: 23.5 C"
    char remark[64];
    if (unit && unit[0] != '\0') {
        snprintf(remark, sizeof(remark), "%s: %.1f %s", sensor_type, value, unit);
    } else {
        snprintf(remark, sizeof(remark), "%s: %.1f", sensor_type, value);
    }

    // Matches tritium-lib sensor_to_cot() format:
    //   uid="tritium-sensor-{device_id}-{sensor_type}"
    //   type="a-f-G-E-S"
    //   how="m-r" (machine-reported)
    int n = snprintf(buf, buf_size,
        "<?xml version='1.0' encoding='UTF-8'?>"
        "<event version=\"2.0\""
        " uid=\"tritium-sensor-%s-%s\""
        " type=\"a-f-G-E-S\""
        " how=\"m-r\""
        " time=\"%s\""
        " start=\"%s\""
        " stale=\"%s\">"
        "<point lat=\"%.7f\" lon=\"%.7f\" hae=\"%.1f\" ce=\"%.1f\" le=\"%.1f\"/>"
        "<detail>"
        "<contact callsign=\"%s\"/>"
        "<remarks>%s</remarks>"
        "</detail>"
        "</event>",
        _device_id, safe_type,
        time_str, start_str, stale_str,
        _lat, _lon, _hae, _ce, _le,
        safe_callsign,
        remark);

    if (n < 0 || n >= (int)buf_size) {
        DBG_ERROR(TAG, "Sensor event buffer overflow (%d >= %u)", n, (unsigned)buf_size);
        return 0;
    }
    return n;
}

int build_status_event(char* buf, size_t buf_size) {
    if (!_initialized) return 0;

    uint32_t epoch = get_epoch();
    if (epoch == 0) return 0;

    char time_str[32], start_str[32], stale_str[32];
    format_iso8601(time_str, sizeof(time_str), epoch);
    format_iso8601(start_str, sizeof(start_str), epoch);
    format_iso8601(stale_str, sizeof(stale_str), epoch + _stale_seconds);

    char safe_callsign[64];
    xml_escape(safe_callsign, sizeof(safe_callsign), _callsign);

    // Status details: battery percentage placeholder, uptime, free heap, RSSI
    unsigned long uptime_s = millis() / 1000;
    uint32_t free_heap = ESP.getFreeHeap();
    int rssi = WiFi.RSSI();

    // Status remark
    char remark[128];
    snprintf(remark, sizeof(remark),
             "uptime=%lus heap=%u rssi=%d",
             uptime_s, (unsigned)free_heap, rssi);

    int n = snprintf(buf, buf_size,
        "<?xml version='1.0' encoding='UTF-8'?>"
        "<event version=\"2.0\""
        " uid=\"tritium-edge-%s\""
        " type=\"%s\""
        " how=\"%s\""
        " time=\"%s\""
        " start=\"%s\""
        " stale=\"%s\">"
        "<point lat=\"%.7f\" lon=\"%.7f\" hae=\"%.1f\" ce=\"%.1f\" le=\"%.1f\"/>"
        "<detail>"
        "<contact callsign=\"%s\"/>"
        "<__group name=\"%s\" role=\"%s\"/>"
        "<status readiness=\"true\"/>"
        "<remarks>%s</remarks>"
        "<tritium_edge device_id=\"%s\" role=\"sensor\""
        " uptime_s=\"%lu\" free_heap=\"%u\" rssi=\"%d\"/>"
        "</detail>"
        "</event>",
        _device_id, _cot_type, _how,
        time_str, start_str, stale_str,
        _lat, _lon, _hae, _ce, _le,
        safe_callsign,
        _team, _role,
        remark,
        _device_id, uptime_s, (unsigned)free_heap, rssi);

    if (n < 0 || n >= (int)buf_size) {
        DBG_ERROR(TAG, "Status event buffer overflow (%d >= %u)", n, (unsigned)buf_size);
        return 0;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

bool send_multicast(const char* xml, size_t len) {
    if (!WiFi.isConnected()) return false;
    if (!xml || xml[0] == '\0') return false;
    if (len == 0) len = strlen(xml);

    if (!_udp.beginMulticastPacket()) {
        // Fallback: manual beginPacket to multicast address
        if (!_udp.beginPacket(TAK_MULTICAST_ADDR, TAK_MULTICAST_PORT)) {
            DBG_ERROR(TAG, "Failed to begin multicast packet");
            return false;
        }
    }

    size_t written = _udp.write((const uint8_t*)xml, len);
    if (!_udp.endPacket()) {
        DBG_ERROR(TAG, "Failed to send multicast packet");
        return false;
    }

    DBG_DEBUG(TAG, "Multicast sent %u bytes to %s:%u",
              (unsigned)written, TAK_MULTICAST_ADDR, TAK_MULTICAST_PORT);
    return (written == len);
}

bool send_tcp(const char* host, uint16_t port, const char* xml, size_t len) {
    if (!WiFi.isConnected()) return false;
    if (!xml || xml[0] == '\0') return false;
    if (len == 0) len = strlen(xml);

    WiFiClient client;
    if (!client.connect(host, port, 5000)) {
        DBG_ERROR(TAG, "TCP connect failed: %s:%u", host, port);
        return false;
    }

    // TAK server protocol: 4-byte big-endian length prefix, then XML payload
    uint8_t len_prefix[4];
    len_prefix[0] = (uint8_t)((len >> 24) & 0xFF);
    len_prefix[1] = (uint8_t)((len >> 16) & 0xFF);
    len_prefix[2] = (uint8_t)((len >> 8) & 0xFF);
    len_prefix[3] = (uint8_t)(len & 0xFF);

    client.write(len_prefix, 4);
    size_t written = client.write((const uint8_t*)xml, len);
    client.flush();
    client.stop();

    DBG_DEBUG(TAG, "TCP sent %u bytes to %s:%u", (unsigned)written, host, port);
    return (written == len);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool init(const CotConfig& config) {
    if (_initialized) return _active;
    _initialized = true;

    _interval_ms = config.interval_ms;
    _stale_seconds = config.stale_seconds;
    _ce = config.ce;
    _le = config.le;

    // Copy string configs
    if (config.device_id && config.device_id[0] != '\0') {
        strncpy(_device_id, config.device_id, sizeof(_device_id) - 1);
    }
    if (config.callsign && config.callsign[0] != '\0') {
        strncpy(_callsign, config.callsign, sizeof(_callsign) - 1);
    }
    if (config.cot_type) {
        strncpy(_cot_type, config.cot_type, sizeof(_cot_type) - 1);
    }
    if (config.team) {
        strncpy(_team, config.team, sizeof(_team) - 1);
    }
    if (config.role) {
        strncpy(_role, config.role, sizeof(_role) - 1);
    }
    if (config.how) {
        strncpy(_how, config.how, sizeof(_how) - 1);
    }

    // Derive identity from MAC if not explicitly provided
    derive_identity_from_mac();

    // Default type if empty
    if (_cot_type[0] == '\0') {
        strncpy(_cot_type, COT_TYPE_SENSOR, sizeof(_cot_type) - 1);
    }
    if (_team[0] == '\0') {
        strncpy(_team, "Cyan", sizeof(_team) - 1);
    }
    if (_role[0] == '\0') {
        strncpy(_role, "Sensor", sizeof(_role) - 1);
    }
    if (_how[0] == '\0') {
        strncpy(_how, "m-g", sizeof(_how) - 1);
    }

    _active = true;
    _last_send_ms = 0;  // Send on first tick

    DBG_INFO(TAG, "Initialized: uid=tritium-edge-%s callsign=%s type=%s interval=%lums",
             _device_id, _callsign, _cot_type, (unsigned long)_interval_ms);
    return true;
}

void set_position(double lat, double lon, float hae) {
    _lat = lat;
    _lon = lon;
    _hae = hae;
    _has_position = true;
}

bool tick() {
    if (!_active) return false;
    if (!WiFi.isConnected()) return false;
    if (!_has_position) return false;

    uint32_t now = millis();
    if (_last_send_ms != 0 && (now - _last_send_ms) < _interval_ms) {
        return false;
    }
    _last_send_ms = now;

    // Build and send position CoT via multicast
    static char xml[1024];
    int len = build_position_event(xml, sizeof(xml));
    if (len <= 0) return false;

    return send_multicast(xml, (size_t)len);
}

bool is_active() {
    return _active;
}

}  // namespace hal_cot

#endif  // SIMULATOR
