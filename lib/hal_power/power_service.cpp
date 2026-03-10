// Tritium-OS Power Management Service — implementation
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "power_service.h"

#if defined(ENABLE_POWER_MGMT) && __has_include("hal_power.h")

#include "debug_log.h"
#include <cstdio>
#include <cstring>

#ifndef SIMULATOR
#include "tritium_compat.h"
#include "display.h"
#else
#include <cstdlib>
static uint32_t millis() { return 0; }
static void display_set_brightness(uint8_t) {}
#endif

static constexpr const char* TAG = "power";

// ============================================================================
// Profile defaults — brightness and timeout values per profile
// ============================================================================

struct ProfileDefaults {
    uint8_t  brightness;
    uint32_t dim_s;
    uint32_t off_s;
    uint32_t sleep_s;
};

static const ProfileDefaults PROFILE_DEFAULTS[] = {
    // PERFORMANCE: wall power, no timeouts
    { 255,   0,    0,    0 },
    // BALANCED: battery, moderate timeouts
    { 180,  60,  120,  300 },
    // POWER_SAVER: low battery, aggressive timeouts
    {  80,  30,   60,  120 },
};

// Emergency mode (battery < 5%): override everything
static const ProfileDefaults EMERGENCY_DEFAULTS = { 40, 15, 30, 30 };

// ============================================================================
// Singleton
// ============================================================================

PowerService& PowerService::instance() {
    static PowerService s;
    return s;
}

// ============================================================================
// Profile name helper
// ============================================================================

static const char* profileName(PowerProfile p) {
    switch (p) {
        case PowerProfile::PERFORMANCE: return "performance";
        case PowerProfile::BALANCED:    return "balanced";
        case PowerProfile::POWER_SAVER: return "power_saver";
        case PowerProfile::AUTO:        return "auto";
    }
    return "unknown";
}

static const char* screenStateName(ScreenState s) {
    switch (s) {
        case ScreenState::ON:     return "on";
        case ScreenState::DIMMED: return "dimmed";
        case ScreenState::OFF:    return "off";
    }
    return "unknown";
}

static const char* powerSourceName(PowerSource s) {
    switch (s) {
        case PowerSource::USB:     return "usb";
        case PowerSource::BATTERY: return "battery";
        default:                   return "unknown";
    }
}

// ============================================================================
// Init
// ============================================================================

bool PowerService::init() {
    // Initialize PowerHAL
#ifndef SIMULATOR
    // Try LGFX I2C first (for boards with AXP2101 PMIC)
    bool powerOk = _power.initLgfx(0, 0x34);
    if (!powerOk) {
        DBG_WARN(TAG, "PMIC not found, power monitoring limited");
    }
#else
    bool powerOk = _power.init();
#endif

    // Load settings from NVS
    loadSettings();

    // Get initial power state
    _lastInfo = _power.getInfo();
    _lastActivityMs = millis();
    _lastPollMs = millis();

    // Resolve initial profile
    if (_activeProfile == PowerProfile::AUTO) {
        _resolvedProfile = resolveAutoProfile(_lastInfo);
    } else {
        _resolvedProfile = _activeProfile;
        _userOverride = true;
    }
    applyProfile(_resolvedProfile);

    // Subscribe to touch events for activity tracking
    TritiumEventBus& bus = TritiumEventBus::instance();
    _touchSubId = bus.subscribeCategory(EVT_CAT_TOUCH, onTouchEvent, this);
    _meshSubId = bus.subscribe(EVT_MESH_MESSAGE, onMeshEvent, this);
    _powerSubId = bus.subscribeCategory(EVT_CAT_POWER, onPowerEvent, this);

    // Observe settings changes
    _settingsObsId = TritiumSettings::instance().onChange(
        SettingsDomain::SYSTEM, nullptr, onSettingsChanged, this);

    // Set low battery callback on PowerHAL
    _power.setLowBatteryThreshold(LOW_BATTERY_PCT);

    _active = true;
    DBG_INFO(TAG, "Power service ready, profile=%s, battery=%d%%",
             profileName(_resolvedProfile),
             _lastInfo.percentage);

    return true;
}

