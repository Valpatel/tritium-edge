/*
 * SPDX-FileCopyrightText: 2026 Valpatel Software LLC
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file shell_apps.h
 * @brief Built-in system apps for the Tritium-OS shell.
 *
 * Each function creates its UI inside the provided viewport container.
 * These are registered with tritium_shell::registerApp() during shell init.
 */

#pragma once
#include "os_shell.h"

namespace shell_apps {

/// Settings app — display brightness, WiFi info, system controls.
void settings_create(lv_obj_t* viewport);

/// About screen — version, board, memory stats, uptime.
void about_create(lv_obj_t* viewport);

/// Brightness control — slider for display backlight.
void brightness_create(lv_obj_t* viewport);

/// WiFi manager — scan, connect, saved networks, AP mode.
void wifi_app_create(lv_obj_t* viewport);

/// System monitor — heap, PSRAM, CPU, temperature, tasks.
void sysmon_app_create(lv_obj_t* viewport);

/// Mesh viewer — peers, topology, broadcast, stats.
void mesh_app_create(lv_obj_t* viewport);

/// File browser — navigate LittleFS and SD card.
void files_app_create(lv_obj_t* viewport);

/// Power manager — battery, profiles, sleep timeouts.
void power_app_create(lv_obj_t* viewport);

/// On-device terminal — serial command console with scrollback.
void terminal_create(lv_obj_t* viewport);

/// Mesh Chat — P2P messaging over ESP-NOW mesh.
void mesh_chat_create(lv_obj_t* viewport);

/// Map viewer — offline tile map from MBTiles on SD card.
void map_app_create(lv_obj_t* viewport);

/// Delete any active LVGL timers from shell apps (call before switching apps).
void cleanup_timers();

/// Register all new shell apps with the launcher.
void register_all_apps();

}  // namespace shell_apps
