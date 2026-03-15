// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0

#include "hal_diag_dump.h"
#include "debug_log.h"

static constexpr const char* TAG = "diag_dump";

// ============================================================================
// SIMULATOR STUB
// ============================================================================
#ifdef SIMULATOR

namespace hal_diag_dump {

bool publish_diagnostic_dump(PublishFn, const char*) { return false; }
int collect_diagnostic_json(char* buf, size_t buf_size) {
    int n = snprintf(buf, buf_size, "{\"error\":\"simulator\"}");
    return n;
}

}  // namespace hal_diag_dump

// ============================================================================
// ESP32 — real diagnostic dump
// ============================================================================
#else

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <nvs.h>

// Optional HAL headers — conditional compilation
#if __has_include("hal_ble_scanner.h")
#include "hal_ble_scanner.h"
#define HAS_BLE_SCANNER 1
#else
#define HAS_BLE_SCANNER 0
#endif

#if __has_include("hal_wifi_scanner.h")
#include "hal_wifi_scanner.h"
#define HAS_WIFI_SCANNER 1
#else
#define HAS_WIFI_SCANNER 0
#endif

#if __has_include("hal_power.h")
#include "hal_power.h"
#define HAS_POWER 1
#else
#define HAS_POWER 0
#endif

#if __has_include("hal_imu.h")
#include "hal_imu.h"
#define HAS_IMU 1
#else
#define HAS_IMU 0
#endif

#if __has_include("hal_heartbeat.h")
#include "hal_heartbeat.h"
#define HAS_HEARTBEAT 1
#else
#define HAS_HEARTBEAT 0
#endif

#if __has_include("hal_rf_monitor.h")
#include "hal_rf_monitor.h"
#define HAS_RF_MONITOR 1
#else
#define HAS_RF_MONITOR 0
#endif

namespace hal_diag_dump {

// Large buffer — allocate in PSRAM for the dump
static constexpr size_t DUMP_BUF_SIZE = 4096;

static int append_heap_info(char* buf, size_t remaining) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);

    multi_heap_info_t psram_info;
    heap_caps_get_info(&psram_info, MALLOC_CAP_SPIRAM);

    return snprintf(buf, remaining,
        "\"heap\":{"
        "\"free\":%u,"
        "\"min_free\":%u,"
        "\"largest_block\":%u,"
        "\"total_allocated\":%u,"
        "\"total_free\":%u,"
        "\"alloc_count\":%u"
        "},"
        "\"psram\":{"
        "\"free\":%u,"
        "\"min_free\":%u,"
        "\"largest_block\":%u,"
        "\"total_allocated\":%u"
        "},",
        (unsigned)info.total_free_bytes,
        (unsigned)info.minimum_free_bytes,
        (unsigned)info.largest_free_block,
        (unsigned)info.total_allocated_bytes,
        (unsigned)info.total_free_bytes,
        (unsigned)info.allocated_blocks,
        (unsigned)psram_info.total_free_bytes,
        (unsigned)psram_info.minimum_free_bytes,
        (unsigned)psram_info.largest_free_block,
        (unsigned)psram_info.total_allocated_bytes
    );
}

static int append_task_info(char* buf, size_t remaining) {
    // Get task count and list
    UBaseType_t task_count = uxTaskGetNumberOfTasks();

    // Limit to reasonable count to avoid buffer overflow
    const int max_tasks = 20;
    TaskStatus_t tasks[max_tasks];
    uint32_t total_runtime;
    UBaseType_t actual = uxTaskGetSystemState(tasks, max_tasks, &total_runtime);

    int pos = snprintf(buf, remaining, "\"tasks\":{\"count\":%u,\"list\":[",
                       (unsigned)task_count);

    for (UBaseType_t i = 0; i < actual && i < (UBaseType_t)max_tasks; i++) {
        if (i > 0 && (size_t)pos < remaining) {
            buf[pos++] = ',';
        }
        pos += snprintf(buf + pos, remaining - pos,
            "{\"name\":\"%s\",\"state\":%d,\"priority\":%u,\"stack_hwm\":%u}",
            tasks[i].pcTaskName ? tasks[i].pcTaskName : "?",
            (int)tasks[i].eCurrentState,
            (unsigned)tasks[i].uxCurrentPriority,
            (unsigned)tasks[i].usStackHighWaterMark
        );
    }

    pos += snprintf(buf + pos, remaining - pos, "]},");
    return pos;
}

