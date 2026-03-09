# Getting Started

Step-by-step guide for setting up the development environment from scratch on Ubuntu/Debian.

## 1. Clone the Repository

```bash
git clone <repo-url>
cd tritium-edge
```

## 2. Run the Setup Script

```bash
./scripts/setup.sh
```

This installs:
- Python 3, pip, venv, clang-format (system packages)
- PlatformIO Core CLI (if not already installed)
- ESP32 udev rules for USB access
- Adds your user to `dialout` and `plugdev` groups

After setup completes, **log out and back in** if group changes were made (the script will tell you).

### Manual Setup (if you prefer)

Install PlatformIO Core following the [official instructions](https://docs.platformio.org/en/latest/core/installation.html), then:

```bash
# USB permissions (Linux)
sudo usermod -aG dialout $USER
sudo usermod -aG plugdev $USER
# Log out and back in
```

## 3. Connect Your Board

Plug your Waveshare ESP32-S3 board in via USB-C. Verify it's detected:

```bash
pio device list
```

You should see a device like `/dev/ttyACM0` (native USB) or `/dev/ttyUSB0` (UART bridge).

## 4. Build

Build firmware for your board (default app is starfield):

```bash
./scripts/build.sh touch-amoled-241b
# or: make build BOARD=touch-amoled-241b
```

Build a specific app:

```bash
./scripts/build.sh touch-lcd-35bc camera
# or: make build BOARD=touch-lcd-35bc APP=camera
```

Run `make list-boards` to see all available board environments.

## 5. Flash

```bash
./scripts/flash.sh touch-amoled-241b
# or: make flash BOARD=touch-amoled-241b
```

The scripts auto-detect the serial port and fix permissions if needed.

The ESP32-S3 boards use native USB, so you may need to hold the BOOT button and press RESET to enter download mode on first flash. After that, the firmware enables CDC-on-boot and subsequent flashes work automatically.

## 6. Monitor Serial Output

```bash
./scripts/monitor.sh
# or: make monitor
```

Or combine flash and monitor in one step:

```bash
./scripts/flash-monitor.sh touch-amoled-241b
# or: make flash-monitor BOARD=touch-amoled-241b
```

You should see output like:

```
Board: RM690B0
Display: 600x450 via QSPI
Display ready: 600x450
App: Starfield
Starfield: 600 stars
Starting animation...
FPS: 28.3
```

Press `Ctrl+]` to exit the monitor.

## 7. Next Steps

- **Try a different app**: See [ADDING_AN_APP.md](ADDING_AN_APP.md)
- **Add a new board**: See [ADDING_A_BOARD.md](ADDING_A_BOARD.md)
- **Understand the architecture**: See [ARCHITECTURE.md](ARCHITECTURE.md)
- **Board specs and pinouts**: See [boards.md](boards.md)

## Troubleshooting

### "Permission denied" on /dev/ttyACM0

Your user isn't in the `dialout` group, or you haven't logged out since adding it:

```bash
sudo usermod -aG dialout $USER
# Log out and back in
```

### "No device found" during flash

1. Check `pio device list` -- is the board visible?
2. Try holding BOOT, pressing RESET, then releasing BOOT to enter download mode
3. Try a different USB cable (some cables are charge-only)
4. Check that udev rules are installed: `ls /etc/udev/rules.d/99-esp32.rules`

### Build fails with "No board selected"

You're missing the `-DBOARD_*` flag. Make sure you're building with a valid environment:

```bash
make build BOARD=touch-amoled-241b
# or
pio run -e touch-amoled-241b
```

### Display doesn't turn on

- **AXS15231B boards** (`touch-lcd-35bc`, `touch-lcd-349`): These need the full register init sequence (~500 bytes of panel configuration). The custom `Panel_AXS15231B` driver in `lib/Panel_AXS15231B/` handles this.
- **3.5B-C specifically**: Requires TCA9554 I/O expander reset before `display.init()`. This is handled automatically in `main.cpp` when `BOARD_TOUCH_LCD_35BC` is defined.
- **Manual backlight**: If the display inits but stays dark, check that the backlight pin (`LCD_BL`) is driven HIGH. The firmware does this explicitly in addition to LovyanGFX's `Light_PWM`.
- **Pins not verified**: Some boards have pin assignments from datasheets that haven't been verified on hardware. Check the "HW Verified" column in [boards.md](boards.md).
- Check serial output for error messages about sprite allocation.

### Sprite allocation fails

The firmware tries to allocate a full-screen sprite in PSRAM. If it fails, it falls back to half-height, then direct rendering. Check serial output for warnings. This usually indicates a PSRAM configuration issue -- verify `board_build.arduino.memory_type = qio_opi` is set in `platformio.ini`.

### Serial monitor shows garbage

Make sure the baud rate matches: `make monitor` defaults to 115200. If using `pio device monitor` directly, pass `-b 115200`.
