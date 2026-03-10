# Tritium-OS: A Software-Defined Operating System for ESP32

## Vision

Tritium-OS is a **universal operating system for ESP32-S3 hardware** that transforms commodity dev boards into a managed fleet of intelligent edge devices. One binary runs on all supported hardware. The device self-identifies, self-configures, and joins a managed mesh — no manual provisioning needed.

**Core principle:** Plug in any supported board, it boots Tritium-OS, fingerprints its own hardware, initializes the correct display and peripherals, connects to the network, and becomes remotely manageable — all without flashing board-specific firmware.

---

## Architecture Overview

```
+--------------------------------------------------------------------+
|  TRITIUM-OS                                                         |
|                                                                     |
|  +---------------------------+  +-------------------------------+   |
|  |  SHELL (Display + Touch)  |  |  REMOTE SHELL (Web + Serial)  |   |
|  |  Window Manager           |  |  Dashboard + REST API          |   |
|  |  Status Bar               |  |  WebSocket Live Updates        |   |
|  |  App Launcher             |  |  Terminal + File Manager       |   |
|  |  Notifications            |  |  OTA + Config                  |   |
|  +---------------------------+  +-------------------------------+   |
|                                                                     |
|  +--------------------------------------------------------------+  |
|  |  APP RUNTIME                                                  |  |
|  |  Foreground App  |  Background Services  |  System Daemons    |  |
|  +--------------------------------------------------------------+  |
|                                                                     |
|  +--------------------------------------------------------------+  |
|  |  CORE OS                                                      |  |
|  |  Service Registry | Settings | Events | Scheduler | Storage   |  |
|  +--------------------------------------------------------------+  |
|                                                                     |
|  +--------------------------------------------------------------+  |
|  |  NETWORK STACK                                                |  |
|  |  WiFi Manager | ESP-NOW Mesh | BLE Serial | mDNS | MQTT      |  |
|  +--------------------------------------------------------------+  |
|                                                                     |
|  +--------------------------------------------------------------+  |
|  |  HARDWARE ABSTRACTION LAYER                                   |  |
|  |  Display | Touch | Camera | Audio | IMU | RTC | PMIC | GPIO  |  |
|  +--------------------------------------------------------------+  |
|                                                                     |
|  +--------------------------------------------------------------+  |
|  |  BOARD SUPPORT PACKAGE (BSP)                                  |  |
|  |  Fingerprint -> board_config_t -> runtime peripheral init     |  |
|  +--------------------------------------------------------------+  |
+--------------------------------------------------------------------+
```

---

## What Already Exists (Foundation)

| Layer | Component | Status | Notes |
|-------|-----------|--------|-------|
| BSP | Board fingerprinting | **Done** | 6 boards, 100% confidence, 154ms |
| BSP | Universal firmware | **Done** | Single binary, runtime display dispatch |
| BSP | Board configs | **Done** | All 6 boards as runtime data |
| HAL | Display (3 drivers) | **Done** | AXS15231B, SH8601, RGB parallel |
| HAL | Touch (4 controllers) | **Done** | FT3168, FT6336, GT911, AXS15231B |
| HAL | WiFi (multi-network) | **Done** | NVS persistence, auto-reconnect |
| HAL | BLE scanner | **Done** | 64 devices, 2-min timeout |
| HAL | ESP-NOW mesh | **Done** | Multi-hop, peer discovery, dedup |
| HAL | OTA | **Done** | WiFi push/pull, SD card, dual partition |
| HAL | File systems | **Done** | LittleFS + SD card |
| HAL | Camera, Audio, IMU, RTC, PMIC | **Done** | Full peripheral coverage |
| HAL | Sleep/wake | **Done** | Light/deep sleep, multiple wake sources |
| Core | Service registry | **Done** | Priority init, tick dispatch, serial commands |
| Core | Serial command protocol | **Done** | IDENTIFY, SERVICES, per-service commands |
| Remote | Web server | **Done** | Dashboard, REST API, mDNS |
| Remote | Screenshot capture | **Done** | RGB565 framebuffer via HTTP |
| Remote | Diagnostics | **Done** | Health, events, anomaly detection |
| Remote | Heartbeat/fleet | **Done** | Periodic status to management server |
| Shell | LVGL framework | **Done** | Theme, widgets, display driver |
| Shell | Boot splash | **Done** | Animated tritium logo |
| Apps | 10 applications | **Done** | Starfield, camera, system, diag, etc. |

