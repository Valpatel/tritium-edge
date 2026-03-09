#pragma once
// Self-seeding firmware distribution service adapter — wraps SeedHAL as a ServiceInterface.
// Priority 70.

#include "service.h"

#if defined(HAS_SDCARD) && HAS_SDCARD && __has_include("hal_seed.h")
#include "hal_seed.h"
#include "hal_sdcard.h"
#endif

class SeedService : public ServiceInterface {
public:
    const char* name() const override { return "seed"; }
    uint8_t capabilities() const override { return SVC_SERIAL_CMD; }
    int initPriority() const override { return 70; }

    bool init() override {
#if defined(HAS_SDCARD) && HAS_SDCARD && __has_include("hal_seed.h")
        if (_sd.init()) {
            _seed_ok = _seed.init(&_sd, nullptr);
            Serial.printf("[tritium] Self-seed: %s\n", _seed_ok ? "OK" : "FAIL");
            return _seed_ok;
        } else {
            Serial.printf("[tritium] Self-seed: SD card not available\n");
            return false;
        }
#else
        return false;
#endif
    }

    bool handleCommand(const char* cmd, const char* args) override {
#if defined(HAS_SDCARD) && HAS_SDCARD && __has_include("hal_seed.h")
        if (strcmp(cmd, "SEED_STATUS") == 0) {
            if (!_seed_ok) {
                Serial.printf("[seed] Not initialized\n");
            } else {
                SeedManifest m;
                bool has_manifest = _seed.getManifest(m);
                Serial.printf("[seed] available=%s manifest=%s",
                              _seed.hasSeedPayload() ? "yes" : "no",
                              has_manifest ? "yes" : "no");
                if (has_manifest) {
                    uint32_t total = 0;
                    for (int i = 0; i < m.file_count; i++) total += m.files[i].size;
                    Serial.printf(" files=%d size=%luKB fw=%s board=%s",
                                  m.file_count, (unsigned long)(total / 1024),
                                  m.firmware_version, m.board_type);
                }
                Serial.printf(" sd=%s\n", _seed.hasSD() ? "yes" : "no");
            }
            return true;
        }
        if (strcmp(cmd, "SEED_LIST") == 0) {
            if (!_seed_ok) {
                Serial.printf("[seed] Not initialized\n");
            } else {
                SeedManifest m;
                if (_seed.getManifest(m)) {
                    Serial.printf("[seed] %d files in manifest:\n", m.file_count);
                    for (int i = 0; i < m.file_count; i++) {
                        Serial.printf("  %s  %lu bytes  %s\n",
                                      m.files[i].path,
                                      (unsigned long)m.files[i].size,
                                      m.files[i].sha256);
                    }
                } else {
                    Serial.printf("[seed] No manifest found. Run SEED_CREATE first.\n");
                }
            }
            return true;
        }
        if (strcmp(cmd, "SEED_CREATE") == 0) {
            if (!_seed_ok) {
                Serial.printf("[seed] Not initialized\n");
            } else {
                Serial.printf("[seed] Creating seed package...\n");
                if (_seed.createSeedPackage()) {
                    SeedManifest m;
                    _seed.getManifest(m);
                    Serial.printf("[seed] Package created: %d files, fw=%s\n",
                                  m.file_count, m.firmware_version);
                } else {
                    Serial.printf("[seed] Package creation failed\n");
                }
            }
            return true;
        }
#endif
        return false;
    }

private:
#if defined(HAS_SDCARD) && HAS_SDCARD && __has_include("hal_seed.h")
    SDCardHAL _sd;
    SeedHAL _seed;
    bool _seed_ok = false;
#endif
};
