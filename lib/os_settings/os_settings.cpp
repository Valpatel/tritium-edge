// Tritium-OS Settings Framework — NVS-backed implementation
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "os_settings.h"
#include <cstdio>
#include <cstring>

// ============================================================================
// Platform: ESP32
// ============================================================================
#ifndef SIMULATOR

#include "tritium_compat.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_mac.h>

// Helper: get last 4 hex chars of MAC address for device-unique names
static void getMacSuffix(char* out, size_t size) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, size, "%02X%02X", mac[4], mac[5]);
}

#else // SIMULATOR

#include <cstdlib>
#include <map>
#include <string>
#include <mutex>

// Simulator: in-memory key-value store instead of NVS
static std::map<std::string, std::string> sim_store;
static std::mutex sim_mutex;

static void getMacSuffix(char* out, size_t size) {
    snprintf(out, size, "ABCD");
}

#endif // SIMULATOR

// ============================================================================
// Factory defaults table
// ============================================================================

// Helpers for static initializer — avoids designated-initializer verbosity
static constexpr SettingsDefault defBool(const char* d, const char* k, bool v) {
    SettingsDefault e{};
    e.domain = d;
    e.key = k;
    e.type = SettingsType::BOOL;
    e.value.b = v;
    return e;
}
static constexpr SettingsDefault defInt(const char* d, const char* k, int32_t v) {
    SettingsDefault e{};
    e.domain = d;
    e.key = k;
    e.type = SettingsType::INT32;
    e.value.i = v;
    return e;
}
static constexpr SettingsDefault defFloat(const char* d, const char* k, float v) {
    SettingsDefault e{};
    e.domain = d;
    e.key = k;
    e.type = SettingsType::FLOAT;
    e.value.f = v;
    return e;
}
static constexpr SettingsDefault defStr(const char* d, const char* k, const char* v) {
    SettingsDefault e{};
    e.domain = d;
    e.key = k;
    e.type = SettingsType::STRING;
    e.value.s = v;
    return e;
}

// The "%XXXX" placeholder is replaced at runtime with the MAC suffix.
const SettingsDefault FACTORY_DEFAULTS[] = {
    // system
    defStr("system",    "name",          "tritium-%XXXX"),
    defStr("system",    "timezone",      "UTC"),
    defInt("system",    "auto_sleep_s",  300),

    // display
    defInt("display",   "brightness",    255),
    defInt("display",   "timeout_s",     60),

    // wifi
    defBool("wifi",     "auto_connect",  true),
    defBool("wifi",     "ap_enabled",    false),
    defStr("wifi",      "ap_ssid",       "Tritium-%XXXX"),
    defStr("wifi",      "ap_password",   "tritium"),

    // bluetooth
    defBool("bluetooth","enabled",       false),
    defBool("bluetooth","serial_enabled",false),
    defStr("bluetooth", "name",          "Tritium-%XXXX"),

    // mesh
    defBool("mesh",     "enabled",       true),
    defStr("mesh",      "role",          "relay"),

    // fleet
    defInt("fleet",     "heartbeat_s",   60),
    defStr("fleet",     "server_url",    ""),

    // developer
    defBool("developer","debug_overlay", false),
    defInt("developer", "log_level",     2),

    // screensaver
    defStr("screensaver",  "type",           "starfield"),
    defInt("screensaver",  "timeout_s",      10),
    defBool("screensaver", "sf_reverse",     false),
    defBool("screensaver", "sf_colors",      true),
    defInt("screensaver",  "sf_star_size",   2),
    defBool("screensaver", "sf_warp",        false),
    defInt("screensaver",  "sf_speed",       12),
    defInt("screensaver",  "sf_brightness",  80),
};
const int FACTORY_DEFAULTS_COUNT =
    sizeof(FACTORY_DEFAULTS) / sizeof(FACTORY_DEFAULTS[0]);

// All known domains for iteration
static const char* ALL_DOMAINS[] = {
    "system", "display", "wifi", "bluetooth", "mesh", "apps", "fleet", "developer", "screensaver"
};
static constexpr int ALL_DOMAINS_COUNT = sizeof(ALL_DOMAINS) / sizeof(ALL_DOMAINS[0]);

