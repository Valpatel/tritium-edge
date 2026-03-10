// Tritium-OS WebSocket Bridge — real-time event streaming, serial terminal,
// system status, and bidirectional command execution over WebSocket.
// Uses ESP-IDF native esp_http_server WebSocket support.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ws_bridge.h"

#include "serial_capture.h"
#include "debug_log.h"

#include "tritium_compat.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>

// Service registry for command dispatch
#if __has_include("service_registry.h")
#include "service_registry.h"
#define WS_HAS_REGISTRY 1
#else
#define WS_HAS_REGISTRY 0
#endif

// Display info for status messages
#if __has_include("display.h")
#include "display.h"
#define WS_HAS_DISPLAY 1
#else
#define WS_HAS_DISPLAY 0
#endif

// App interface for screenshot capture
#if __has_include("app.h")
#include "app.h"
#define WS_HAS_APP 1
#else
#define WS_HAS_APP 0
#endif

// Event bus for auto-forwarding events to WS clients
#if __has_include("os_events.h")
#include "os_events.h"
#define WS_HAS_EVENTS 1
#else
#define WS_HAS_EVENTS 0
#endif

// Settings framework for get/set over WS
#if __has_include("os_settings.h")
#include "os_settings.h"
#define WS_HAS_SETTINGS 1
#else
#define WS_HAS_SETTINGS 0
#endif

static const char* TAG = "ws_bridge";

// ── Configuration ────────────────────────────────────────────────────────────

static constexpr int    MAX_CLIENTS       = 4;
static constexpr int    MAX_MSG_RATE      = 10;    // messages/second per client
static constexpr size_t MAX_MSG_SIZE      = 2048;  // max incoming message bytes
static constexpr int    STATUS_INTERVAL   = 2000;  // ms between status broadcasts
static constexpr int    CLEANUP_INTERVAL  = 5000;  // ms between cleanup passes

// ── Channel subscription tracking ────────────────────────────────────────────

enum Channel : uint8_t {
    CH_STATUS = 0x01,
    CH_EVENTS = 0x02,
    CH_SERIAL = 0x04,
    CH_ALL    = 0x07,
};

struct ClientState {
    int      fd;
    uint8_t  channels;       // subscribed channel bitmask
    uint32_t msg_count;      // messages this second
    uint32_t last_rate_reset; // millis of last rate reset
    bool     active;
};

static ClientState _clients[MAX_CLIENTS];
static SemaphoreHandle_t _client_mutex = nullptr;

// ── Server reference ─────────────────────────────────────────────────────────

static httpd_handle_t _server_ref = nullptr;
static uint32_t _last_status_ms  = 0;
static uint32_t _last_cleanup_ms = 0;

// Screenshot app reference (set externally)
static App* _screenshot_app = nullptr;

// ── Forward declarations ─────────────────────────────────────────────────────

extern const char* _serial_capture_drain_inject();
extern void _serial_capture_write_line(const char* line);

static ClientState* findClient(int fd);
static ClientState* addClient(int fd);
static void removeClient(int fd);
static bool checkRate(ClientState* cs);
static void parseSubscribe(ClientState* cs, const char* json);
static void handleCommand(int fd, const char* cmd);
static void handleSerialInput(int fd, const char* data);
static void handleSettingChange(int fd, const char* json);
static void handleSettingsGet(int fd, const char* json);
static void handleSettingsSet(int fd, const char* json);
static void handleScreenshot(int fd);
static void sendPong(int fd);
static void buildStatusJson(char* buf, size_t size);
static void broadcastToChannel(uint8_t channel, const char* json);
static void sendTextToFd(int fd, const char* text);

// Helpers for minimal JSON parsing (no ArduinoJson)
static bool jsonStrVal(const char* json, const char* key, char* out, size_t out_size);
static bool jsonHasKey(const char* json, const char* key);

// ── Helper: send text frame to a single client fd ────────────────────────────