// ============================================================================
// Settings load
// ============================================================================

void PowerService::loadSettings() {
    TritiumSettings& s = TritiumSettings::instance();

    int32_t prof = s.getInt(SettingsDomain::SYSTEM, "pwr_profile", 3);
    if (prof >= 0 && prof <= 3) {
        _activeProfile = static_cast<PowerProfile>(prof);
    }

    _dimTimeoutS  = (uint32_t)s.getInt(SettingsDomain::SYSTEM, "pwr_dim_s",   60);
    _offTimeoutS  = (uint32_t)s.getInt(SettingsDomain::SYSTEM, "pwr_off_s",  120);
    _sleepTimeoutS = (uint32_t)s.getInt(SettingsDomain::SYSTEM, "pwr_sleep_s", 300);
    _wakeOnTouch  = s.getBool(SettingsDomain::SYSTEM, "pwr_wake_touch", true);
    _wakeOnMesh   = s.getBool(SettingsDomain::SYSTEM, "pwr_wake_mesh",  true);

    // If profile is not AUTO, user has overridden
    _userOverride = (_activeProfile != PowerProfile::AUTO);
}

// ============================================================================
// Profile application
// ============================================================================

void PowerService::applyProfile(PowerProfile profile) {
    const ProfileDefaults* defs;

    // Check for emergency mode
    if (_lastInfo.percentage >= 0 && _lastInfo.percentage < EMERGENCY_BATTERY_PCT &&
        !_lastInfo.is_usb_powered) {
        defs = &EMERGENCY_DEFAULTS;
        DBG_WARN(TAG, "EMERGENCY power mode, battery=%d%%", _lastInfo.percentage);
    } else {
        int idx = static_cast<int>(profile);
        if (idx < 0 || idx > 2) idx = 1; // fallback to balanced
        defs = &PROFILE_DEFAULTS[idx];
    }

    _resolvedProfile = profile;
    _targetBrightness = defs->brightness;

    // Only override timeouts if user hasn't customized them via settings
    // For auto profile, always apply defaults; for manual, use settings values
    if (_activeProfile == PowerProfile::AUTO) {
        _dimTimeoutS = defs->dim_s;
        _offTimeoutS = defs->off_s;
        _sleepTimeoutS = defs->sleep_s;
    }

    // Apply brightness if screen is currently ON
    if (_screenState == ScreenState::ON) {
        setScreenBrightness(_targetBrightness);
    }

    DBG_INFO(TAG, "Profile applied: %s (brightness=%u, dim=%us, off=%us, sleep=%us)",
             profileName(profile), _targetBrightness,
             (unsigned)_dimTimeoutS, (unsigned)_offTimeoutS, (unsigned)_sleepTimeoutS);
}

PowerProfile PowerService::resolveAutoProfile(const PowerInfo& info) {
    // USB/wall power -> performance
    if (info.is_usb_powered) {
        return PowerProfile::PERFORMANCE;
    }

    // Battery present and low -> power saver
    if (info.has_battery && info.percentage >= 0 && info.percentage < LOW_BATTERY_PCT) {
        return PowerProfile::POWER_SAVER;
    }

    // On battery, adequate charge -> balanced
    return PowerProfile::BALANCED;
}

// ============================================================================
// Screen state machine
// ============================================================================

