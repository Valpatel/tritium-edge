// Tritium-OS Settings Service — implementation
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "settings_service.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef SIMULATOR
#include "tritium_compat.h"
#define LOG(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#endif

// ============================================================================
// Init
// ============================================================================

bool SettingsService::init() {
    bool ok = TritiumSettings::instance().init();
    if (ok) {
        LOG("[tritium] Settings: ready (%d factory defaults)\n",
            FACTORY_DEFAULTS_COUNT);
    } else {
        LOG("[tritium] Settings: init FAILED\n");
    }
    return ok;
}

// ============================================================================
// Serial command dispatch
// ============================================================================

bool SettingsService::handleCommand(const char* cmd, const char* args) {
    if (strcmp(cmd, "SET") == 0) return handleSet(args);
    if (strcmp(cmd, "GET") == 0) return handleGet(args);
    if (strcmp(cmd, "SETTINGS") == 0) return handleSettings(args);
    if (strcmp(cmd, "FACTORY_RESET") == 0) return handleFactoryReset(args);
    return false;
}

// ============================================================================
// SET <domain> <key> <value>
// ============================================================================

bool SettingsService::handleSet(const char* args) {
    if (!args || !*args) {
        LOG("[settings] Usage: SET <domain> <key> <value>\n");
        return true;
    }

    char domain[16], key[16], value[SETTINGS_MAX_STRING_LEN];
    domain[0] = key[0] = value[0] = '\0';

    // Parse: domain key value (value may contain spaces)
    const char* p = args;
    auto skipSpace = [&]() { while (*p == ' ') p++; };
    auto readToken = [&](char* out, size_t maxLen) {
        size_t i = 0;
        while (*p && *p != ' ' && i < maxLen - 1) out[i++] = *p++;
        out[i] = '\0';
    };

    skipSpace();
    readToken(domain, sizeof(domain));
    skipSpace();
    readToken(key, sizeof(key));
    skipSpace();

    // Rest of args is the value (may contain spaces)
    strncpy(value, p, sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';

    if (!domain[0] || !key[0]) {
        LOG("[settings] Usage: SET <domain> <key> <value>\n");
        return true;
    }

    // Determine type from factory defaults
    TritiumSettings& s = TritiumSettings::instance();
    bool found = false;

    // Try to find the key in factory defaults to determine type
    for (int i = 0; i < FACTORY_DEFAULTS_COUNT; i++) {
        if (strcmp(FACTORY_DEFAULTS[i].domain, domain) == 0 &&
            strcmp(FACTORY_DEFAULTS[i].key, key) == 0) {
            found = true;
            bool ok = false;
            switch (FACTORY_DEFAULTS[i].type) {
                case SettingsType::BOOL:
                    ok = s.setBool(domain, key,
                                   strcmp(value, "true") == 0 ||
                                   strcmp(value, "1") == 0);
                    break;
                case SettingsType::INT32:
                    ok = s.setInt(domain, key, atoi(value));
                    break;
                case SettingsType::FLOAT:
                    ok = s.setFloat(domain, key, (float)atof(value));
                    break;
                case SettingsType::STRING:
                    ok = s.setString(domain, key, value);
                    break;
                default:
                    break;
            }
            LOG("[settings] SET %s.%s = %s %s\n", domain, key, value,
                ok ? "OK" : "FAILED");
            break;
        }
    }

    if (!found) {
        // Unknown key — store as string
        bool ok = s.setString(domain, key, value);
        LOG("[settings] SET %s.%s = %s (as string) %s\n", domain, key, value,
            ok ? "OK" : "FAILED");
    }

    return true;
}

// ============================================================================
// GET <domain> <key>
// ============================================================================

bool SettingsService::handleGet(const char* args) {
    if (!args || !*args) {
        LOG("[settings] Usage: GET <domain> <key>\n");
        return true;
    }

    char domain[16], key[16];
    domain[0] = key[0] = '\0';

    const char* p = args;
    auto skipSpace = [&]() { while (*p == ' ') p++; };
    auto readToken = [&](char* out, size_t maxLen) {
        size_t i = 0;
        while (*p && *p != ' ' && i < maxLen - 1) out[i++] = *p++;
        out[i] = '\0';
    };

    skipSpace();
    readToken(domain, sizeof(domain));
    skipSpace();
    readToken(key, sizeof(key));

    if (!domain[0] || !key[0]) {
        LOG("[settings] Usage: GET <domain> <key>\n");
        return true;
    }

    TritiumSettings& s = TritiumSettings::instance();

    // Find type in factory defaults
    for (int i = 0; i < FACTORY_DEFAULTS_COUNT; i++) {
        if (strcmp(FACTORY_DEFAULTS[i].domain, domain) == 0 &&
            strcmp(FACTORY_DEFAULTS[i].key, key) == 0) {
            switch (FACTORY_DEFAULTS[i].type) {
                case SettingsType::BOOL:
                    LOG("[settings] %s.%s = %s\n", domain, key,
                        s.getBool(domain, key) ? "true" : "false");
                    return true;
                case SettingsType::INT32:
                    LOG("[settings] %s.%s = %d\n", domain, key,
                        s.getInt(domain, key));
                    return true;
                case SettingsType::FLOAT:
                    LOG("[settings] %s.%s = %.2f\n", domain, key,
                        s.getFloat(domain, key));
                    return true;
                case SettingsType::STRING:
                    LOG("[settings] %s.%s = %s\n", domain, key,
                        s.getString(domain, key));
                    return true;
                default:
                    break;
            }
        }
    }

    // Unknown key — try as string
    const char* val = s.getString(domain, key, "");
    LOG("[settings] %s.%s = %s\n", domain, key, val);
    return true;
}

// ============================================================================
// SETTINGS [domain]
// ============================================================================

bool SettingsService::handleSettings(const char* args) {
    char jsonBuf[512];  // stack — only used briefly for serial output
    const char* domain = (args && *args) ? args : nullptr;

    // Skip leading whitespace
    if (domain) {
        while (*domain == ' ') domain++;
        if (*domain == '\0') domain = nullptr;
    }

    int n = TritiumSettings::instance().toJson(jsonBuf, sizeof(jsonBuf), domain);
    if (n > 0) {
        LOG("%s\n", jsonBuf);
    } else {
        LOG("[settings] Failed to export settings\n");
    }
    return true;
}

// ============================================================================
// FACTORY_RESET [domain]
// ============================================================================

bool SettingsService::handleFactoryReset(const char* args) {
    const char* domain = (args && *args) ? args : nullptr;
    if (domain) {
        while (*domain == ' ') domain++;
        if (*domain == '\0') domain = nullptr;
    }

    bool ok = TritiumSettings::instance().factoryReset(domain);
    LOG("[settings] Factory reset %s: %s\n",
        domain ? domain : "ALL", ok ? "OK" : "FAILED");
    return true;
}

// ============================================================================
// JSON export for web API
// ============================================================================

int SettingsService::toJson(char* buf, size_t size) {
    return TritiumSettings::instance().toJson(buf, size);
}