// ============================================================================
// Singleton
// ============================================================================

TritiumSettings& TritiumSettings::instance() {
    static TritiumSettings inst;
    return inst;
}

// ============================================================================
// Namespace helper
// ============================================================================

void TritiumSettings::makeNamespace(const char* domain, char* ns, size_t ns_size) {
    snprintf(ns, ns_size, "trit_%.10s", domain);
}

// ============================================================================
// Mutex helpers
// ============================================================================

#ifndef SIMULATOR

static inline SemaphoreHandle_t asMutex(void* m) {
    return static_cast<SemaphoreHandle_t>(m);
}

static inline void lockMutex(void* m) {
    if (m) xSemaphoreTake(asMutex(m), portMAX_DELAY);
}

static inline void unlockMutex(void* m) {
    if (m) xSemaphoreGive(asMutex(m));
}

#else // SIMULATOR

static inline void lockMutex(void*) { sim_mutex.lock(); }
static inline void unlockMutex(void*) { sim_mutex.unlock(); }

#endif

// ============================================================================
// NVS helpers (ESP32 only) — check if a key exists in an NVS namespace
// ============================================================================

#ifndef SIMULATOR

// Returns true if the key exists in the given NVS namespace
static bool nvs_key_exists(const char* ns, const char* key) {
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) {
        return false;  // namespace doesn't exist yet
    }
    // Try to get the size of any type — if the key exists, one will succeed
    size_t len = 0;
    esp_err_t err = nvs_get_str(handle, key, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        int32_t dummy_i;
        err = nvs_get_i32(handle, key, &dummy_i);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t dummy_u;
        err = nvs_get_u8(handle, key, &dummy_u);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        size_t blob_len = 0;
        err = nvs_get_blob(handle, key, nullptr, &blob_len);
    }
    nvs_close(handle);
    return (err != ESP_ERR_NVS_NOT_FOUND);
}

#endif // !SIMULATOR

// ============================================================================
// Init
// ============================================================================

bool TritiumSettings::init() {
    if (_initialized) return true;

#ifndef SIMULATOR
    // Initialize NVS flash — required before any nvs_open() calls
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        Serial.printf("[settings] ERROR: nvs_flash_init failed (0x%x)\n", ret);
        return false;
    }

    _mutex = xSemaphoreCreateMutex();
    if (!_mutex) {
        Serial.printf("[settings] ERROR: failed to create mutex\n");
        return false;
    }
#endif

    applyDefaults();
    _initialized = true;

#ifndef SIMULATOR
    Serial.printf("[settings] Initialized with NVS backing store\n");
#else
    printf("[settings] Initialized with in-memory backing store (simulator)\n");
#endif

    return true;
}

// ============================================================================
// Apply factory defaults (only writes keys that don't yet exist in NVS)
// ============================================================================