void PowerService::updateScreenState(uint32_t idle_s) {
    ScreenState newState = _screenState;

    // Timeouts of 0 mean disabled for that transition
    if (_offTimeoutS > 0 && idle_s >= _offTimeoutS) {
        newState = ScreenState::OFF;
    } else if (_dimTimeoutS > 0 && idle_s >= _dimTimeoutS) {
        newState = ScreenState::DIMMED;
    } else {
        newState = ScreenState::ON;
    }

    if (newState == _screenState) return;

    ScreenState oldState = _screenState;
    _screenState = newState;

    switch (newState) {
        case ScreenState::ON:
            setScreenBrightness(_targetBrightness);
            _sleep.displayWake();
            TritiumEventBus::instance().publish(EVT_DISPLAY_WAKE);
            DBG_INFO(TAG, "Screen ON (brightness=%u)", _targetBrightness);
            break;

        case ScreenState::DIMMED: {
            uint8_t dimmed = _targetBrightness / 2;
            if (dimmed < 10) dimmed = 10; // minimum visible
            setScreenBrightness(dimmed);
            DBG_INFO(TAG, "Screen DIMMED (brightness=%u)", dimmed);
            break;
        }

        case ScreenState::OFF:
            setScreenBrightness(0);
            _sleep.displaySleep(true);
            TritiumEventBus::instance().publish(EVT_DISPLAY_SLEEP);
            DBG_INFO(TAG, "Screen OFF");
            break;
    }

    (void)oldState;
}

void PowerService::setScreenBrightness(uint8_t brightness) {
#ifndef SIMULATOR
    display_set_brightness(brightness);
#else
    (void)brightness;
#endif
}

// ============================================================================
// Activity tracking and wake
// ============================================================================

void PowerService::resetActivity() {
    _lastActivityMs = millis();
}

void PowerService::wake() {
    resetActivity();

    if (_screenState != ScreenState::ON) {
        _screenState = ScreenState::ON;
        setScreenBrightness(_targetBrightness);
        _sleep.displayWake();
        TritiumEventBus::instance().publish(EVT_DISPLAY_WAKE);
        DBG_INFO(TAG, "Forced wake, brightness=%u", _targetBrightness);
    }
}

PowerProfile PowerService::getProfile() const { return _activeProfile; }
ScreenState PowerService::getScreenState() const { return _screenState; }

uint32_t PowerService::getIdleSeconds() const {
    uint32_t now = millis();
    return (now - _lastActivityMs) / 1000;
}

int PowerService::getBatteryPercent() const { return _lastInfo.percentage; }
bool PowerService::isBatteryCharging() const { return _lastInfo.is_charging; }
bool PowerService::hasBattery() const { return _lastInfo.has_battery || _power.hasPMIC(); }

// ============================================================================
// Tick — main loop, called every frame
// ============================================================================

void PowerService::tick() {
    if (!_active) return;

    uint32_t now = millis();

    // Rate-limit polling to once per second
    if (now - _lastPollMs < POLL_INTERVAL_MS) return;
    _lastPollMs = now;

    // Poll power HAL for battery updates
    _power.poll();
    PowerInfo info = _power.getInfo();

    // Detect power source transitions for auto profile
    bool sourceChanged = (info.is_usb_powered != _lastInfo.is_usb_powered);
    bool chargingChanged = (info.is_charging != _lastInfo.is_charging);
    bool crossedLowThreshold = false;

    if (info.percentage >= 0 && _lastInfo.percentage >= 0) {
        bool wasLow = (_lastInfo.percentage < LOW_BATTERY_PCT);
        bool isLow = (info.percentage < LOW_BATTERY_PCT);
        crossedLowThreshold = (wasLow != isLow);
    }

    _lastInfo = info;

    // Auto-profile switching (only if user hasn't manually overridden)
    if (_activeProfile == PowerProfile::AUTO &&
        (sourceChanged || crossedLowThreshold || chargingChanged)) {
        PowerProfile newProfile = resolveAutoProfile(info);
        if (newProfile != _resolvedProfile) {
            DBG_INFO(TAG, "Auto-switch: %s -> %s (usb=%d, bat=%d%%)",
                     profileName(_resolvedProfile), profileName(newProfile),
                     info.is_usb_powered, info.percentage);
            applyProfile(newProfile);

            // Wake screen on power source change (plugging in/unplugging)
            if (sourceChanged) {
                wake();
            }
        }
    }

    // Emergency mode check (overrides everything)
    if (!info.is_usb_powered && info.percentage >= 0 &&
        info.percentage < EMERGENCY_BATTERY_PCT) {
        // Re-apply to get emergency defaults
        applyProfile(_resolvedProfile);
    }

    // Screen state machine
    uint32_t idle_s = getIdleSeconds();
    updateScreenState(idle_s);

    // Light sleep check — only if screen is already off and sleep is enabled
    if (_screenState == ScreenState::OFF && _sleepTimeoutS > 0 &&
        idle_s >= _sleepTimeoutS) {
        DBG_INFO(TAG, "Entering light sleep after %us idle", (unsigned)idle_s);
        TritiumEventBus::instance().publish(EVT_SYSTEM_SLEEP);

        // Configure wake sources
        _sleep.setWakeTimer(5 * 1000000ULL); // Wake every 5s to check for activity
        _sleep.setWakeUART(0);               // Wake on serial input

        // Enter light sleep — execution pauses here
        _sleep.sleep(SleepMode::LIGHT_SLEEP);

        // Resumed from light sleep
        TritiumEventBus::instance().publish(EVT_SYSTEM_WAKE);
        DBG_INFO(TAG, "Woke from light sleep");

        // Don't auto-wake screen; let next touch/serial/mesh event trigger wake()
        _lastPollMs = millis();
    }
}