---

## What Needs to Be Built

### Phase 1: Core OS Services

#### 1.1 Settings Framework
**Priority: Critical**

A unified key-value settings store that all services and apps can use. Replaces the current scattered NVS usage.

```
Settings domains:
  system.*        — device name, timezone, locale, auto-sleep timeout
  display.*       — brightness, orientation, sleep timeout
  wifi.*          — saved networks[], auto-connect, AP mode SSID/pass
  bluetooth.*     — device name, discoverability, serial baud
  mesh.*          — role (node/gateway/leaf), channel, encryption key
  apps.*          — per-app preferences
  fleet.*         — server URL, heartbeat interval, device ID
```

**Features:**
- NVS-backed with LittleFS fallback for large values
- Change notifications (observer pattern) — services react to setting changes
- Web API: `GET/PUT /api/settings/{domain}/{key}`
- Serial: `SET wifi.ssid MyNetwork`, `GET system.name`
- Import/export as JSON for fleet provisioning
- Factory defaults per domain

#### 1.2 Event Bus
**Priority: Critical**

Inter-service communication without tight coupling. Services publish events, others subscribe.

```
Events:
  system.boot, system.sleep, system.wake
  wifi.connected, wifi.disconnected, wifi.scan_complete
  ble.device_found, ble.device_lost
  mesh.peer_joined, mesh.peer_left, mesh.message
  touch.tap, touch.swipe, touch.long_press
  app.switch, app.notification
  ota.available, ota.progress, ota.complete
  power.low_battery, power.charging, power.usb_connected
```

**Features:**
- Synchronous (direct callback) and async (FreeRTOS queue) subscribers
- Event filtering by pattern (`wifi.*`, `*.connected`)
- Event history ring buffer (last 100 events) for diagnostics
- WebSocket bridge — frontend receives real-time events

#### 1.3 Task Scheduler
**Priority: High**

FreeRTOS task management with named tasks, priorities, stack monitoring.

```
System tasks:
  tritium.display    — frame rendering, 60 FPS target
  tritium.network    — WiFi, mesh, heartbeat
  tritium.services   — service tick dispatch
  tritium.events     — event bus processing
  tritium.watchdog   — health monitoring
```

**Features:**
- Named task creation with stack overflow detection
- CPU usage per task (via `uxTaskGetSystemState`)
- Deadlock detection (watchdog timeout per task)
- Graceful shutdown ordering

---

### Phase 2: Network Management

#### 2.1 WiFi Manager (Enhanced)
**Priority: Critical**

Extend the existing `hal_wifi` into a full WiFi management system.

**Features:**
- **Multiple saved networks** (up to 10) with priority ordering
- **Auto-failover**: if primary drops, try networks in priority order
- **Signal quality tracking**: per-network RSSI history, connection reliability score
- **AP mode**: built-in captive portal for initial setup
  - SSID: `Tritium-XXXX` (last 4 of MAC)
  - Password: `tritium` (configurable)
  - Captive portal redirects to setup wizard
- **Web UI** at `/wifi`:
  - Scan for networks (with signal strength bars)
  - Add/remove/reorder saved networks
  - Current connection status + IP info
  - AP mode toggle
  - DNS settings, static IP option
- **Auto-channel selection** for AP mode
- **WPS support** (push-button)
- **Enterprise WiFi** (WPA2-Enterprise with certificates from provisioning)

#### 2.2 BLE Serial Port
**Priority: High**

UART-over-BLE for serial terminal access when no WiFi is available.

**Features:**
- Nordic UART Service (NUS) — compatible with standard BLE terminal apps
- Full serial command protocol over BLE (same as USB serial)
- Auto-advertise when WiFi is disconnected
- Configurable: always-on, WiFi-fallback, or disabled
- Pairing with PIN code
- Multiple simultaneous connections (up to 3)
- Binary transfer mode for file upload/download

#### 2.3 Mesh Networking (Enhanced)
**Priority: High**

Extend ESP-NOW mesh into a reliable P2P network layer.

**Features:**
- **Auto-discovery**: new devices automatically join mesh
- **Topology visualization**: web UI shows mesh graph with RSSI links
- **Message routing**: addressed messages (not just broadcast flooding)
- **Shared state**: distributed key-value store across mesh
  - e.g., all nodes share their sensor readings, battery levels