void TritiumSettings::applyDefaults(const char* domain) {
    char macSuffix[5];
    getMacSuffix(macSuffix, sizeof(macSuffix));

    for (int i = 0; i < FACTORY_DEFAULTS_COUNT; i++) {
        const SettingsDefault& d = FACTORY_DEFAULTS[i];

        // Filter by domain if specified
        if (domain && strcmp(domain, d.domain) != 0) continue;

        char ns[16];
        makeNamespace(d.domain, ns, sizeof(ns));

#ifndef SIMULATOR
        // Check if key already exists — if so, skip
        if (nvs_key_exists(ns, d.key)) continue;

        // Key doesn't exist — write the factory default
        nvs_handle_t handle;
        if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) continue;

        switch (d.type) {
            case SettingsType::BOOL:
                nvs_set_u8(handle, d.key, d.value.b ? 1 : 0);
                break;
            case SettingsType::INT32:
                nvs_set_i32(handle, d.key, d.value.i);
                break;
            case SettingsType::FLOAT:
                nvs_set_blob(handle, d.key, &d.value.f, sizeof(float));
                break;
            case SettingsType::STRING: {
                // Replace %XXXX placeholder with MAC suffix
                const char* val = d.value.s;
                char resolved[SETTINGS_MAX_STRING_LEN];
                const char* placeholder = strstr(val, "%XXXX");
                if (placeholder) {
                    int prefix_len = (int)(placeholder - val);
                    snprintf(resolved, sizeof(resolved), "%.*s%s%s",
                             prefix_len, val, macSuffix, placeholder + 5);
                    nvs_set_str(handle, d.key, resolved);
                } else {
                    nvs_set_str(handle, d.key, val);
                }
                break;
            }
            default:
                break;
        }
        nvs_commit(handle);
        nvs_close(handle);
#else
        // Simulator: use "ns:key" as map key
        std::string mapKey = std::string(ns) + ":" + d.key;
        if (sim_store.find(mapKey) != sim_store.end()) continue;

        switch (d.type) {
            case SettingsType::BOOL:
                sim_store[mapKey] = d.value.b ? "1" : "0";
                break;
            case SettingsType::INT32:
                sim_store[mapKey] = std::to_string(d.value.i);
                break;
            case SettingsType::FLOAT:
                sim_store[mapKey] = std::to_string(d.value.f);
                break;
            case SettingsType::STRING: {
                const char* val = d.value.s;
                char resolved[SETTINGS_MAX_STRING_LEN];
                const char* placeholder = strstr(val, "%XXXX");
                if (placeholder) {
                    int prefix_len = (int)(placeholder - val);
                    snprintf(resolved, sizeof(resolved), "%.*s%s%s",
                             prefix_len, val, macSuffix, placeholder + 5);
                    sim_store[mapKey] = resolved;
                } else {
                    sim_store[mapKey] = val;
                }
                break;
            }
            default:
                break;
        }
#endif
    }
}

// ============================================================================
// Getters
// ============================================================================

bool TritiumSettings::getBool(const char* domain, const char* key, bool default_val) {
    lockMutex(_mutex);
    bool result = default_val;
    char ns[16];
    makeNamespace(domain, ns, sizeof(ns));

#ifndef SIMULATOR
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t val;
        if (nvs_get_u8(handle, key, &val) == ESP_OK) {
            result = (val != 0);
        }
        nvs_close(handle);
    }
#else
    std::string mapKey = std::string(ns) + ":" + key;
    auto it = sim_store.find(mapKey);
    if (it != sim_store.end()) {
        result = (it->second == "1" || it->second == "true");
    }
#endif

    unlockMutex(_mutex);
    return result;
}

int32_t TritiumSettings::getInt(const char* domain, const char* key, int32_t default_val) {
    lockMutex(_mutex);
    int32_t result = default_val;
    char ns[16];
    makeNamespace(domain, ns, sizeof(ns));

#ifndef SIMULATOR
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) == ESP_OK) {
        int32_t val;
        if (nvs_get_i32(handle, key, &val) == ESP_OK) {
            result = val;
        }
        nvs_close(handle);
    }
#else
    std::string mapKey = std::string(ns) + ":" + key;
    auto it = sim_store.find(mapKey);
    if (it != sim_store.end()) {
        result = atoi(it->second.c_str());
    }
#endif

    unlockMutex(_mutex);
    return result;
}

float TritiumSettings::getFloat(const char* domain, const char* key, float default_val) {
    lockMutex(_mutex);
    float result = default_val;
    char ns[16];
    makeNamespace(domain, ns, sizeof(ns));

#ifndef SIMULATOR
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) == ESP_OK) {
        float val;
        size_t len = sizeof(float);
        if (nvs_get_blob(handle, key, &val, &len) == ESP_OK && len == sizeof(float)) {
            result = val;
        }
        nvs_close(handle);
    }
#else
    std::string mapKey = std::string(ns) + ":" + key;
    auto it = sim_store.find(mapKey);
    if (it != sim_store.end()) {
        result = (float)atof(it->second.c_str());
    }
#endif

    unlockMutex(_mutex);
    return result;
}

