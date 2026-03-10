// Tritium-OS Settings Service
// ServiceInterface wrapper for TritiumSettings — provides serial commands
// and JSON API integration.
//
// Priority 5: initializes before WiFi so settings are available early.
//
// Serial commands:
//   SET <domain> <key> <value>   — set a setting
//   GET <domain> <key>           — read a setting
//   SETTINGS [domain]            — dump all settings (or one domain) as JSON
//   FACTORY_RESET [domain]       — reset to factory defaults
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "service.h"
#include "os_settings.h"

class SettingsService : public ServiceInterface {
public:
    const char* name() const override { return "settings"; }
    uint8_t capabilities() const override { return SVC_SERIAL_CMD | SVC_WEB_API; }
    int initPriority() const override { return 5; }

    bool init() override;
    bool handleCommand(const char* cmd, const char* args) override;
    int toJson(char* buf, size_t size) override;

private:
    // Command handlers
    bool handleSet(const char* args);
    bool handleGet(const char* args);
    bool handleSettings(const char* args);
    bool handleFactoryReset(const char* args);
};
