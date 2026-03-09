// Tritium-OS WebSocket Bridge — real-time event streaming, serial terminal,
// system status, and bidirectional command execution over WebSocket.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ws_bridge.h"

#if HAS_ASYNC_WEBSERVER

#include "serial_capture.h"
#include "debug_log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_timer.h>
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
    uint32_t id;
    uint8_t  channels;       // subscribed channel bitmask
    uint32_t msg_count;      // messages this second
    uint32_t last_rate_reset; // millis of last rate reset
    bool     active;
};

static ClientState _clients[MAX_CLIENTS];
static SemaphoreHandle_t _client_mutex = nullptr;

// ── WebSocket instance ───────────────────────────────────────────────────────

static AsyncWebSocket* _ws = nullptr;
static AsyncWebServer* _server_ref = nullptr;
static uint32_t _last_status_ms  = 0;
static uint32_t _last_cleanup_ms = 0;

// Screenshot app reference (set externally)
static App* _screenshot_app = nullptr;

// ── Forward declarations ─────────────────────────────────────────────────────

extern const char* _serial_capture_drain_inject();
extern void _serial_capture_write_line(const char* line);

static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len);
static ClientState* findClient(uint32_t id);
static ClientState* addClient(uint32_t id);
static void removeClient(uint32_t id);
static bool checkRate(ClientState* cs);
static void parseSubscribe(ClientState* cs, const char* json);
static void handleCommand(AsyncWebSocketClient* client, const char* cmd);
static void handleSerialInput(AsyncWebSocketClient* client, const char* data);
static void handleSettingChange(AsyncWebSocketClient* client, const char* json);
static void handleSettingsGet(AsyncWebSocketClient* client, const char* json);
static void handleSettingsSet(AsyncWebSocketClient* client, const char* json);
static void handleScreenshot(AsyncWebSocketClient* client);
static void sendPong(AsyncWebSocketClient* client);
static void buildStatusJson(char* buf, size_t size);
static void broadcastToChannel(uint8_t channel, const char* json);

// Helpers for minimal JSON parsing (no ArduinoJson)
static bool jsonStrVal(const char* json, const char* key, char* out, size_t out_size);
static bool jsonHasKey(const char* json, const char* key);

// ── Terminal page (PROGMEM) ──────────────────────────────────────────────────

#include "web/terminal_html.h"

// ── Public API ───────────────────────────────────────────────────────────────

void ws_bridge::init(AsyncWebServer* server) {
    if (_ws) return;  // already initialized

    _client_mutex = xSemaphoreCreateMutex();
    if (!_client_mutex) {
        DEBUG_LOG(TAG, "Failed to create client mutex");
        return;
    }

    memset(_clients, 0, sizeof(_clients));

    _ws = new AsyncWebSocket("/ws");
    _ws->onEvent(onWsEvent);
    server->addHandler(_ws);
    _server_ref = server;

    // Register /terminal page
    server->on("/terminal", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send_P(200, "text/html", TERMINAL_HTML, TERMINAL_HTML_LEN);
    });

    // Hook serial capture to forward lines to WS clients
    serial_capture::init();
    serial_capture::onLine([](const char* line, void*) {
        ws_bridge::sendSerialLine(line);
    }, nullptr);

    // Subscribe to TritiumEventBus and auto-forward all events to WS clients
#if WS_HAS_EVENTS
    TritiumEventBus::instance().subscribeAll([](const TritiumEvent& event, void*) {
        if (!_ws || _ws->count() == 0) return;

        const char* name = eventIdToName(event.id);
        char data_buf[128];

        // Format event data based on content
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
            // Try as string if data looks like text
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

    DEBUG_LOG(TAG, "WebSocket bridge initialized at /ws (max %d clients)", MAX_CLIENTS);
}

void ws_bridge::broadcast(const char* json) {
    if (!_ws || !json) return;
    _ws->textAll(json);
}

void ws_bridge::sendStatus() {
    if (!_ws) return;

    char buf[512];
    buildStatusJson(buf, sizeof(buf));
    broadcastToChannel(CH_STATUS, buf);
}

void ws_bridge::sendEvent(const char* event_name, const char* data_json) {
    if (!_ws || !event_name) return;

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
    if (!_ws || !message) return;

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"toast\",\"level\":\"%s\",\"message\":\"%s\"}",
             level ? level : "info", message);
    _ws->textAll(buf);
}