const char* TritiumSettings::getString(const char* domain, const char* key,
                                       const char* default_val) {
    lockMutex(_mutex);
    char ns[16];
    makeNamespace(domain, ns, sizeof(ns));

#ifndef SIMULATOR
    nvs_handle_t handle;
    bool got_value = false;
    if (nvs_open(ns, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(_strBuf);
        if (nvs_get_str(handle, key, _strBuf, &len) == ESP_OK) {
            got_value = true;
        }
        nvs_close(handle);
    }
    if (!got_value) {
        strncpy(_strBuf, default_val, sizeof(_strBuf) - 1);
        _strBuf[sizeof(_strBuf) - 1] = '\0';
    }
#else
    std::string mapKey = std::string(ns) + ":" + key;
    auto it = sim_store.find(mapKey);
    if (it != sim_store.end()) {
        strncpy(_strBuf, it->second.c_str(), sizeof(_strBuf) - 1);
    } else {
        strncpy(_strBuf, default_val, sizeof(_strBuf) - 1);
    }
    _strBuf[sizeof(_strBuf) - 1] = '\0';
#endif

    unlockMutex(_mutex);
    return _strBuf;
}

// ============================================================================
// Setters
// ============================================================================

bool TritiumSettings::setBool(const char* domain, const char* key, bool value) {
    lockMutex(_mutex);
    bool ok = false;
    char ns[16];
    makeNamespace(domain, ns, sizeof(ns));

#ifndef SIMULATOR
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_u8(handle, key, value ? 1 : 0) == ESP_OK) {
            ok = (nvs_commit(handle) == ESP_OK);
        }
        nvs_close(handle);
    }
#else
    std::string mapKey = std::string(ns) + ":" + key;
    sim_store[mapKey] = value ? "1" : "0";
    ok = true;
#endif

    unlockMutex(_mutex);
    if (ok) notifyObservers(domain, key);
    return ok;
}

bool TritiumSettings::setInt(const char* domain, const char* key, int32_t value) {
    lockMutex(_mutex);
    bool ok = false;
    char ns[16];
    makeNamespace(domain, ns, sizeof(ns));

#ifndef SIMULATOR
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_i32(handle, key, value) == ESP_OK) {
            ok = (nvs_commit(handle) == ESP_OK);
        }
        nvs_close(handle);
    }
#else
    std::string mapKey = std::string(ns) + ":" + key;
    sim_store[mapKey] = std::to_string(value);
    ok = true;
#endif

    unlockMutex(_mutex);
    if (ok) notifyObservers(domain, key);
    return ok;
}

bool TritiumSettings::setFloat(const char* domain, const char* key, float value) {
    lockMutex(_mutex);
    bool ok = false;
    char ns[16];
    makeNamespace(domain, ns, sizeof(ns));

#ifndef SIMULATOR
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_blob(handle, key, &value, sizeof(float)) == ESP_OK) {
            ok = (nvs_commit(handle) == ESP_OK);
        }
        nvs_close(handle);
    }
#else
    std::string mapKey = std::string(ns) + ":" + key;
    sim_store[mapKey] = std::to_string(value);
    ok = true;
#endif

    unlockMutex(_mutex);
    if (ok) notifyObservers(domain, key);
    return ok;
}

bool TritiumSettings::setString(const char* domain, const char* key, const char* value) {
    if (!value) return false;

    lockMutex(_mutex);
    bool ok = false;
    char ns[16];
    makeNamespace(domain, ns, sizeof(ns));

#ifndef SIMULATOR
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_str(handle, key, value) == ESP_OK) {
            ok = (nvs_commit(handle) == ESP_OK);
        }
        nvs_close(handle);
    }
#else
    std::string mapKey = std::string(ns) + ":" + key;
    sim_store[mapKey] = value;
    ok = true;
#endif

    unlockMutex(_mutex);
    if (ok) notifyObservers(domain, key);
    return ok;
}

// ============================================================================
// Observer pattern
// ============================================================================

