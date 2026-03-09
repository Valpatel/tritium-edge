// Tritium-OS Settings Framework
// Unified key-value settings storage with NVS backing and change notifications.
//
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <cstdint>
#include <cstddef>

// Maximum number of concurrent observers
static constexpr int SETTINGS_MAX_OBSERVERS = 32;

// Maximum key length (NVS limit is 15 chars; namespace = "trit_" + domain truncated)
static constexpr int SETTINGS_MAX_KEY_LEN = 15;

// Maximum string value length
static constexpr int SETTINGS_MAX_STRING_LEN = 128;

// Settings value types
enum class SettingsType : uint8_t {
    BOOL,
    INT32,
    FLOAT,
    STRING,
    BLOB
};

// Settings domains — each maps to an NVS namespace "trit_<domain>"
namespace SettingsDomain {
    static constexpr const char* SYSTEM      = "system";
    static constexpr const char* SCREEN      = "display";   // "DISPLAY" conflicts with Arduino.h #define
    static constexpr const char* WIFI        = "wifi";
    static constexpr const char* BLUETOOTH   = "bluetooth";
    static constexpr const char* MESH        = "mesh";
    static constexpr const char* APPS        = "apps";
    static constexpr const char* FLEET       = "fleet";
    static constexpr const char* DEVELOPER   = "developer";
    static constexpr const char* SCREENSAVER = "screensaver";
}

// Callback signature for settings change observers
typedef void (*SettingsCallback)(const char* domain, const char* key, void* user_data);

// Factory default entry — compiled-in default values
struct SettingsDefault {
    const char* domain;
    const char* key;
    SettingsType type;
    union {
        bool   b;
        int32_t i;
        float   f;
        const char* s;
    } value;
};

// Factory defaults table — defined in os_settings.cpp, accessible for type lookups
extern const SettingsDefault FACTORY_DEFAULTS[];
extern const int FACTORY_DEFAULTS_COUNT;

// ---------------------------------------------------------------------------
// TritiumSettings — singleton settings manager
// ---------------------------------------------------------------------------
class TritiumSettings {
public:
    static TritiumSettings& instance();

    // Initialize NVS and load factory defaults for any missing keys.
    // Call once at boot, before any other settings access.
    bool init();

    // -----------------------------------------------------------------------
    // Typed getters — return the stored value or default_val if not found
    // -----------------------------------------------------------------------
    bool        getBool(const char* domain, const char* key, bool default_val = false);
    int32_t     getInt(const char* domain, const char* key, int32_t default_val = 0);
    float       getFloat(const char* domain, const char* key, float default_val = 0.0f);
    const char* getString(const char* domain, const char* key,
                          const char* default_val = "");

    // -----------------------------------------------------------------------
    // Typed setters — persist to NVS and notify observers. Returns true on success.
    // -----------------------------------------------------------------------
    bool setBool(const char* domain, const char* key, bool value);
    bool setInt(const char* domain, const char* key, int32_t value);
    bool setFloat(const char* domain, const char* key, float value);
    bool setString(const char* domain, const char* key, const char* value);

    // -----------------------------------------------------------------------
    // Observer pattern
    // -----------------------------------------------------------------------

    // Register an observer for changes to a specific domain/key.
    // Pass nullptr for key to observe all keys in the domain.
    // Pass nullptr for domain to observe all settings changes.
    // Returns observer_id (>= 0) on success, -1 on failure.
    int onChange(const char* domain, const char* key,
                SettingsCallback cb, void* user_data = nullptr);

    // Remove a previously registered observer by ID.
    void removeObserver(int observer_id);

    // -----------------------------------------------------------------------
    // Bulk operations
    // -----------------------------------------------------------------------

    // Export settings as JSON into buf. If domain is non-null, export only that
    // domain; otherwise export all domains. Returns bytes written (excluding NUL),
    // or -1 on error. Output is always NUL-terminated if size > 0.
    int toJson(char* buf, size_t size, const char* domain = nullptr);

    // Import settings from a JSON string. Returns true if at least one key was set.
    bool fromJson(const char* json);

    // Reset to factory defaults. If domain is non-null, reset only that domain.
    // Returns true on success.
    bool factoryReset(const char* domain = nullptr);

    // -----------------------------------------------------------------------
    // Key iteration
    // -----------------------------------------------------------------------

    // Populate `keys` array with pointers to key names in the given domain.
    // Returns number of keys found (up to max_keys).
    // NOTE: returned pointers are valid only until the next settings call.
    int getKeys(const char* domain, const char** keys, int max_keys);

private:
    TritiumSettings() = default;
    ~TritiumSettings() = default;
    TritiumSettings(const TritiumSettings&) = delete;
    TritiumSettings& operator=(const TritiumSettings&) = delete;

    // Build the NVS namespace string for a domain: "trit_<domain>" (max 15 chars)
    static void makeNamespace(const char* domain, char* ns, size_t ns_size);

    // Notify all matching observers
    void notifyObservers(const char* domain, const char* key);

    // Apply factory defaults for a single domain (or all if domain == nullptr)
    void applyDefaults(const char* domain = nullptr);

    // Clear all keys in an NVS namespace
    bool clearNamespace(const char* ns);

    // Observer storage
    struct Observer {
        bool active;
        char domain[16];      // empty = wildcard (all domains)
        char key[16];         // empty = wildcard (all keys in domain)
        SettingsCallback cb;
        void* user_data;
    };
    Observer _observers[SETTINGS_MAX_OBSERVERS] = {};
    int _nextObserverId = 0;

    // Scratch buffer for getString return values (caller must copy)
    char _strBuf[SETTINGS_MAX_STRING_LEN] = {};

    // Scratch buffer for key iteration
    static constexpr int MAX_ITER_KEYS = 32;
    char _keyBuf[MAX_ITER_KEYS][16] = {};

    bool _initialized = false;

    // Mutex handle (void* to avoid pulling in FreeRTOS headers here)
    void* _mutex = nullptr;
};