void ws_bridge::sendSerialLine(const char* line) {
    if (!_ws || !line) return;

    // Escape special JSON chars in the line
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
            continue;  // skip CR
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

void ws_bridge::handleMessage(AsyncWebSocketClient* client, const char* data, size_t len) {
    if (!client || !data || len == 0 || len > MAX_MSG_SIZE) return;

    // Null-terminate (data may not be terminated)
    char msg[MAX_MSG_SIZE + 1];
    size_t copy_len = (len < MAX_MSG_SIZE) ? len : MAX_MSG_SIZE;
    memcpy(msg, data, copy_len);
    msg[copy_len] = '\0';

    // Rate limiting
    ClientState* cs = findClient(client->id());
    if (cs && !checkRate(cs)) {
        client->text("{\"type\":\"error\",\"message\":\"Rate limit exceeded\"}");
        return;
    }

    // Parse message type
    char type[32] = {0};
    if (!jsonStrVal(msg, "type", type, sizeof(type))) return;

    if (strcmp(type, "subscribe") == 0) {
        if (cs) parseSubscribe(cs, msg);
    } else if (strcmp(type, "command") == 0 || strcmp(type, "cmd") == 0) {
        char cmd[256] = {0};
        if (jsonStrVal(msg, "cmd", cmd, sizeof(cmd))) {
            handleCommand(client, cmd);
        }
    } else if (strcmp(type, "serial") == 0) {
        char serial_data[512] = {0};
        if (jsonStrVal(msg, "data", serial_data, sizeof(serial_data))) {
            handleSerialInput(client, serial_data);
        }
    } else if (strcmp(type, "setting") == 0) {
        handleSettingChange(client, msg);
    } else if (strcmp(type, "settings_get") == 0) {
        handleSettingsGet(client, msg);
    } else if (strcmp(type, "settings_set") == 0) {
        handleSettingsSet(client, msg);
    } else if (strcmp(type, "screenshot") == 0) {
        handleScreenshot(client);
    } else if (strcmp(type, "ping") == 0) {
        sendPong(client);
    }
}

int ws_bridge::clientCount() {
    if (!_ws) return 0;
    return _ws->count();
}

void ws_bridge::cleanup() {
    if (!_ws) return;

    uint32_t now = millis();

    // Periodic client cleanup
    if (now - _last_cleanup_ms >= CLEANUP_INTERVAL) {
        _ws->cleanupClients(MAX_CLIENTS);
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
        // Split command and args at first space (same as main.cpp)
        char cmd_copy[256];
        strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
        cmd_copy[sizeof(cmd_copy) - 1] = '\0';

        // Strip trailing newline/CR
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

        // Echo the command to serial subscribers
        char echo_buf[300];
        snprintf(echo_buf, sizeof(echo_buf), "> %s%s%s",
                 verb, args ? " " : "", args ? args : "");
        sendSerialLine(echo_buf);

        // Built-in commands
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

// ── WebSocket event handler ──────────────────────────────────────────────────

static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT: {
            DEBUG_LOG(TAG, "WS client %u connected from %s",
                      client->id(), client->remoteIP().toString().c_str());
            ClientState* cs = addClient(client->id());
            if (!cs) {
                // Too many clients
                client->text("{\"type\":\"error\",\"message\":\"Server full\"}");
                client->close();
                return;
            }
            // Send welcome message
            char welcome[128];
            snprintf(welcome, sizeof(welcome),
                     "{\"type\":\"toast\",\"level\":\"info\","
                     "\"message\":\"Connected to Tritium-OS\"}");
            client->text(welcome);
            break;
        }

        case WS_EVT_DISCONNECT:
            DEBUG_LOG(TAG, "WS client %u disconnected", client->id());
            removeClient(client->id());
            break;

        case WS_EVT_DATA: {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            if (info->final && info->index == 0 && info->len == len) {
                // Complete single-frame message
                if (info->opcode == WS_TEXT) {
                    ws_bridge::handleMessage(client, (const char*)data, len);
                }
            }
            break;
        }

        case WS_EVT_PONG:
            break;

        case WS_EVT_ERROR:
            DEBUG_LOG(TAG, "WS client %u error", client->id());
            break;
    }
}

// ── Client state management ──────────────────────────────────────────────────

static ClientState* findClient(uint32_t id) {
    if (!_client_mutex) return nullptr;
    ClientState* result = nullptr;
    if (xSemaphoreTake(_client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clients[i].active && _clients[i].id == id) {
                result = &_clients[i];
                break;
            }
        }
        xSemaphoreGive(_client_mutex);
    }
    return result;
}

