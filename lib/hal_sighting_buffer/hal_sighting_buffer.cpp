// Created by Matthew Valancy
// Copyright 2026 Valpatel Software LLC
// Licensed under AGPL-3.0 — see LICENSE for details.

#include "hal_sighting_buffer.h"

#ifdef SIMULATOR

namespace hal_sighting_buffer {
bool init(const BufferConfig&) { return false; }
void shutdown() {}
void add_ble_sighting(const char*, const char*, int8_t, bool, uint16_t) {}
void add_wifi_sighting(const char*, const uint8_t[6], int8_t, uint8_t, uint8_t) {}
int flush_to_sd() { return 0; }
int get_pending_count() { return 0; }
int sync_to_server(const char*, const char*) { return 0; }
void tick() {}
Stats get_stats() { return {}; }
}

#else

#include "tritium_compat.h"
// SD card via VFS — use POSIX file ops on /sdcard/
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <dirent.h>

#if __has_include("hal_diag.h")
#include "hal_diag.h"
#define SB_LOG(sev, fmt, ...) hal_diag::log(sev, "sighting", fmt, ##__VA_ARGS__)
#else
#define SB_LOG(sev, fmt, ...) ((void)0)
#endif

namespace hal_sighting_buffer {

// --- Internal state ---

static constexpr int MAX_ENTRY_LEN = 256;

struct MemEntry {
    char json[MAX_ENTRY_LEN];
};

static MemEntry* _ring = nullptr;
static uint16_t _ring_head = 0;   // next write position
static uint16_t _ring_count = 0;  // entries in ring
static SemaphoreHandle_t _mutex = nullptr;

static BufferConfig _config;
static bool _inited = false;
static bool _sd_available = false;
static uint32_t _last_flush_ms = 0;
static char _current_file[64];

// Stats
static uint32_t _ble_buffered = 0;
static uint32_t _wifi_buffered = 0;
static uint32_t _total_flushed = 0;
static uint32_t _total_synced = 0;

// --- Helpers ---

static const char* authStr(uint8_t auth_type) {
    switch (auth_type) {
        case 0: return "OPEN";
        case 1: return "WEP";
        case 2: return "WPA";
        case 3: return "WPA2";
        case 4: return "WPA/2";
        case 5: return "WPA3";
        default: return "?";
    }
}

static bool is_ntp_synced() {
    time_t now = time(nullptr);
    return now > 1700000000;
}

static uint32_t get_timestamp() {
    time_t now = time(nullptr);
    if (now > 1700000000) return (uint32_t)now;
    return millis() / 1000;
}

static void make_filename(char* out, size_t out_size) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm t;
        gmtime_r(&now, &t);
        snprintf(out, out_size, "%s/%04d%02d%02d_%02d%02d%02d.jsonl",
            _config.base_dir,
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
            t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        uint32_t sec = millis() / 1000;
        snprintf(out, out_size, "%s/boot_%010lu.jsonl",
            _config.base_dir, (unsigned long)sec);
    }
}

static bool ensure_dir() {
    if (!_sd_available) return false;
    char fulldir[128];
    snprintf(fulldir, sizeof(fulldir), "/sdcard%s", _config.base_dir);
    struct stat st;
    if (stat(fulldir, &st) != 0) {
        return (mkdir(fulldir, 0755) == 0);
    }
    return true;
}

static bool check_file_rotation() {
    if (_current_file[0] == '\0') {
        make_filename(_current_file, sizeof(_current_file));
        return true;
    }
    // Check if current file exceeds max size
    char fullpath[128];
    snprintf(fullpath, sizeof(fullpath), "/sdcard%s", _current_file);
    struct stat st;
    if (stat(fullpath, &st) == 0) {
        if ((size_t)st.st_size >= _config.max_file_size) {
            make_filename(_current_file, sizeof(_current_file));
            return true;
        }
    }
    return true;
}

static void add_to_ring(const char* json_line) {
    if (!_ring || !_mutex) return;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    uint16_t idx = (_ring_head) % _config.max_memory_entries;
    strncpy(_ring[idx].json, json_line, MAX_ENTRY_LEN - 1);
    _ring[idx].json[MAX_ENTRY_LEN - 1] = '\0';

    if (_ring_count < _config.max_memory_entries) {
        _ring_count++;
    }
    _ring_head = (idx + 1) % _config.max_memory_entries;

    xSemaphoreGive(_mutex);
}

// --- Public API ---