- **Gateway election**: mesh auto-elects WiFi-connected node as gateway
- **Firmware distribution**: gateway can push OTA to mesh peers
- **Encrypted channels**: AES-128 encryption with shared mesh key
- **Latency measurement**: round-trip time per peer
- **Mesh roles**:
  - `GATEWAY` — bridges mesh to WiFi/server
  - `RELAY` — forwards messages, battery permitting
  - `LEAF` — doesn't relay, low power
  - `SENSOR` — deep sleep, wake-transmit-sleep cycle

#### 2.4 MQTT Client
**Priority: Medium**

Persistent MQTT connection for cloud integration.

**Features:**
- Auto-reconnect with exponential backoff
- Last Will and Testament (device offline detection)
- Topic hierarchy: `tritium/{device_id}/status`, `tritium/{device_id}/command`
- QoS levels 0, 1, 2
- TLS support with certificate from provisioning
- Bridge to event bus: MQTT messages become internal events

---

### Phase 3: On-Device Shell (Display UI)

#### 3.1 Window Manager
**Priority: Critical**

LVGL-based window management system with consistent chrome across all apps.

**Shell Components:**
```
+------------------------------------------+
| STATUS BAR (always visible, 24px)        |
| WiFi: -67dBm  BLE: 3  Mesh: 5  Bat: 87%|
+------------------------------------------+
|                                          |
|                                          |
|          ACTIVE APP VIEWPORT             |
|                                          |
|                                          |
+------------------------------------------+
| NAV BAR (swipe up to show, 48px)         |
| [Home] [Back] [Apps] [Settings] [Lock]   |
+------------------------------------------+
```

**Status Bar indicators:**
- WiFi signal strength (icon + dBm)
- BLE device count
- Mesh peer count
- Battery percentage + charging icon
- Clock (from RTC or NTP)
- Notification dot
- Active app name

**Navigation:**
- Swipe up from bottom: show nav bar
- Swipe down from top: notification shade
- Swipe left/right: switch between running apps
- Long press home: app launcher grid
- Hardware: use any available button as home key

#### 3.2 App Launcher
**Priority: High**

Grid/list of installed apps with icons and descriptions.

```
+------------------------------------------+
|  TRITIUM-OS                    12:34 PM  |
+------------------------------------------+
|                                          |
|  [Settings]  [Files]   [WiFi]           |
|                                          |
|  [Terminal]  [Camera]  [System]          |
|                                          |
|  [Starfield] [Effects] [Mesh]           |
|                                          |
|  [OTA]       [Diag]    [BLE]            |
|                                          |
+------------------------------------------+
```

**Features:**
- App icons (32x32 RGB565 bitmaps stored in flash)
- App metadata: name, description, version, author, capabilities
- Long-press for app info / force stop
- Recently used apps at top
- System apps vs user apps distinction

#### 3.3 Notification System
**Priority: Medium**

Toast notifications + notification shade.

**Notification types:**
- **Toast**: brief overlay (3s), auto-dismiss — "WiFi Connected"
- **Persistent**: stays in shade until dismissed — "OTA Update Available"
- **Alert**: modal dialog requiring action — "Low Battery: 5%"
- **Progress**: progress bar overlay — "Updating firmware... 45%"

**Sources:**
- WiFi events (connect/disconnect/scan)
- BLE events (device found/lost)
- Mesh events (peer join/leave)
- OTA events (update available/progress/complete)
- Power events (low battery, charging)
- App-generated notifications

#### 3.4 Lock Screen
**Priority: Low**

Optional lock screen with clock display.

**Features:**
- Large clock (NTP or RTC synced)
- Date, weather (if available from mesh peer)
- Battery status
- Notification preview
- Swipe to unlock (no PIN — embedded device)
- Auto-lock on inactivity timeout
- "Find my device" — screen flashes on mesh command

---

### Phase 4: System Apps

#### 4.1 Settings App
**Priority: Critical**

On-device settings with LVGL UI.