static int append_wifi_info(char* buf, size_t remaining) {
    bool connected = WiFi.isConnected();
    int pos = snprintf(buf, remaining,
        "\"wifi\":{"
        "\"connected\":%s,"
        "\"ssid\":\"%s\","
        "\"ip\":\"%s\","
        "\"mac\":\"%s\","
        "\"rssi\":%d,"
        "\"channel\":%d"
        "},",
        connected ? "true" : "false",
        connected ? WiFi.SSID().c_str() : "",
        connected ? WiFi.localIP().toString().c_str() : "0.0.0.0",
        WiFi.macAddress().c_str(),
        connected ? WiFi.RSSI() : 0,
        connected ? (int)WiFi.channel() : 0
    );
    return pos;
}

static int append_system_info(char* buf, size_t remaining) {
    const esp_app_desc_t* desc = esp_app_get_description();
    const char* version = desc ? desc->version : "unknown";
    const char* idf_ver = desc ? desc->idf_ver : "unknown";

    esp_reset_reason_t reason = esp_reset_reason();
    const char* reset_reasons[] = {
        "unknown", "poweron", "ext", "sw", "panic",
        "int_wdt", "task_wdt", "wdt", "deepsleep",
        "brownout", "sdio"
    };
    int reason_idx = (int)reason;
    const char* reason_str = (reason_idx >= 0 && reason_idx < 11)
        ? reset_reasons[reason_idx] : "unknown";

#ifdef DISPLAY_DRIVER
    const char* board = DISPLAY_DRIVER;
#else
    const char* board = "unknown";
#endif

    return snprintf(buf, remaining,
        "\"system\":{"
        "\"version\":\"%s\","
        "\"idf_version\":\"%s\","
        "\"board\":\"%s\","
        "\"uptime_s\":%lu,"
        "\"reset_reason\":\"%s\","
        "\"chip_model\":\"%s\","
        "\"chip_revision\":%d,"
        "\"cpu_freq_mhz\":%d"
        "},",
        version, idf_ver, board,
        (unsigned long)(millis() / 1000),
        reason_str,
        ESP.getChipModel(),
        (int)ESP.getChipRevision(),
        (int)ESP.getCpuFreqMHz()
    );
}

static int append_hal_status(char* buf, size_t remaining) {
    int pos = snprintf(buf, remaining, "\"hals\":{");

#if HAS_BLE_SCANNER
    pos += snprintf(buf + pos, remaining - pos,
        "\"ble_scanner\":{\"active\":%s},",
        hal_ble_scanner::is_active() ? "true" : "false");
#endif

#if HAS_WIFI_SCANNER
    pos += snprintf(buf + pos, remaining - pos,
        "\"wifi_scanner\":{\"active\":true},");
#endif

#if HAS_HEARTBEAT
    pos += snprintf(buf + pos, remaining - pos,
        "\"heartbeat\":{\"group\":\"%s\"},",
        hal_heartbeat::get_group());
#endif

    // Remove trailing comma if present
    if (pos > 0 && buf[pos - 1] == ',') {
        pos--;
    }

    pos += snprintf(buf + pos, remaining - pos, "},");
    return pos;
}

int collect_diagnostic_json(char* buf, size_t buf_size) {
    int pos = 0;

    pos += snprintf(buf + pos, buf_size - pos, "{");

    // System info
    pos += append_system_info(buf + pos, buf_size - pos);

    // Heap info
    pos += append_heap_info(buf + pos, buf_size - pos);

    // Task info
    pos += append_task_info(buf + pos, buf_size - pos);

    // WiFi info
    pos += append_wifi_info(buf + pos, buf_size - pos);

    // HAL statuses
    pos += append_hal_status(buf + pos, buf_size - pos);

    // Timestamp
    pos += snprintf(buf + pos, buf_size - pos,
        "\"dump_time_ms\":%lu", (unsigned long)millis());

    pos += snprintf(buf + pos, buf_size - pos, "}");

    return pos;
}

bool publish_diagnostic_dump(PublishFn publish, const char* device_id) {
    if (!publish || !device_id || device_id[0] == '\0') {
        DBG_ERROR(TAG, "Invalid publish function or device_id");
        return false;
    }

    // Allocate in PSRAM if available
    char* buf = (char*)heap_caps_malloc(DUMP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        buf = (char*)malloc(DUMP_BUF_SIZE);
    }
    if (!buf) {
        DBG_ERROR(TAG, "Failed to allocate dump buffer");
        return false;
    }

    int len = collect_diagnostic_json(buf, DUMP_BUF_SIZE);

    // Build topic
    char topic[128];
    snprintf(topic, sizeof(topic), "tritium/%s/diagnostics", device_id);

    DBG_INFO(TAG, "Publishing diagnostic dump (%d bytes) to %s", len, topic);
    bool ok = publish(topic, buf, false, 1);

    free(buf);

    if (ok) {
        DBG_INFO(TAG, "Diagnostic dump published successfully");
    } else {
        DBG_ERROR(TAG, "Failed to publish diagnostic dump");
    }

    return ok;
}

}  // namespace hal_diag_dump

#endif  // SIMULATOR