bool init(const BufferConfig& config) {
    if (_inited) return true;

    _config = config;
    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) return false;

    _ring = (MemEntry*)malloc(config.max_memory_entries * sizeof(MemEntry));
    if (!_ring) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
        return false;
    }
    memset(_ring, 0, config.max_memory_entries * sizeof(MemEntry));

    _ring_head = 0;
    _ring_count = 0;
    _ble_buffered = 0;
    _wifi_buffered = 0;
    _total_flushed = 0;
    _total_synced = 0;
    _current_file[0] = '\0';
    _last_flush_ms = millis();

    // Check if SD card is available (VFS mounted at /sdcard)
    { struct stat st; _sd_available = (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode)); }
    if (_sd_available) {
        ensure_dir();
        Serial.printf("[sighting] SD card available, dir: %s\n", _config.base_dir);
    } else {
        Serial.printf("[sighting] SD card not available — memory-only mode\n");
    }

    _inited = true;
    Serial.printf("[sighting] Initialized (flush=%lums, max_mem=%u, max_file=%uKB)\n",
        (unsigned long)config.flush_interval_ms,
        (unsigned)config.max_memory_entries,
        (unsigned)(config.max_file_size / 1024));
    return true;
}

void shutdown() {
    if (!_inited) return;

    // Flush remaining entries before shutdown
    if (_ring_count > 0 && _sd_available) {
        flush_to_sd();
    }

    _inited = false;
    if (_ring) {
        free(_ring);
        _ring = nullptr;
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

void add_ble_sighting(const char* mac, const char* name, int8_t rssi,
                       bool is_known, uint16_t seen_count) {
    if (!_inited) return;

    char line[MAX_ENTRY_LEN];
    bool ntp = is_ntp_synced();
    uint32_t ts = get_timestamp();

    snprintf(line, sizeof(line),
        "{\"t\":\"ble\",\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,"
        "\"known\":%d,\"seen\":%u,\"ts\":%lu,\"ntp\":%s}",
        mac ? mac : "",
        name ? name : "",
        rssi,
        is_known ? 1 : 0,
        (unsigned)seen_count,
        (unsigned long)ts,
        ntp ? "true" : "false");

    add_to_ring(line);
    _ble_buffered++;
}

void add_wifi_sighting(const char* ssid, const uint8_t bssid[6], int8_t rssi,
                        uint8_t channel, uint8_t auth_type) {
    if (!_inited) return;

    char line[MAX_ENTRY_LEN];
    bool ntp = is_ntp_synced();
    uint32_t ts = get_timestamp();

    snprintf(line, sizeof(line),
        "{\"t\":\"wifi\",\"ssid\":\"%s\",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"rssi\":%d,\"ch\":%u,\"auth\":\"%s\",\"ts\":%lu,\"ntp\":%s}",
        ssid ? ssid : "",
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
        rssi, (unsigned)channel, authStr(auth_type),
        (unsigned long)ts,
        ntp ? "true" : "false");

    add_to_ring(line);
    _wifi_buffered++;
}

int flush_to_sd() {
    if (!_inited || !_mutex) return 0;
    if (!_sd_available) return 0;

    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_ring_count == 0) {
        xSemaphoreGive(_mutex);
        return 0;
    }

    if (!ensure_dir() || !check_file_rotation()) {
        xSemaphoreGive(_mutex);
        return -1;
    }

    char fullpath[128];
    snprintf(fullpath, sizeof(fullpath), "/sdcard%s", _current_file);
    FILE* f = fopen(fullpath, "a");
    if (!f) {
        Serial.printf("[sighting] Failed to open %s for append\n", _current_file);
        xSemaphoreGive(_mutex);
        return -1;
    }

    // Write all buffered entries
    // Read from oldest to newest
    int flushed = 0;
    uint16_t start;
    if (_ring_count < _config.max_memory_entries) {
        start = 0;
    } else {
        start = _ring_head;  // oldest entry (ring has wrapped)
    }

    for (uint16_t i = 0; i < _ring_count; i++) {
        uint16_t idx = (start + i) % _config.max_memory_entries;
        if (_ring[idx].json[0] != '\0') {
            fputs(_ring[idx].json, f);
            fputc('\n', f);
            flushed++;
        }
    }

    fclose(f);

    _ring_count = 0;
    _ring_head = 0;
    _total_flushed += flushed;

    xSemaphoreGive(_mutex);

    Serial.printf("[sighting] Flushed %d entries to %s\n", flushed, _current_file);
    return flushed;
}

int get_pending_count() {
    if (!_inited || !_sd_available) return 0;

    int count = 0;
    char fulldir[128];
    snprintf(fulldir, sizeof(fulldir), "/sdcard%s", _config.base_dir);
    DIR* dir = opendir(fulldir);
    if (!dir) return 0;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        size_t len = strlen(ent->d_name);
        if (len > 6 && strcmp(ent->d_name + len - 6, ".jsonl") == 0) {
            char filepath[192];
            snprintf(filepath, sizeof(filepath), "%s/%s", fulldir, ent->d_name);
            FILE* f = fopen(filepath, "r");
            if (f) {
                int c;
                while ((c = fgetc(f)) != EOF) {
                    if (c == '\n') count++;
                }
                fclose(f);
            }
        }
    }
    closedir(dir);
    return count;
}

