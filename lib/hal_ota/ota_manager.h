// Tritium-OS OTA Manager — enhanced firmware update system with web UI
// and mesh distribution capabilities.
// Copyright 2026 Valpatel Software LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <stdint.h>
#include <stddef.h>

namespace ota_manager {

enum OtaState : uint8_t {
    OTA_IDLE,
    OTA_CHECKING,
    OTA_DOWNLOADING,
    OTA_WRITING,
    OTA_VERIFYING,
    OTA_READY_REBOOT,
    OTA_FAILED,
};

struct OtaStatus {
    OtaState state;
    uint8_t progress_pct;      // 0-100
    uint32_t bytes_written;
    uint32_t total_bytes;
    char current_version[32];
    char new_version[32];
    char error_msg[64];
    const char* active_partition;
    const char* next_partition;
    uint32_t partition_size;
};

struct OtaHistoryEntry {
    char version[32];
    uint32_t timestamp;
    bool success;
    char source[16];  // "web", "url", "sd", "mesh"
};

typedef void (*OtaProgressCallback)(const OtaStatus& status, void* user_data);

bool init();
const OtaStatus& getStatus();

// Update sources
bool updateFromUpload(const uint8_t* data, size_t len, bool final_chunk);
bool updateFromUrl(const char* url);
bool updateFromSD(const char* path);

// Post-update
bool rollback();
bool reboot();
bool markValid();  // Mark current firmware as good (prevent auto-rollback)

// Progress
void onProgress(OtaProgressCallback cb, void* user_data = nullptr);

// History
int getHistory(OtaHistoryEntry* entries, int max_count);

// Mesh distribution
bool meshDistribute();  // Push current firmware to all mesh peers

}  // namespace ota_manager