static void sendTextToFd(int fd, const char* text) {
    if (!_server_ref || !text || fd < 0) return;
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t*)text;
    frame.len = strlen(text);
    httpd_ws_send_frame_async(_server_ref, fd, &frame);
}

// ── Helper: get active WebSocket client count ────────────────────────────────

static int getActiveClientCount() {
    int count = 0;
    if (_client_mutex && xSemaphoreTake(_client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clients[i].active) count++;
        }
        xSemaphoreGive(_client_mutex);
    }
    return count;
}

// ── WebSocket handler (called by esp_http_server) ────────────────────────────

static esp_err_t ws_handler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        // New WebSocket connection opened
        int fd = httpd_req_to_sockfd(req);
        DBG_INFO(TAG, "WS client fd=%d connected", fd);
        ClientState* cs = addClient(fd);
        if (!cs) {
            const char* err = "{\"type\":\"error\",\"message\":\"Server full\"}";
            httpd_ws_frame_t frame = {};
            frame.type = HTTPD_WS_TYPE_TEXT;
            frame.payload = (uint8_t*)err;
            frame.len = strlen(err);
            httpd_ws_send_frame(req, &frame);
            return ESP_FAIL;
        }
        // Send welcome
        const char* welcome = "{\"type\":\"toast\",\"level\":\"info\","
                              "\"message\":\"Connected to Tritium-OS\"}";
        httpd_ws_frame_t frame = {};
        frame.type = HTTPD_WS_TYPE_TEXT;
        frame.payload = (uint8_t*)welcome;
        frame.len = strlen(welcome);
        httpd_ws_send_frame(req, &frame);
        return ESP_OK;
    }

    // Receive WebSocket frame
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;

    // First call with len=0 to get the frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;
    if (frame.len == 0) return ESP_OK;
    if (frame.len > MAX_MSG_SIZE) return ESP_FAIL;

    uint8_t* buf = (uint8_t*)malloc(frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        DBG_INFO(TAG, "WS client fd=%d disconnected", fd);
        removeClient(fd);
        free(buf);
        return ESP_OK;
    }

    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        free(buf);
        return ESP_OK;
    }

    buf[frame.len] = '\0';
    int fd = httpd_req_to_sockfd(req);

    // Rate limiting
    ClientState* cs = findClient(fd);
    if (cs && !checkRate(cs)) {
        sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"Rate limit exceeded\"}");
        free(buf);
        return ESP_OK;
    }

    // Parse message type
    char type[32] = {0};
    const char* msg = (const char*)buf;
    if (!jsonStrVal(msg, "type", type, sizeof(type))) {
        free(buf);
        return ESP_OK;
    }

    if (strcmp(type, "subscribe") == 0) {
        if (cs) parseSubscribe(cs, msg);
    } else if (strcmp(type, "command") == 0 || strcmp(type, "cmd") == 0) {
        char cmd[256] = {0};
        if (jsonStrVal(msg, "cmd", cmd, sizeof(cmd))) {
            handleCommand(fd, cmd);
        }
    } else if (strcmp(type, "serial") == 0) {
        char serial_data[512] = {0};
        if (jsonStrVal(msg, "data", serial_data, sizeof(serial_data))) {
            handleSerialInput(fd, serial_data);
        }
    } else if (strcmp(type, "setting") == 0) {
        handleSettingChange(fd, msg);
    } else if (strcmp(type, "settings_get") == 0) {
        handleSettingsGet(fd, msg);
    } else if (strcmp(type, "settings_set") == 0) {
        handleSettingsSet(fd, msg);
    } else if (strcmp(type, "screenshot") == 0) {
        handleScreenshot(fd);
    } else if (strcmp(type, "ping") == 0) {
        sendPong(fd);
    }

    free(buf);
    return ESP_OK;
}

// ── Public API ───────────────────────────────────────────────────────────────