static ClientState* addClient(uint32_t id) {
    if (!_client_mutex) return nullptr;
    ClientState* result = nullptr;
    if (xSemaphoreTake(_client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!_clients[i].active) {
                _clients[i].id = id;
                _clients[i].channels = CH_ALL;  // subscribe to all by default
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

static void removeClient(uint32_t id) {
    if (!_client_mutex) return;
    if (xSemaphoreTake(_client_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (_clients[i].active && _clients[i].id == id) {
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
    // Look for channel names in the JSON array
    if (strstr(json, "\"status\""))  channels |= CH_STATUS;
    if (strstr(json, "\"events\""))  channels |= CH_EVENTS;
    if (strstr(json, "\"serial\""))  channels |= CH_SERIAL;
    if (strstr(json, "\"all\""))     channels  = CH_ALL;

    cs->channels = channels ? channels : CH_ALL;
}

// ── Command dispatch ─────────────────────────────────────────────────────────

static void handleCommand(AsyncWebSocketClient* client, const char* cmd) {
    if (!cmd) return;

    // Inject command into serial capture for processing by main loop
    serial_capture::injectCommand(cmd);

    // Immediate response for certain commands
    if (strcmp(cmd, "WIFI_SCAN") == 0) {
        client->text("{\"type\":\"toast\",\"level\":\"info\","
                     "\"message\":\"WiFi scan started\"}");
    }
}

// ── Serial input from terminal ───────────────────────────────────────────────

static void handleSerialInput(AsyncWebSocketClient* client, const char* data) {
    if (!data) return;

    // Strip trailing newline if present, then inject
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

static void handleSettingChange(AsyncWebSocketClient* client, const char* json) {
    // Settings framework not yet implemented — acknowledge and log
    char domain[32] = {0}, key[64] = {0}, value[128] = {0};
    jsonStrVal(json, "domain", domain, sizeof(domain));
    jsonStrVal(json, "key", key, sizeof(key));
    jsonStrVal(json, "value", value, sizeof(value));

    DEBUG_LOG(TAG, "Setting change: %s.%s = %s", domain, key, value);

    // Handle display brightness immediately (common use case)
#if WS_HAS_DISPLAY
    if (strcmp(domain, "display") == 0 && strcmp(key, "brightness") == 0) {
        int brightness = atoi(value);
        if (brightness >= 0 && brightness <= 255) {
            display_set_brightness((uint8_t)brightness);
            char resp[128];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"toast\",\"level\":\"info\","
                     "\"message\":\"Brightness set to %d\"}", brightness);
            client->text(resp);
            return;
        }
    }
#endif

    client->text("{\"type\":\"toast\",\"level\":\"info\","
                 "\"message\":\"Setting acknowledged (framework pending)\"}");
}

// ── Settings get/set via TritiumSettings ──────────────────────────────────

static void handleSettingsGet(AsyncWebSocketClient* client, const char* json) {
    if (!client || !json) return;

#if WS_HAS_SETTINGS
    char domain[32] = {0}, key[64] = {0};
    jsonStrVal(json, "domain", domain, sizeof(domain));
    jsonStrVal(json, "key", key, sizeof(key));

    auto& settings = TritiumSettings::instance();

    if (domain[0] && !key[0]) {
        // Export entire domain as JSON
        char buf[1024];
        int written = settings.toJson(buf, sizeof(buf), domain);
        if (written > 0) {
            char resp[1200];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"settings\",\"domain\":\"%s\",\"data\":%s}",
                     domain, buf);
            client->text(resp);
        } else {
            client->text("{\"type\":\"error\",\"message\":\"Domain not found or empty\"}");
        }
    } else if (domain[0] && key[0]) {
        // Return single key value as string
        const char* val = settings.getString(domain, key, nullptr);
        if (val) {
            char resp[512];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"settings\",\"domain\":\"%s\",\"key\":\"%s\","
                     "\"value\":\"%s\"}",
                     domain, key, val);
            client->text(resp);
        } else {
            // Try as int (getString returns nullptr only if not a string type)
            int32_t ival = settings.getInt(domain, key, INT32_MIN);
            if (ival != INT32_MIN) {
                char resp[256];
                snprintf(resp, sizeof(resp),
                         "{\"type\":\"settings\",\"domain\":\"%s\",\"key\":\"%s\","
                         "\"value\":%ld}",
                         domain, key, (long)ival);
                client->text(resp);
            } else {
                client->text("{\"type\":\"error\",\"message\":\"Key not found\"}");
            }
        }
    } else {
        // Export all settings
        char buf[2048];
        int written = settings.toJson(buf, sizeof(buf));
        if (written > 0) {
            char resp[2200];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"settings\",\"data\":%s}", buf);
            client->text(resp);
        } else {
            client->text("{\"type\":\"error\",\"message\":\"No settings available\"}");
        }
    }
#else
    client->text("{\"type\":\"error\",\"message\":\"Settings framework not available\"}");
#endif
}

static void handleSettingsSet(AsyncWebSocketClient* client, const char* json) {
    if (!client || !json) return;

#if WS_HAS_SETTINGS
    char domain[32] = {0}, key[64] = {0}, value[128] = {0};
    jsonStrVal(json, "domain", domain, sizeof(domain));
    jsonStrVal(json, "key", key, sizeof(key));
    jsonStrVal(json, "value", value, sizeof(value));

    if (!domain[0] || !key[0]) {
        client->text("{\"type\":\"error\",\"message\":\"Missing domain or key\"}");
        return;
    }

    DEBUG_LOG(TAG, "Settings set: %s.%s = %s", domain, key, value);

    auto& settings = TritiumSettings::instance();
    bool ok = settings.setString(domain, key, value);

    if (ok) {
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"toast\",\"level\":\"info\","
                 "\"message\":\"Setting %s.%s updated\"}", domain, key);
        client->text(resp);

        // Also broadcast the change as an event so other clients see it
        char evt_data[256];
        snprintf(evt_data, sizeof(evt_data),
                 "{\"domain\":\"%s\",\"key\":\"%s\",\"value\":\"%s\"}",
                 domain, key, value);
        ws_bridge::sendEvent("settings_changed", evt_data);
    } else {
        client->text("{\"type\":\"error\",\"message\":\"Failed to save setting\"}");
    }
#else
    client->text("{\"type\":\"error\",\"message\":\"Settings framework not available\"}");
#endif
}

// ── Screenshot capture ───────────────────────────────────────────────────────

static void handleScreenshot(AsyncWebSocketClient* client) {
#if WS_HAS_APP
    if (!_screenshot_app) {
        client->text("{\"type\":\"error\",\"message\":\"No screenshot provider\"}");
        return;
    }

    int w = 0, h = 0;
    uint16_t* fb = _screenshot_app->getFramebuffer(w, h);
    if (!fb || w <= 0 || h <= 0) {
        client->text("{\"type\":\"error\",\"message\":\"Framebuffer not available\"}");
        return;
    }

    // Send metadata first
    char meta[128];
    snprintf(meta, sizeof(meta),
             "{\"type\":\"screenshot\",\"width\":%d,\"height\":%d,\"format\":\"rgb565\"}",
             w, h);
    client->text(meta);

    // Send raw RGB565 framebuffer as binary message
    size_t fb_size = (size_t)w * h * 2;
    client->binary((const uint8_t*)fb, fb_size);
#else
    client->text("{\"type\":\"error\",\"message\":\"Display not available\"}");
#endif
}

// ── Ping/pong ────────────────────────────────────────────────────────────────

static void sendPong(AsyncWebSocketClient* client) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"pong\",\"time\":%lu}",
             (unsigned long)(esp_timer_get_time() / 1000000ULL));
    client->text(buf);
}