```
Settings
├── Display
│   ├── Brightness (slider)
│   ├── Auto-brightness (toggle)
│   ├── Screen timeout (30s/1m/5m/never)
│   └── Orientation (auto/portrait/landscape)
├── WiFi
│   ├── Enable/Disable
│   ├── Saved Networks (list, drag to reorder)
│   ├── Add Network (scan + manual SSID)
│   ├── AP Mode (toggle + SSID/password)
│   └── Advanced (DNS, static IP, proxy)
├── Bluetooth
│   ├── Enable/Disable
│   ├── Device Name
│   ├── Serial Port (enable/disable/baud rate)
│   └── Paired Devices
├── Mesh
│   ├── Enable/Disable
│   ├── Role (gateway/relay/leaf)
│   ├── Channel
│   ├── Encryption Key
│   └── Peer List
├── System
│   ├── Device Name
│   ├── Timezone
│   ├── Date & Time (manual or NTP)
│   ├── About (firmware version, board, MAC, uptime)
│   ├── Factory Reset
│   └── Reboot
├── Fleet
│   ├── Server URL
│   ├── Heartbeat Interval
│   ├── Device ID
│   └── Auto-Update (toggle)
└── Developer
    ├── Serial Log Level
    ├── I2C Bus Scan
    ├── GPIO Test
    ├── Memory Stats
    └── Enable Debug Overlay
```

#### 4.2 File Manager App
**Priority: High**

On-device + web file browser for LittleFS and SD card.

**On-device (LVGL):**
- Tree view of file system
- File operations: rename, delete, copy, move
- Preview: text files, images (if display supports resolution)
- Storage usage bar

**Web UI (`/files`):**
- Drag-and-drop file upload
- Download files/folders (zip)
- Create/edit text files inline
- File size, modification time
- Multi-select operations
- Storage statistics (used/free/total for both LittleFS and SD)

**API:**
```
GET    /api/fs/list?path=/&storage=littlefs
GET    /api/fs/read?path=/config.json
POST   /api/fs/write?path=/config.json          (body: file content)
POST   /api/fs/upload?path=/                     (multipart form)
DELETE /api/fs/delete?path=/old_file.txt
POST   /api/fs/mkdir?path=/new_dir
GET    /api/fs/download?path=/logs/boot.log
GET    /api/fs/stats                             (storage usage)
```

#### 4.3 Terminal App
**Priority: Medium**

On-device serial terminal (LVGL text console).

**Features:**
- Full serial command protocol access
- Scrollback buffer (100 lines)
- Auto-scroll with pause on scroll-up
- Command history (up/down arrows)
- Monospace font rendering
- Copy output to clipboard (web only)
- Color support (ANSI escape codes)

**Web version (`/terminal`):**
- WebSocket-based real-time serial console
- Input field at bottom
- ANSI color rendering
- Downloadable log export

#### 4.4 OTA Manager App
**Priority: High**

Firmware update management.

**On-device:**
- Current firmware version display
- Check for updates (from fleet server)
- Download + install progress bar
- Rollback to previous firmware
- Reboot to apply

**Web UI (`/ota`):**
- Drag-and-drop firmware.bin upload
- Progress bar with percentage
- Version comparison (current vs uploaded)
- Update all mesh peers (gateway distributes to mesh)
- Update history log
- Rollback button

**API:**
```
GET    /api/ota/status         (current version, partition info)
POST   /api/ota/upload         (firmware.bin multipart)
POST   /api/ota/update-url     (body: {"url": "http://..."})
POST   /api/ota/rollback
POST   /api/ota/reboot
GET    /api/ota/history
POST   /api/ota/mesh-update    (push to all mesh peers)
```

#### 4.5 Mesh Viewer App
**Priority: Medium**

Visualize the ESP-NOW mesh network.

**On-device:**
- List view of peers with RSSI, hop count, last seen
- Role indicators (gateway/relay/leaf)
- Connection quality bars

**Web UI (`/mesh`):**
- Force-directed graph visualization (D3.js or similar)
- Nodes = devices, edges = RSSI links
- Color coding: green (strong), yellow (weak), red (lost)
- Click node for details (board type, firmware version, battery, uptime)
- Ping any peer
- Send message to peer
- Topology export (JSON)

#### 4.6 Camera App (Enhanced)
**Priority: Low**

Extend existing camera app.

**Features:**
- Live preview on display
- Capture to SD card
- Web streaming (`/stream` — MJPEG)
- Timelapse mode
- Motion detection (frame differencing)
- QR code scanning (for device pairing)

#### 4.7 Sensor Dashboard App
**Priority: Medium**

Real-time hardware sensor display.