void ws_bridge::init(httpd_handle_t server) {
    if (_server_ref) return;  // already initialized

    _client_mutex = xSemaphoreCreateMutex();
    if (!_client_mutex) {
        DBG_INFO(TAG, "Failed to create client mutex");
        return;
    }

    memset(_clients, 0, sizeof(_clients));
    _server_ref = server;

    // Register WebSocket URI handler
    httpd_uri_t ws_uri = {};
    ws_uri.uri = "/ws";
    ws_uri.method = HTTP_GET;
    ws_uri.handler = ws_handler;
    ws_uri.user_ctx = nullptr;
    ws_uri.is_websocket = true;
    httpd_register_uri_handler(server, &ws_uri);

    // Hook serial capture to forward lines to WS clients
    serial_capture::init();
    serial_capture::onLine([](const char* line, void*) {
        ws_bridge::sendSerialLine(line);
    }, nullptr);

    // Subscribe to TritiumEventBus and auto-forward all events to WS clients
#if WS_HAS_EVENTS
    TritiumEventBus::instance().subscribeAll([](const TritiumEvent& event, void*) {
        if (!_server_ref || getActiveClientCount() == 0) return;

        const char* name = eventIdToName(event.id);
        char data_buf[128];

        if (event.data_len == 0) {
            ws_bridge::sendEvent(name);
        } else if (event.data_len == sizeof(int32_t)) {
            snprintf(data_buf, sizeof(data_buf), "{\"value\":%ld}",
                     (long)event.asInt());
            ws_bridge::sendEvent(name, data_buf);
        } else if (event.data_len == sizeof(float)) {
            snprintf(data_buf, sizeof(data_buf), "{\"value\":%.2f}",
                     (double)event.asFloat());
            ws_bridge::sendEvent(name, data_buf);
        } else if (event.data_len > 0) {
            const char* str = event.asString();
            if (str && str[0] >= 0x20) {
                snprintf(data_buf, sizeof(data_buf), "{\"value\":\"%s\"}", str);
                ws_bridge::sendEvent(name, data_buf);
            } else {
                ws_bridge::sendEvent(name);
            }
        }
    }, nullptr);
#endif

    DBG_INFO(TAG, "WebSocket bridge initialized at /ws (max %d clients)", MAX_CLIENTS);
}

void ws_bridge::broadcast(const char* json) {
    if (!_server_ref || !json) return;
    if (_client_mutex && xSemaphoreTake(_client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clients[i].active) {
                sendTextToFd(_clients[i].fd, json);
            }
        }
        xSemaphoreGive(_client_mutex);
    }
}

void ws_bridge::sendStatus() {
    if (!_server_ref) return;

    char buf[512];
    buildStatusJson(buf, sizeof(buf));
    broadcastToChannel(CH_STATUS, buf);
}

void ws_bridge::sendEvent(const char* event_name, const char* data_json) {
    if (!_server_ref || !event_name) return;

    char buf[512];
    if (data_json) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"event\",\"name\":\"%s\",\"data\":%s}",
                 event_name, data_json);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"event\",\"name\":\"%s\"}",
                 event_name);
    }
    broadcastToChannel(CH_EVENTS, buf);
}

void ws_bridge::sendToast(const char* message, const char* level) {
    if (!_server_ref || !message) return;

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"toast\",\"level\":\"%s\",\"message\":\"%s\"}",
             level ? level : "info", message);
    broadcast(buf);
}

