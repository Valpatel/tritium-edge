/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once
#include "service.h"

class BleSerialService : public ServiceInterface {
public:
    const char* name() const override { return "ble_serial"; }
    uint8_t capabilities() const override { return SVC_TICK | SVC_SERIAL_CMD; }
    int initPriority() const override { return 45; }  // After WiFi (10), before most services
    bool init() override;
    void tick() override;
    bool handleCommand(const char* cmd, const char* args) override;
    int toJson(char* buf, size_t size) override;
};