int TritiumSettings::onChange(const char* domain, const char* key,
                              SettingsCallback cb, void* user_data) {
    if (!cb) return -1;

    lockMutex(_mutex);
    for (int i = 0; i < SETTINGS_MAX_OBSERVERS; i++) {
        if (!_observers[i].active) {
            _observers[i].active = true;
            _observers[i].domain[0] = '\0';
            _observers[i].key[0] = '\0';
            if (domain) {
                strncpy(_observers[i].domain, domain, sizeof(_observers[i].domain) - 1);
                _observers[i].domain[sizeof(_observers[i].domain) - 1] = '\0';
            }
            if (key) {
                strncpy(_observers[i].key, key, sizeof(_observers[i].key) - 1);
                _observers[i].key[sizeof(_observers[i].key) - 1] = '\0';
            }
            _observers[i].cb = cb;
            _observers[i].user_data = user_data;
            unlockMutex(_mutex);
            return i;
        }
    }
    unlockMutex(_mutex);
    return -1;  // No slots available
}

void TritiumSettings::removeObserver(int observer_id) {
    if (observer_id < 0 || observer_id >= SETTINGS_MAX_OBSERVERS) return;
    lockMutex(_mutex);
    _observers[observer_id].active = false;
    _observers[observer_id].cb = nullptr;
    unlockMutex(_mutex);
}

void TritiumSettings::notifyObservers(const char* domain, const char* key) {
    // Copy observer list under lock, then call callbacks outside lock
    // to avoid deadlocks if callbacks access settings.
    Observer snapshot[SETTINGS_MAX_OBSERVERS];
    lockMutex(_mutex);
    memcpy(snapshot, _observers, sizeof(snapshot));
    unlockMutex(_mutex);

    for (int i = 0; i < SETTINGS_MAX_OBSERVERS; i++) {
        if (!snapshot[i].active || !snapshot[i].cb) continue;

        // Domain filter: empty domain = wildcard
        if (snapshot[i].domain[0] != '\0' && strcmp(snapshot[i].domain, domain) != 0) {
            continue;
        }
        // Key filter: empty key = wildcard
        if (snapshot[i].key[0] != '\0' && strcmp(snapshot[i].key, key) != 0) {
            continue;
        }

        snapshot[i].cb(domain, key, snapshot[i].user_data);
    }
}

// ============================================================================
// JSON export
// ============================================================================