void ws_bridge::sendSerialLine(const char* line) {
    if (!_server_ref || !line) return;

    char escaped[512];
    size_t j = 0;
    for (size_t i = 0; line[i] && j < sizeof(escaped) - 2; i++) {
        char c = line[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= sizeof(escaped) - 1) break;
            escaped[j++] = '\\';
            escaped[j++] = c;
        } else if (c == '\n') {
            if (j + 2 >= sizeof(escaped) - 1) break;
            escaped[j++] = '\\';
            escaped[j++] = 'n';
        } else if (c == '\r') {
            continue;
        } else if (c == '\t') {
            if (j + 2 >= sizeof(escaped) - 1) break;
            escaped[j++] = '\\';
            escaped[j++] = 't';
        } else if ((uint8_t)c >= 0x20) {
            escaped[j++] = c;
        }
    }
    escaped[j] = '\0';

    char buf[600];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"serial\",\"data\":\"%s\\n\"}", escaped);
    broadcastToChannel(CH_SERIAL, buf);
}

int ws_bridge::clientCount() {
    return getActiveClientCount();
}

void ws_bridge::cleanup() {
    ws_bridge::tick();
}

void ws_bridge::tick() {
    if (!_server_ref) return;

    uint32_t now = millis();

    // Periodic cleanup — check if clients are still connected
    if (now - _last_cleanup_ms >= CLEANUP_INTERVAL) {
        if (_client_mutex && xSemaphoreTake(_client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (_clients[i].active) {
                    // Check if socket is still valid
                    if (httpd_ws_get_fd_info(_server_ref, _clients[i].fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
                        DBG_INFO(TAG, "WS client fd=%d gone, removing", _clients[i].fd);
                        _clients[i].active = false;
                    }
                }
            }
            xSemaphoreGive(_client_mutex);
        }
        _last_cleanup_ms = now;
    }

    // Periodic status broadcast
    if (now - _last_status_ms >= STATUS_INTERVAL) {
        sendStatus();
        _last_status_ms = now;
    }

    // Drain injected serial commands and dispatch them
    const char* cmd;
    while ((cmd = _serial_capture_drain_inject()) != nullptr) {
        char cmd_copy[256];
        strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
        cmd_copy[sizeof(cmd_copy) - 1] = '\0';

        size_t clen = strlen(cmd_copy);
        while (clen > 0 && (cmd_copy[clen - 1] == '\n' || cmd_copy[clen - 1] == '\r')) {
            cmd_copy[--clen] = '\0';
        }
        if (clen == 0) continue;

        char* space = strchr(cmd_copy, ' ');
        const char* verb = cmd_copy;
        const char* args = nullptr;
        if (space) {
            *space = '\0';
            args = space + 1;
        }

        char echo_buf[300];
        snprintf(echo_buf, sizeof(echo_buf), "> %s%s%s",
                 verb, args ? " " : "", args ? args : "");
        sendSerialLine(echo_buf);

        if (strcmp(verb, "IDENTIFY") == 0) {
#if WS_HAS_DISPLAY && WS_HAS_REGISTRY
            const display_health_t* dh = display_get_health();
            char resp[256];
            snprintf(resp, sizeof(resp),
                     "{\"board\":\"%s\",\"display\":\"%dx%d\",\"services\":%d}",
                     dh ? dh->driver : "unknown",
                     display_get_width(), display_get_height(),
                     ServiceRegistry::count());
            sendSerialLine(resp);
#endif
        } else if (strcmp(verb, "SERVICES") == 0) {
#if WS_HAS_REGISTRY
            char line[128];
            snprintf(line, sizeof(line), "[svc] %d services:", ServiceRegistry::count());
            sendSerialLine(line);
            for (int i = 0; i < ServiceRegistry::count(); i++) {
                auto* s = ServiceRegistry::at(i);
                snprintf(line, sizeof(line), "  %-20s pri=%3d cap=%02X",
                         s->name(), s->initPriority(), s->capabilities());
                sendSerialLine(line);
            }
#endif
        }
#if WS_HAS_REGISTRY
        else if (!ServiceRegistry::dispatchCommand(verb, args)) {
            char err[128];
            snprintf(err, sizeof(err), "[cmd] Unknown: %s", verb);
            sendSerialLine(err);
        }
#endif
    }
}

// ── Set screenshot app reference ─────────────────────────────────────────────

void ws_bridge_set_screenshot_app(App* app) {
    _screenshot_app = app;
}

// ── Client state management ──────────────────────────────────────────────────

static ClientState* findClient(int fd) {
    if (!_client_mutex) return nullptr;
    ClientState* result = nullptr;
    if (xSemaphoreTake(_client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clients[i].active && _clients[i].fd == fd) {
                result = &_clients[i];
                break;
            }
        }
        xSemaphoreGive(_client_mutex);
    }
    return result;
}