int sync_to_server(const char* server_url, const char* device_id) {
    if (!_inited || !_sd_available) return -1;
    if (!server_url || !device_id) return -1;

    // Check WiFi connectivity
    { wifi_ap_record_t _ap; if (esp_wifi_sta_get_ap_info(&_ap) != ESP_OK) return -1; }

    // First flush any in-memory data
    flush_to_sd();

    char fulldir[128];
    snprintf(fulldir, sizeof(fulldir), "/sdcard%s", _config.base_dir);
    DIR* dir = opendir(fulldir);
    if (!dir) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/api/devices/%s/sightings", server_url, device_id);

    int total_uploaded = 0;

    // Collect file names first (can't iterate dir while modifying)
    char files[16][128];
    int file_count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr && file_count < 16) {
        size_t len = strlen(ent->d_name);
        if (len > 6 && strcmp(ent->d_name + len - 6, ".jsonl") == 0) {
            snprintf(files[file_count], sizeof(files[0]), "%s/%s",
                fulldir, ent->d_name);
            file_count++;
        }
    }
    closedir(dir);

    for (int fi = 0; fi < file_count; fi++) {
        // Read the file
        FILE* f = fopen(files[fi], "r");
        if (!f) continue;

        // Get file size
        fseek(f, 0, SEEK_END);
        size_t file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (file_size == 0) {
            fclose(f);
            remove(files[fi]);
            continue;
        }

        // Allocate buffer for the POST body
        // Overhead: ~60 bytes for wrapper + file content + commas
        size_t body_size = file_size + 256;
        char* body = (char*)malloc(body_size);
        if (!body) {
            fclose(f);
            continue;
        }

        int pos = snprintf(body, body_size,
            "{\"device_id\":\"%s\",\"sightings\":[", device_id);

        bool first = true;
        int line_count = 0;
        char line_buf[MAX_ENTRY_LEN];
        int li = 0;

        int ch;
        while ((ch = fgetc(f)) != EOF && pos < (int)body_size - MAX_ENTRY_LEN - 10) {
            if (ch == '\n') {
                line_buf[li] = '\0';
                if (li > 0) {
                    if (!first) body[pos++] = ',';
                    first = false;
                    int wrote = snprintf(body + pos, body_size - pos, "%s", line_buf);
                    pos += wrote;
                    line_count++;
                }
                li = 0;
            } else if (li < MAX_ENTRY_LEN - 1) {
                line_buf[li++] = (char)ch;
            }
        }
        // Handle last line without trailing newline
        if (li > 0) {
            line_buf[li] = '\0';
            if (!first) body[pos++] = ',';
            first = false;
            int wrote = snprintf(body + pos, body_size - pos, "%s", line_buf);
            pos += wrote;
            line_count++;
        }
        fclose(f);

        pos += snprintf(body + pos, body_size - pos, "]}");

        if (line_count == 0) {
            free(body);
            remove(files[fi]);
            continue;
        }

        // POST to server using ESP-IDF HTTP client
        esp_http_client_config_t http_config = {};
        http_config.url = url;
        http_config.method = HTTP_METHOD_POST;
        esp_http_client_handle_t client = esp_http_client_init(&http_config);
        if (!client) {
            free(body);
            Serial.printf("[sighting] Failed to create HTTP client\n");
            break;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, pos);
        esp_err_t err = esp_http_client_perform(client);
        int httpCode = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        free(body);

        if (err == ESP_OK && httpCode == 200) {
            remove(files[fi]);
            total_uploaded += line_count;
            Serial.printf("[sighting] Synced %d sightings from %s\n",
                line_count, files[fi]);
        } else {
            Serial.printf("[sighting] Sync failed for %s: HTTP %d (err=%s)\n",
                files[fi], httpCode, esp_err_to_name(err));
            break;  // Stop on first failure
        }
    }

    if (total_uploaded > 0) {
        _total_synced += total_uploaded;
        Serial.printf("[sighting] Total synced: %d sightings\n", total_uploaded);
    }

    return total_uploaded;
}

void tick() {
    if (!_inited) return;

    uint32_t now = millis();
    if ((now - _last_flush_ms) >= _config.flush_interval_ms) {
        _last_flush_ms = now;
        if (_ring_count > 0 && _sd_available) {
            flush_to_sd();
        }
    }
}

Stats get_stats() {
    Stats s = {};
    s.ble_sightings_buffered = _ble_buffered;
    s.wifi_sightings_buffered = _wifi_buffered;
    s.total_flushed_to_sd = _total_flushed;
    s.total_synced_to_server = _total_synced;
    s.pending_on_sd = (uint32_t)get_pending_count();
    s.sd_available = _sd_available;
    return s;
}

}  // namespace hal_sighting_buffer

#endif  // !SIMULATOR