int TritiumSettings::toJson(char* buf, size_t size, const char* domain) {
    if (!buf || size == 0) return -1;

    lockMutex(_mutex);

    int pos = 0;

    // Helper macro: append formatted text to buf, tracking pos. Sets pos = -1 on overflow.
    #define JAPPEND(fmt, ...) do { \
        if (pos >= 0) { \
            int _n = snprintf(buf + pos, size - pos, fmt, ##__VA_ARGS__); \
            if (_n < 0 || (size_t)(pos + _n) >= size) pos = -1; \
            else pos += _n; \
        } \
    } while(0)

    JAPPEND("{");

    // Determine which domains to export
    const char* domains[ALL_DOMAINS_COUNT];
    int domainCount = 0;
    if (domain) {
        domains[0] = domain;
        domainCount = 1;
    } else {
        for (int i = 0; i < ALL_DOMAINS_COUNT; i++) {
            domains[i] = ALL_DOMAINS[i];
        }
        domainCount = ALL_DOMAINS_COUNT;
    }

    bool firstDomain = true;
    for (int d = 0; d < domainCount && pos >= 0; d++) {
        // Find factory defaults for this domain to know key types
        bool hasKeys = false;
        for (int i = 0; i < FACTORY_DEFAULTS_COUNT; i++) {
            if (strcmp(FACTORY_DEFAULTS[i].domain, domains[d]) != 0) continue;

            if (!hasKeys) {
                if (!firstDomain) JAPPEND(",");
                JAPPEND("\"%s\":{", domains[d]);
                firstDomain = false;
                hasKeys = true;
            } else {
                JAPPEND(",");
            }

            const SettingsDefault& def = FACTORY_DEFAULTS[i];
            char ns[16];
            makeNamespace(domains[d], ns, sizeof(ns));

#ifndef SIMULATOR
            nvs_handle_t handle;
            if (nvs_open(ns, NVS_READONLY, &handle) == ESP_OK) {
                switch (def.type) {
                    case SettingsType::BOOL: {
                        uint8_t val;
                        bool bval = def.value.b;
                        if (nvs_get_u8(handle, def.key, &val) == ESP_OK) {
                            bval = (val != 0);
                        }
                        JAPPEND("\"%s\":%s", def.key, bval ? "true" : "false");
                        break;
                    }
                    case SettingsType::INT32: {
                        int32_t val = def.value.i;
                        nvs_get_i32(handle, def.key, &val);
                        JAPPEND("\"%s\":%d", def.key, (int)val);
                        break;
                    }
                    case SettingsType::FLOAT: {
                        float val = def.value.f;
                        size_t len = sizeof(float);
                        nvs_get_blob(handle, def.key, &val, &len);
                        JAPPEND("\"%s\":%.2f", def.key, val);
                        break;
                    }
                    case SettingsType::STRING: {
                        char strval[SETTINGS_MAX_STRING_LEN];
                        size_t len = sizeof(strval);
                        const char* fallback = def.value.s ? def.value.s : "";
                        if (nvs_get_str(handle, def.key, strval, &len) == ESP_OK) {
                            JAPPEND("\"%s\":\"%s\"", def.key, strval);
                        } else {
                            JAPPEND("\"%s\":\"%s\"", def.key, fallback);
                        }
                        break;
                    }
                    default:
                        break;
                }
                nvs_close(handle);
            }
#else
            std::string mapKey = std::string(ns) + ":" + def.key;
            auto it = sim_store.find(mapKey);
            const char* val = (it != sim_store.end()) ? it->second.c_str() : "";

            switch (def.type) {
                case SettingsType::BOOL:
                    JAPPEND("\"%s\":%s", def.key,
                            (strcmp(val, "1") == 0 || strcmp(val, "true") == 0)
                                ? "true" : "false");
                    break;
                case SettingsType::INT32:
                    JAPPEND("\"%s\":%d", def.key, atoi(val));
                    break;
                case SettingsType::FLOAT:
                    JAPPEND("\"%s\":%.2f", def.key, atof(val));
                    break;
                case SettingsType::STRING:
                    JAPPEND("\"%s\":\"%s\"", def.key, val);
                    break;
                default:
                    break;
            }
#endif
        }
        if (hasKeys) JAPPEND("}");
    }

    JAPPEND("}");
    #undef JAPPEND
    unlockMutex(_mutex);
    return pos;
}

// ============================================================================
// JSON import (minimal parser for flat domain objects)
// ============================================================================