static ClientState* addClient(int fd) {
    if (!_client_mutex) return nullptr;
    ClientState* result = nullptr;
    if (xSemaphoreTake(_client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!_clients[i].active) {
                _clients[i].fd = fd;
                _clients[i].channels = CH_ALL;
                _clients[i].msg_count = 0;
                _clients[i].last_rate_reset = millis();
                _clients[i].active = true;
                result = &_clients[i];
                break;
            }
        }
        xSemaphoreGive(_client_mutex);
    }
    return result;
}

static void removeClient(int fd) {
    if (!_client_mutex) return;
    if (xSemaphoreTake(_client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clients[i].active && _clients[i].fd == fd) {
                _clients[i].active = false;
                break;
            }
        }
        xSemaphoreGive(_client_mutex);
    }
}

// ── Rate limiting ────────────────────────────────────────────────────────────

static bool checkRate(ClientState* cs) {
    if (!cs) return false;
    uint32_t now = millis();
    if (now - cs->last_rate_reset >= 1000) {
        cs->msg_count = 0;
        cs->last_rate_reset = now;
    }
    if (cs->msg_count >= (uint32_t)MAX_MSG_RATE) return false;
    cs->msg_count++;
    return true;
}

// ── Subscription parsing ─────────────────────────────────────────────────────

static void parseSubscribe(ClientState* cs, const char* json) {
    if (!cs || !json) return;

    uint8_t channels = 0;
    if (strstr(json, "\"status\""))  channels |= CH_STATUS;
    if (strstr(json, "\"events\""))  channels |= CH_EVENTS;
    if (strstr(json, "\"serial\""))  channels |= CH_SERIAL;
    if (strstr(json, "\"all\""))     channels  = CH_ALL;

    cs->channels = channels ? channels : CH_ALL;
}

// ── Command dispatch ─────────────────────────────────────────────────────────

static void handleCommand(int fd, const char* cmd) {
    if (!cmd) return;
    serial_capture::injectCommand(cmd);

    if (strcmp(cmd, "WIFI_SCAN") == 0) {
        sendTextToFd(fd, "{\"type\":\"toast\",\"level\":\"info\","
                         "\"message\":\"WiFi scan started\"}");
    }
}

// ── Serial input from terminal ───────────────────────────────────────────────

static void handleSerialInput(int fd, const char* data) {
    if (!data) return;
    char cleaned[512];
    strncpy(cleaned, data, sizeof(cleaned) - 1);
    cleaned[sizeof(cleaned) - 1] = '\0';

    size_t len = strlen(cleaned);
    while (len > 0 && (cleaned[len - 1] == '\n' || cleaned[len - 1] == '\r')) {
        cleaned[--len] = '\0';
    }
    if (len > 0) {
        serial_capture::injectCommand(cleaned);
    }
}

// ── Settings change ──────────────────────────────────────────────────────────

static void handleSettingChange(int fd, const char* json) {
    char domain[32] = {0}, key[64] = {0}, value[128] = {0};
    jsonStrVal(json, "domain", domain, sizeof(domain));
    jsonStrVal(json, "key", key, sizeof(key));
    jsonStrVal(json, "value", value, sizeof(value));

    DBG_INFO(TAG, "Setting change: %s.%s = %s", domain, key, value);

#if WS_HAS_DISPLAY
    if (strcmp(domain, "display") == 0 && strcmp(key, "brightness") == 0) {
        int brightness = atoi(value);
        if (brightness >= 0 && brightness <= 255) {
            display_set_brightness((uint8_t)brightness);
            char resp[128];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"toast\",\"level\":\"info\","
                     "\"message\":\"Brightness set to %d\"}", brightness);
            sendTextToFd(fd, resp);
            return;
        }
    }