**Shows:**
- IMU: accelerometer + gyroscope live plot
- Audio: microphone level meter + FFT spectrum
- Temperature (from PMIC or IMU)
- Battery voltage + charge current graph
- WiFi RSSI over time
- BLE device count over time

---

### Phase 5: Remote Management Suite (Web UI)

#### 5.1 Web Dashboard (Enhanced)
**Priority: Critical**

Complete redesign of the web interface with Tritium branding.

**Design language:**
- Dark theme with neon accents (cyan `#00f0ff`, magenta `#ff2a6d`, green `#05ffa1`)
- Monospace headers ("TRITIUM-OS" in top-left)
- Glowing borders on active panels
- Responsive — works on mobile browsers

**Dashboard layout:**
```
+--------------------------------------------------------------------+
|  TRITIUM-OS  v2.0.0               esp32-43c-live  12:34:56 UTC     |
+--------------------------------------------------------------------+
|                                                                     |
|  SYSTEM                    NETWORK                DISPLAY           |
|  Board: 4.3C-BOX          WiFi: TritiumLab       800x480 RGB       |
|  Uptime: 3h 42m           IP: 10.42.0.237        Verified: YES     |
|  CPU: 12%                 RSSI: -54 dBm          FPS: 8.5          |
|  Heap: 142KB free         Mesh: 4 peers          Frames: 18442     |
|  PSRAM: 7.2MB free        BLE: 12 devices                          |
|  Temp: 42C                                                         |
|                                                                     |
|  SERVICES (5/5)            QUICK ACTIONS                            |
|  [*] wifi       10.42..   [Screenshot]  [Restart]                  |
|  [*] heartbeat  60s       [OTA Upload]  [Terminal]                 |
|  [*] espnow     4 peers   [File Manager] [Settings]               |
|  [*] diag       OK        [Mesh View]   [Factory Reset]           |
|  [*] webserver  :80                                                |
|                                                                     |
+--------------------------------------------------------------------+
```

#### 5.2 Live Screenshot Viewer
**Priority: High** (already partially exists)

**Enhancements:**
- Auto-refresh toggle (1s / 5s / manual)
- Click-to-interact (touch event injection via API)
- Zoom + pan
- Screenshot download (PNG)
- Side-by-side fleet view (all devices)
- Video recording (capture sequence to GIF/WebM)

```
POST /api/touch?x=120&y=340&type=tap
POST /api/touch?x=120&y=340&dx=-100&dy=0&type=swipe
```

#### 5.3 Fleet Management View
**Priority: High**

Multi-device management from a single browser tab.

**Features:**
- Grid of all fleet devices with live status
- Bulk operations: update all, restart all, push config to all
- Device grouping (by board type, location, role)
- Firmware version matrix
- Alert dashboard (anomalies, offline devices, low battery)
- Side-by-side screenshot comparison

---

### Phase 6: Advanced Features

#### 6.1 Tritium Package Manager (TPM)
**Priority: Medium**

Install/update apps and services without reflashing.

**Concept:**
- Apps compiled as position-independent ELF modules
- Stored on SD card or LittleFS
- Loaded at runtime via dynamic linker
- Package metadata: name, version, dependencies, board compatibility
- Package repository on fleet server

**Alternative (simpler):**
- Apps as Lua/MicroPython scripts interpreted at runtime
- Core OS in C++, user apps in scripting language
- Hot-reload without reboot

#### 6.2 Voice Control
**Priority: Low**

Wake-word activation + command recognition.

**Built on existing:**
- `hal_audio` for microphone input
- `hal_voice` for VAD + MFCC feature extraction
- Keyword spotting model (TFLite Micro)

**Commands:**
- "Hey Tritium" — wake word
- "What's the battery?" — status query
- "Turn off display" — control command
- "Send message to gateway" — mesh command

#### 6.3 Automation Engine
**Priority: Low**

Simple if-then rules for autonomous behavior.

```json
{
  "rules": [
    {
      "name": "low_battery_sleep",
      "trigger": {"event": "power.battery", "condition": "< 10"},
      "action": {"type": "sleep", "duration_s": 3600}
    },
    {
      "name": "motion_alert",
      "trigger": {"event": "imu.motion"},
      "action": {"type": "mesh_broadcast", "message": "motion_detected"}
    }
  ]
}
```

#### 6.4 Secure Boot + Encrypted Flash
**Priority: Medium**