bool TritiumSettings::fromJson(const char* json) {
    if (!json) return false;

    // Simple state-machine parser for: {"domain":{"key":value,...},...}
    // Supports string, int, float, bool values.
    bool anySet = false;
    const char* p = json;

    // Skip to opening brace
    while (*p && *p != '{') p++;
    if (!*p) return false;
    p++;  // skip '{'

    char domain[16];
    char key[16];
    char value[SETTINGS_MAX_STRING_LEN];

    auto skipWhitespace = [&]() { while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; };
    auto readQuotedString = [&](char* out, size_t maxLen) -> bool {
        if (*p != '"') return false;
        p++;  // skip opening quote
        size_t i = 0;
        while (*p && *p != '"' && i < maxLen - 1) {
            if (*p == '\\' && *(p + 1)) { p++; }  // skip escape
            out[i++] = *p++;
        }
        out[i] = '\0';
        if (*p == '"') p++;  // skip closing quote
        return true;
    };

    while (*p) {
        skipWhitespace();
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }

        // Read domain name
        if (!readQuotedString(domain, sizeof(domain))) break;
        skipWhitespace();
        if (*p != ':') break;
        p++;
        skipWhitespace();
        if (*p != '{') break;
        p++;  // enter domain object

        while (*p) {
            skipWhitespace();
            if (*p == '}') { p++; break; }
            if (*p == ',') { p++; continue; }

            // Read key
            if (!readQuotedString(key, sizeof(key))) break;
            skipWhitespace();
            if (*p != ':') break;
            p++;
            skipWhitespace();

            // Determine value type from factory defaults
            SettingsType type = SettingsType::STRING;
            for (int i = 0; i < FACTORY_DEFAULTS_COUNT; i++) {
                if (strcmp(FACTORY_DEFAULTS[i].domain, domain) == 0 &&
                    strcmp(FACTORY_DEFAULTS[i].key, key) == 0) {
                    type = FACTORY_DEFAULTS[i].type;
                    break;
                }
            }

            // Read value based on type
            if (*p == '"') {
                // String value
                readQuotedString(value, sizeof(value));
                setString(domain, key, value);
                anySet = true;
            } else if (*p == 't' || *p == 'f') {
                // Boolean
                bool bval = (*p == 't');
                while (*p && *p != ',' && *p != '}') p++;
                setBool(domain, key, bval);
                anySet = true;
            } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
                // Number — read into buffer
                size_t vi = 0;
                bool isFloat = false;
                while (*p && *p != ',' && *p != '}' && *p != ' ' &&
                       vi < sizeof(value) - 1) {
                    if (*p == '.') isFloat = true;
                    value[vi++] = *p++;
                }
                value[vi] = '\0';

                if (isFloat || type == SettingsType::FLOAT) {
                    setFloat(domain, key, (float)atof(value));
                } else {
                    setInt(domain, key, atoi(value));
                }
                anySet = true;
            } else {
                // Unknown — skip to next comma or brace
                while (*p && *p != ',' && *p != '}') p++;
            }
        }
    }

    return anySet;
}

// ============================================================================
// Factory reset
// ============================================================================

bool TritiumSettings::factoryReset(const char* domain) {
    lockMutex(_mutex);

    const char* domains[ALL_DOMAINS_COUNT];
    int domainCount = 0;
    if (domain) {
        domains[0] = domain;
        domainCount = 1;
    } else {
        for (int i = 0; i < ALL_DOMAINS_COUNT; i++) {
            domains[i] = ALL_DOMAINS[i];
        }
        domainCount = ALL_DOMAINS_COUNT;
    }

    bool ok = true;
    for (int d = 0; d < domainCount; d++) {
        char ns[16];
        makeNamespace(domains[d], ns, sizeof(ns));
        if (!clearNamespace(ns)) ok = false;
    }

    unlockMutex(_mutex);

    // Re-apply factory defaults for cleared domains
    if (domain) {
        applyDefaults(domain);
    } else {
        applyDefaults();
    }

#ifndef SIMULATOR
    Serial.printf("[settings] Factory reset: %s\n", domain ? domain : "all");
#else
    printf("[settings] Factory reset: %s\n", domain ? domain : "all");
#endif

    return ok;
}

bool TritiumSettings::clearNamespace(const char* ns) {
#ifndef SIMULATOR
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return (err == ESP_OK);
#else
    std::string prefix = std::string(ns) + ":";
    auto it = sim_store.begin();
    while (it != sim_store.end()) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = sim_store.erase(it);
        } else {
            ++it;
        }
    }
    return true;
#endif
}

// ============================================================================
// Key iteration
// ============================================================================

int TritiumSettings::getKeys(const char* domain, const char** keys, int max_keys) {
    if (!domain || !keys || max_keys <= 0) return 0;

    lockMutex(_mutex);

    int count = 0;
    for (int i = 0; i < FACTORY_DEFAULTS_COUNT && count < max_keys &&
             count < MAX_ITER_KEYS; i++) {
        if (strcmp(FACTORY_DEFAULTS[i].domain, domain) == 0) {
            strncpy(_keyBuf[count], FACTORY_DEFAULTS[i].key, 15);
            _keyBuf[count][15] = '\0';
            keys[count] = _keyBuf[count];
            count++;
        }
    }

    unlockMutex(_mutex);
    return count;
}