#endif

    sendTextToFd(fd, "{\"type\":\"toast\",\"level\":\"info\","
                     "\"message\":\"Setting acknowledged (framework pending)\"}");
}

// ── Settings get/set via TritiumSettings ──────────────────────────────────

static void handleSettingsGet(int fd, const char* json) {
    if (fd < 0 || !json) return;

#if WS_HAS_SETTINGS
    char domain[32] = {0}, key[64] = {0};
    jsonStrVal(json, "domain", domain, sizeof(domain));
    jsonStrVal(json, "key", key, sizeof(key));

    auto& settings = TritiumSettings::instance();

    if (domain[0] && !key[0]) {
        char buf[1024];
        int written = settings.toJson(buf, sizeof(buf), domain);
        if (written > 0) {
            char resp[1200];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"settings\",\"domain\":\"%s\",\"data\":%s}",
                     domain, buf);
            sendTextToFd(fd, resp);
        } else {
            sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"Domain not found or empty\"}");
        }
    } else if (domain[0] && key[0]) {
        const char* val = settings.getString(domain, key, nullptr);
        if (val) {
            char resp[512];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"settings\",\"domain\":\"%s\",\"key\":\"%s\","
                     "\"value\":\"%s\"}",
                     domain, key, val);
            sendTextToFd(fd, resp);
        } else {
            int32_t ival = settings.getInt(domain, key, INT32_MIN);
            if (ival != INT32_MIN) {
                char resp[256];
                snprintf(resp, sizeof(resp),
                         "{\"type\":\"settings\",\"domain\":\"%s\",\"key\":\"%s\","
                         "\"value\":%ld}",
                         domain, key, (long)ival);
                sendTextToFd(fd, resp);
            } else {
                sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"Key not found\"}");
            }
        }
    } else {
        char buf[2048];
        int written = settings.toJson(buf, sizeof(buf));
        if (written > 0) {
            char resp[2200];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"settings\",\"data\":%s}", buf);
            sendTextToFd(fd, resp);
        } else {
            sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"No settings available\"}");
        }
    }
#else
    sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"Settings framework not available\"}");
#endif
}

static void handleSettingsSet(int fd, const char* json) {
    if (fd < 0 || !json) return;

#if WS_HAS_SETTINGS
    char domain[32] = {0}, key[64] = {0}, value[128] = {0};
    jsonStrVal(json, "domain", domain, sizeof(domain));
    jsonStrVal(json, "key", key, sizeof(key));
    jsonStrVal(json, "value", value, sizeof(value));

    if (!domain[0] || !key[0]) {
        sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"Missing domain or key\"}");
        return;
    }

    DBG_INFO(TAG, "Settings set: %s.%s = %s", domain, key, value);

    auto& settings = TritiumSettings::instance();
    bool ok = settings.setString(domain, key, value);

    if (ok) {
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"toast\",\"level\":\"info\","
                 "\"message\":\"Setting %s.%s updated\"}", domain, key);
        sendTextToFd(fd, resp);

        char evt_data[256];
        snprintf(evt_data, sizeof(evt_data),
                 "{\"domain\":\"%s\",\"key\":\"%s\",\"value\":\"%s\"}",
                 domain, key, value);
        ws_bridge::sendEvent("settings_changed", evt_data);
    } else {
        sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"Failed to save setting\"}");
    }
#else
    sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"Settings framework not available\"}");
#endif
}

// ── Screenshot capture ───────────────────────────────────────────────────────