Hardware security features.

**Features:**
- Secure boot v2 (RSA-3072 signature verification)
- Flash encryption (AES-256-XTS)
- Certificate-based OTA verification
- Provisioning via QR code (key exchange)
- Tamper detection (accelerometer-based)

---

## Branding & Design Language

Styled after **valpatel.com** and **graphlings.net** — a cyberpunk terminal aesthetic that looks like a real operating system, not a toy demo.

### Identity

**Name:** Tritium-OS
**Tagline:** "Software-Defined Edge Intelligence"
**Logo:** Stylized atom with three electron orbits (tritium = 3 particles), rendered in neon cyan with breathing glow animation

### Color Palette

| Name | Hex | Usage | Glow |
|------|-----|-------|------|
| Tritium Cyan | `#00f0ff` | Primary accent, active elements, links, borders | `rgba(0, 240, 255, 0.15)` |
| Magenta | `#ff2a6d` | Alerts, errors, destructive actions | `rgba(255, 42, 109, 0.15)` |
| Neon Green | `#05ffa1` | Success, connected, healthy, operational | `rgba(5, 255, 161, 0.15)` |
| Neon Yellow | `#fcee0a` | Warnings, pending, caution states | `rgba(252, 238, 10, 0.15)` |
| Void | `#0a0a0f` | Primary background | — |
| Surface 1 | `#0e0e14` | Raised panel background | — |
| Surface 2 | `#12121a` | Card backgrounds | — |
| Surface 3 | `#1a1a2e` | Active panel, modal background | — |
| Ghost | `#8888aa` | Secondary text, inactive borders | — |
| Text | `#c8d0dc` | Primary body text | — |
| Bright | `#e0e0ff` | Headings, emphasized text | — |

### Visual Effects

**Glow system** (applied to all interactive and status elements):
- Active borders: `1px solid rgba(0, 240, 255, 0.3)` with `box-shadow: 0 0 12px rgba(0, 240, 255, 0.15)`
- Hover state: border brightens to `0.6` opacity, shadow expands to `20px`
- Status dots: colored circle with matching color `box-shadow` glow

**Scanline overlay** (CRT effect on display):
- Repeating horizontal lines at `rgba(0, 0, 0, 0.03)` — subtle, never distracting
- Interlace texture using cyan at `0.008` opacity for depth
- Applied to on-device display and web dashboard panels

**Grid background**:
- Subtle `80px` grid lines at `rgba(0, 240, 255, 0.025)` on web dashboard
- On-device: simplified to faint dot grid at panel edges

**Breathing animations**:
- Status indicators pulse with sine-wave opacity (1.5s period)
- Active connections have subtle border glow pulse
- Logo atom orbits rotate slowly (8s period)

**Transitions**:
- All state changes: `0.25s ease` (opacity, transform, border-color)
- Panel entrance: scale `0.92 -> 1.0` with fade
- Modal backdrop: `blur(8px)` with dark overlay

### Typography

**Web UI:**
- **Body:** Inter (300-700 weights) — clean sans-serif for readability
- **Code/Data:** JetBrains Mono or Fira Code — all technical values, IPs, hex, metrics
- **Headers:** UPPERCASE, letter-spacing `0.1em`, Inter 600
- **Sizing:** 7-11px labels, 10-14px body (compact, information-dense)

**On-device (LVGL):**
- **Body:** Built-in LVGL sans-serif (Montserrat)
- **Mono:** Built-in LVGL monospace for values, status, terminal
- **Headers:** UPPERCASE with `LV_LABEL_LONG_SCROLL`

### Panel Design

**Card/panel pattern** (both web and on-device):
```
+- - - - - - - - - - - - - - - - - -+
|  [ PANEL TITLE ]           STATUS  |    <- bracket notation, uppercase
|                                     |
|  Content area                       |    <- Surface 2 background
|  Monospace values aligned right     |    <- Cyan accent on values
|                                     |
+- - - - - - - - - - - - - - - - - -+
     ^                                     <- 1px cyan border at 0.08 opacity
     Hover: border glows to 0.3, shadow appears
```

**Corner brackets** on interactive panels:
```css
/* Top-left and bottom-right bracket decorations */
.panel::before { content: "[ "; }
.panel::after  { content: " ]"; }
```

### Status Indicators