// ============================================================================
// Event handlers
// ============================================================================

void PowerService::onTouchEvent(const TritiumEvent& event, void* user_data) {
    (void)event;
    auto* self = static_cast<PowerService*>(user_data);
    if (!self->_wakeOnTouch) return;
    self->wake();
}

void PowerService::onMeshEvent(const TritiumEvent& event, void* user_data) {
    (void)event;
    auto* self = static_cast<PowerService*>(user_data);
    if (!self->_wakeOnMesh) return;
    self->resetActivity();
    // Wake screen on mesh message if it's off
    if (self->_screenState == ScreenState::OFF) {
        self->wake();
    }
}

void PowerService::onPowerEvent(const TritiumEvent& event, void* user_data) {
    auto* self = static_cast<PowerService*>(user_data);

    // USB plug/unplug triggers immediate re-evaluation
    if (event.id == EVT_POWER_USB_CONNECT || event.id == EVT_POWER_USB_DISCONNECT) {
        self->resetActivity();
        // Auto-profile will re-evaluate on next tick
    }
}

void PowerService::onSettingsChanged(const char* domain, const char* key, void* user_data) {
    if (strcmp(domain, SettingsDomain::SYSTEM) != 0) return;
    if (!key) return;

    // Only reload if it's one of our keys
    if (strncmp(key, "pwr_", 4) != 0) return;

    auto* self = static_cast<PowerService*>(user_data);
    DBG_INFO(TAG, "Settings changed: %s.%s, reloading", domain, key);
    self->loadSettings();

    // Re-apply current profile with new settings
    if (self->_activeProfile == PowerProfile::AUTO) {
        PowerProfile newProfile = self->resolveAutoProfile(self->_lastInfo);
        self->applyProfile(newProfile);
    } else {
        self->applyProfile(self->_activeProfile);
    }
}

// ============================================================================
// Serial commands
// ============================================================================