static void handleScreenshot(int fd) {
#if WS_HAS_APP
    if (!_screenshot_app) {
        sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"No screenshot provider\"}");
        return;
    }

    int w = 0, h = 0;
    uint16_t* fb = _screenshot_app->getFramebuffer(w, h);
    if (!fb || w <= 0 || h <= 0) {
        sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"Framebuffer not available\"}");
        return;
    }

    // Send metadata first
    char meta[128];
    snprintf(meta, sizeof(meta),
             "{\"type\":\"screenshot\",\"width\":%d,\"height\":%d,\"format\":\"rgb565\"}",
             w, h);
    sendTextToFd(fd, meta);

    // Send raw RGB565 framebuffer as binary message
    size_t fb_size = (size_t)w * h * 2;
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = (uint8_t*)fb;
    frame.len = fb_size;
    httpd_ws_send_frame_async(_server_ref, fd, &frame);
#else
    sendTextToFd(fd, "{\"type\":\"error\",\"message\":\"Display not available\"}");
#endif
}

// ── Ping/pong ────────────────────────────────────────────────────────────────

static void sendPong(int fd) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"pong\",\"time\":%lu}",
             (unsigned long)(esp_timer_get_time() / 1000000ULL));
    sendTextToFd(fd, buf);
}

// ── Status JSON builder ──────────────────────────────────────────────────────

static void buildStatusJson(char* buf, size_t size) {
    uint32_t uptime = millis() / 1000;
    uint32_t heap = esp_get_free_heap_size();
    uint32_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // Get WiFi RSSI and IP
    int8_t rssi = 0;
    char ip_str[16] = "0.0.0.0";
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    int services = 0;
#if WS_HAS_REGISTRY
    services = ServiceRegistry::count();
#endif

    int disp_w = 0, disp_h = 0;
#if WS_HAS_DISPLAY
    disp_w = display_get_width();
    disp_h = display_get_height();
#endif

    snprintf(buf, size,
             "{\"type\":\"status\","
             "\"uptime\":%lu,"
             "\"heap\":%lu,"
             "\"psram\":%lu,"
             "\"rssi\":%d,"
             "\"ip\":\"%s\","
             "\"services\":%d,"
             "\"clients\":%d,"
             "\"display_w\":%d,"
             "\"display_h\":%d}",
             (unsigned long)uptime,
             (unsigned long)heap,
             (unsigned long)psram,
             (int)rssi,
             ip_str,
             services,
             ws_bridge::clientCount(),
             disp_w, disp_h);
}

// ── Channel-filtered broadcast ───────────────────────────────────────────────

static void broadcastToChannel(uint8_t channel, const char* json) {
    if (!_server_ref || !json) return;

    if (_client_mutex && xSemaphoreTake(_client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clients[i].active && (_clients[i].channels & channel)) {
                sendTextToFd(_clients[i].fd, json);
            }
        }
        xSemaphoreGive(_client_mutex);
    }
}

// ── Minimal JSON string value parser ─────────────────────────────────────────

static bool jsonStrVal(const char* json, const char* key, char* out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return false;

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char* start = strstr(json, pattern);
    if (!start) {
        snprintf(pattern, sizeof(pattern), "\"%s\":", key);
        start = strstr(json, pattern);
        if (!start) return false;
        start += strlen(pattern);
        while (*start == ' ') start++;

        size_t i = 0;
        while (start[i] && start[i] != ',' && start[i] != '}' &&
               start[i] != ']' && i < out_size - 1) {
            out[i] = start[i];
            i++;
        }
        out[i] = '\0';
        return i > 0;
    }

    start += strlen(pattern);

    size_t i = 0;
    while (start[i] && start[i] != '"' && i < out_size - 1) {
        if (start[i] == '\\' && start[i + 1]) {
            i++;
            if (start[i] == 'n') out[i - 1] = '\n';
            else out[i - 1] = start[i];
        }
        out[i] = start[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool jsonHasKey(const char* json, const char* key) {
    if (!json || !key) return false;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern) != nullptr;
}