```
  [*] wifi           10.42.0.237     <- green dot with green glow
  [*] heartbeat      60s interval    <- green dot
  [!] mesh           2/4 peers       <- yellow dot with yellow glow
  [x] bluetooth      disabled        <- gray dot, no glow
  [!] battery        12%             <- red dot with red glow pulse
```

### Button Styles

```
Primary:   Cyan border, transparent bg, uppercase text
           Hover: fill with rgba(0,240,255,0.1), glow, translate Y -2px

Danger:    Magenta border, transparent bg
           Hover: fill with rgba(255,42,109,0.1)

Success:   Green border
Ghost:     No border, text only, underline on hover
Disabled:  Ghost color border, 0.4 opacity
```

### Boot Sequence (On-Device)

```
[0.0s]  Void black
[0.1s]  Subtle grid pattern fades in (bg-noise layer)
[0.3s]  Tritium atom logo materializes center-screen
        Three orbital rings rotate in, electrons pulse cyan
[0.6s]  "TRITIUM-OS" renders below in monospace, letter by letter
[0.8s]  Version string fades in: "v2.0 | Universal Firmware"
[1.0s]  Logo shrinks to top-left, status lines appear:
        "Scanning hardware..." -> "ESP32-S3-Touch-LCD-4.3C-BOX [100%]"
        "Initializing display..." -> "ST7262 800x480 [VERIFIED]"
[1.5s]  Service init progress (each service appears as it inits):
        "[ wifi      ] Connecting...  TritiumLab  10.42.0.237"
        "[ heartbeat ] Active         60s interval"
        "[ mesh      ] Discovering... 4 peers found"
        "[ diag      ] Boot #29      OK"
        "[ webserver ] http://tritium-d404.local"
[2.5s]  All lines flash green simultaneously
        "SYSTEM READY" in large cyan text, 500ms hold
[3.0s]  Crossfade to home screen / last active app
```

### Web Dashboard Boot Animation

```
Page load:
  - Void background
  - Grid pattern fades in (0.3s)
  - Header bar slides down with "TRITIUM-OS" monospace text
  - Panels scale-in from 0.92 with staggered delay (50ms each)
  - Status values animate from "---" to real values
  - Connection indicator pulses green once connected via WebSocket
```

---

## Implementation Status

### Sprint 1: Foundation — COMPLETE
- [x] Settings framework (NVS-backed, change notifications) — `lib/os_settings/`
- [x] Event bus (pub/sub with category filtering) — `lib/os_events/`
- [x] Enhanced WiFi manager (AP mode, failover, 10 networks, priority) — `lib/hal_wifi/`
- [x] Service registry with priority init and tick dispatch — `include/service.h`
- [x] 16 services: WiFi, BLE, mesh, OTA, diag, settings, heartbeat, webserver, etc.

### Sprint 2: Shell — COMPLETE
- [x] LVGL 9.2 window manager (status bar + nav bar + viewport) — `lib/os_shell/`
- [x] App launcher grid with themed icons — `shell_apps.cpp`
- [x] Settings app (brightness, about screen) — `shell_apps.cpp`
- [x] Toast notification system (3 stacked, auto-dismiss) — `os_shell.cpp`
- [x] Notification shade (swipe-down gesture) — `os_shell.cpp`
- [x] Tritium cyberpunk theme (11-color palette) — `shell_theme.h`
- [x] Shell status bar wired to live service data — WiFi, BLE, mesh, battery, clock
- [x] 6 launcher apps: Map, Chat, Terminal, Files, About, Settings (11 tabs)
- [x] Lock screen with PIN entry and lockout protection — `lock_screen.cpp`

### Sprint 3: Remote Management — MOSTLY COMPLETE
- [x] Web dashboard redesign (Valpatel cyberpunk theme) — `web/dashboard.html`
- [x] WiFi manager web UI (scan, connect, save, reorder) — `web/wifi.html`
- [x] File manager web UI (browse, upload, download, delete) — `web/files.html`
- [x] OTA manager web UI (upload, progress, rollback) — `web/ota.html`
- [x] Mesh topology web UI — `web/mesh.html`
- [x] Terminal web UI — `web/terminal.html`
- [x] 35+ REST API endpoints — `hal_webserver.cpp`
- [x] Live screenshot viewer with auto-refresh — existing `/api/screenshot`
- [x] WebSocket bridge — real-time events, serial terminal, commands — `ws_bridge.cpp`
- [x] Settings REST API (GET/PUT/reset) — `hal_webserver.cpp`

