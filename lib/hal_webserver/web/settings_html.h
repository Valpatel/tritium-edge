// Tritium-OS Web Settings — PROGMEM-embedded HTML page for remote device
// configuration. Fetches settings via GET /api/settings, saves via PUT.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

static const char SETTINGS_HTML[] PROGMEM =
#include "settings.html.inc"
;