// ── Status JSON builder ──────────────────────────────────────────────────────

static void buildStatusJson(char* buf, size_t size) {
    uint32_t uptime = millis() / 1000;
    uint32_t heap = esp_get_free_heap_size();
    uint32_t psram = 0;
    if (psramFound()) {
        psram = ESP.getFreePsram();
    }

    int8_t rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
    const char* ip = WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "0.0.0.0";

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
             ip,
             services,
             ws_bridge::clientCount(),
             disp_w, disp_h);
}

// ── Channel-filtered broadcast ───────────────────────────────────────────────

static void broadcastToChannel(uint8_t channel, const char* json) {
    if (!_ws || !json) return;

    // Fast path: if only one channel matters, iterate clients
    for (auto& client : _ws->getClients()) {
        if (client.status() != WS_CONNECTED) continue;

        ClientState* cs = findClient(client.id());
        if (cs && (cs->channels & channel)) {
            client.text(json);
        }
    }
}

// ── Minimal JSON string value parser ─────────────────────────────────────────
// Extracts a string value for a given key from flat JSON.
// Only handles simple cases — no nested objects, no escaped quotes in values.

static bool jsonStrVal(const char* json, const char* key, char* out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return false;

    // Build search pattern: "key":"
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char* start = strstr(json, pattern);
    if (!start) {
        // Try without quotes on value (for numbers): "key":
        snprintf(pattern, sizeof(pattern), "\"%s\":", key);
        start = strstr(json, pattern);
        if (!start) return false;
        start += strlen(pattern);

        // Skip whitespace
        while (*start == ' ') start++;

        // Read until comma, brace, bracket, or end
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

    // Copy until closing quote
    size_t i = 0;
    while (start[i] && start[i] != '"' && i < out_size - 1) {
        if (start[i] == '\\' && start[i + 1]) {
            // Handle escaped chars
            i++;
            if (start[i] == 'n') out[i - 1] = '\n';
            else out[i - 1] = start[i];
            // Adjust — we consumed an extra char
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

#endif  // HAS_ASYNC_WEBSERVER