### Sprint 4: Communication — PARTIALLY COMPLETE
- [ ] BLE serial port (NUS) — BLOCKED (NimBLE esp_bt.h not found)
- [x] Enhanced mesh (addressed messages, shared state, hop routing) — `mesh_manager.cpp`
- [x] Mesh protocol (discovery, topology, encrypted channels) — `mesh_protocol.h`
- [x] Terminal app (web) — `web/terminal.html`
- [x] Mesh topology visualizer (web) — `web/mesh.html`
- [x] On-device terminal app (LVGL) — `shell_apps.cpp`
- [x] On-device mesh chat app (LVGL) — `shell_apps.cpp`

### Sprint 5: Polish — IN PROGRESS
- [ ] Boot sequence animation with service progress — IN PROGRESS
- [x] Notification shade — `os_shell.cpp`
- [ ] Sensor dashboard app — TODO
- [ ] Camera streaming — TODO
- [ ] Fleet management multi-device view — TODO (handled by tritium-sc)

### Sprint 6: Advanced — NOT STARTED
- [ ] Automation rules engine
- [ ] MQTT cloud bridge
- [ ] Package system (scripted apps)
- [ ] Secure boot + encrypted flash

---

## Technical Constraints

| Constraint | Value | Impact |
|-----------|-------|--------|
| Flash | 16MB | ~3MB for OS + apps (20%), rest for data/OTA |
| PSRAM | 8MB | Framebuffers (~750KB for 800x480x2), LVGL (~200KB), app data |
| RAM | 320KB | FreeRTOS tasks, network buffers, service state |
| CPU | 240MHz dual-core | Core 0: networking/services, Core 1: display/app |
| Display | 172x640 to 800x480 | UI must scale across 6 resolutions |
| Touch | 5 controllers | Unified touch API already exists |
| WiFi | 2.4GHz only | No 5GHz, ESP-NOW shares radio |
| BLE | BLE 5.0 | NimBLE stack, ~20 connections |
| Storage | LittleFS (~1MB) + SD (up to 32GB) | Config in LittleFS, data on SD |

### Memory Budget

```
Fleet build:    1.37MB flash (20.9%), 206KB RAM (63%)
OS build:       1.55MB flash (23.6%), 210KB RAM (64%)    <- with Valpatel web pages
OS+Shell build: 1.63MB flash (24.9%), 212KB RAM (65%)    <- with LVGL shell
Budget:         6.55MB flash, 320KB RAM
Headroom:       4.9MB flash (75%), 108KB RAM (33%)
```

### Multi-Resolution UI Strategy

All LVGL layouts use **percentage-based positioning** and **breakpoints**:
- **Small** (width < 300): single column, compact status bar
- **Medium** (300-500): two columns, full status bar
- **Large** (width > 500): three columns, expanded panels

---

## File Structure (Proposed)

```
tritium-edge/
├── os/                          # Tritium-OS core (NEW)
│   ├── settings/                # Settings framework
│   ├── events/                  # Event bus
│   ├── shell/                   # Window manager, status bar, nav
│   ├── apps/                    # System app implementations
│   │   ├── launcher/            # App launcher
│   │   ├── settings/            # Settings UI
│   │   ├── files/               # File manager
│   │   ├── terminal/            # Serial terminal
│   │   ├── ota/                 # OTA manager
│   │   └── mesh_viewer/         # Mesh topology
│   └── web/                     # Web UI assets
│       ├── index.html           # Dashboard SPA
│       ├── css/tritium.css      # Theme
│       └── js/                  # Frontend modules
├── lib/                         # HALs (unchanged)
├── apps/                        # User apps (unchanged)
├── src/                         # Boot + main loop
└── include/                     # Headers
```

---

## Success Criteria

1. **Zero-touch deployment**: Flash universal firmware, plug in any board, it works
2. **Remote manageable**: Full device control from a web browser
3. **Self-healing network**: WiFi failover, mesh auto-recovery, OTA rollback
4. **Fleet awareness**: Every device knows about every other device
5. **Developer friendly**: New apps in <30 minutes, new boards in <1 hour
6. **Responsive**: Display UI at 30+ FPS, web UI updates in <500ms
7. **Robust**: Watchdog recovery, crash logging, graceful degradation