bool PowerService::handleCommand(const char* cmd, const char* args) {
    if (strcmp(cmd, "POWER") == 0) {
        static char buf[512];
        int n = toJson(buf, sizeof(buf));
        if (n > 0) {
#ifndef SIMULATOR
            Serial.printf("%s\n", buf);
#else
            printf("%s\n", buf);
#endif
        }
        return true;
    }

    if (strcmp(cmd, "POWER_PROFILE") == 0) {
        if (!args || !*args) {
#ifndef SIMULATOR
            Serial.printf("[power] Usage: POWER_PROFILE <0-3> (0=performance, 1=balanced, 2=saver, 3=auto)\n");
            Serial.printf("[power] Current: %s\n", profileName(_resolvedProfile));
#endif
            return true;
        }

        // Skip whitespace
        while (*args == ' ') args++;

        int val = args[0] - '0';
        if (val < 0 || val > 3) {
#ifndef SIMULATOR
            Serial.printf("[power] Invalid profile: %s (must be 0-3)\n", args);
#endif
            return true;
        }

        PowerProfile newProfile = static_cast<PowerProfile>(val);
        _activeProfile = newProfile;
        _userOverride = (newProfile != PowerProfile::AUTO);

        // Persist to NVS
        TritiumSettings::instance().setInt(SettingsDomain::SYSTEM, "pwr_profile", val);

        if (newProfile == PowerProfile::AUTO) {
            PowerProfile resolved = resolveAutoProfile(_lastInfo);
            applyProfile(resolved);
            DBG_INFO(TAG, "Profile set to AUTO (resolved: %s)", profileName(resolved));
        } else {
            applyProfile(newProfile);
            DBG_INFO(TAG, "Profile set to %s (manual override)", profileName(newProfile));
        }

#ifndef SIMULATOR
        Serial.printf("[power] Profile set: %s\n", profileName(
            newProfile == PowerProfile::AUTO ? _resolvedProfile : newProfile));
#endif
        return true;
    }

    if (strcmp(cmd, "POWER_WAKE") == 0) {
        wake();
#ifndef SIMULATOR
        Serial.printf("[power] Forced wake\n");
#endif
        return true;
    }

    return false;
}

// ============================================================================
// JSON API
// ============================================================================

int PowerService::toJson(char* buf, size_t size) {
    PowerInfo info = _power.getInfo();
    uint32_t idle_s = getIdleSeconds();

    int n = snprintf(buf, size,
        "{"
        "\"profile\":\"%s\","
        "\"auto_profile\":%s,"
        "\"battery_pct\":%d,"
        "\"voltage\":%.2f,"
        "\"charging\":%s,"
        "\"usb_powered\":%s,"
        "\"source\":\"%s\","
        "\"screen_state\":\"%s\","
        "\"idle_seconds\":%u,"
        "\"dim_timeout\":%u,"
        "\"off_timeout\":%u,"
        "\"sleep_timeout\":%u"
        "}",
        profileName(_resolvedProfile),
        (_activeProfile == PowerProfile::AUTO) ? "true" : "false",
        info.percentage,
        (double)info.voltage,
        info.is_charging ? "true" : "false",
        info.is_usb_powered ? "true" : "false",
        powerSourceName(info.source),
        screenStateName(_screenState),
        (unsigned)idle_s,
        (unsigned)_dimTimeoutS,
        (unsigned)_offTimeoutS,
        (unsigned)_sleepTimeoutS
    );

    if (n < 0 || (size_t)n >= size) {
        if (size > 0) buf[0] = '\0';
        return -1;
    }

    return n;
}

#else // ENABLE_POWER_MGMT not defined or hal_power.h not available

// ============================================================================
// Stub implementation when power management is disabled
// ============================================================================

PowerService& PowerService::instance() {
    static PowerService s;
    return s;
}

bool PowerService::init() { return false; }
void PowerService::tick() {}
void PowerService::resetActivity() {}
void PowerService::wake() {}
PowerProfile PowerService::getProfile() const { return PowerProfile::AUTO; }
ScreenState PowerService::getScreenState() const { return ScreenState::ON; }
uint32_t PowerService::getIdleSeconds() const { return 0; }
int PowerService::getBatteryPercent() const { return -1; }
bool PowerService::isBatteryCharging() const { return false; }
bool PowerService::hasBattery() const { return false; }

bool PowerService::handleCommand(const char* cmd, const char* args) {
    (void)cmd;
    (void)args;
    return false;
}

int PowerService::toJson(char* buf, size_t size) {
    (void)buf;
    (void)size;
    return 0;
}

#endif // ENABLE_POWER_MGMT
