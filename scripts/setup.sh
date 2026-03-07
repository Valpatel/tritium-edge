#!/usr/bin/env bash
set -euo pipefail

# ESP32 Hardware Development Environment Setup
# Tested on Ubuntu/Debian-based systems

echo "=== ESP32 Hardware Dev Environment Setup ==="

# Install system dependencies
echo "Installing system dependencies..."
sudo apt-get update && sudo apt-get install -y python3 python3-pip python3-venv clang-format libsdl2-dev

# Check for Python 3
if ! command -v python3 &> /dev/null; then
    echo "ERROR: Python 3 installation failed."
    exit 1
fi

# Install PlatformIO Core (CLI)
if ! command -v pio &> /dev/null; then
    echo "Installing PlatformIO Core..."
    curl -fsSL -o /tmp/get-platformio.py https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
    python3 /tmp/get-platformio.py
    rm /tmp/get-platformio.py

    # Add to PATH if not already there
    PIO_PATH="$HOME/.platformio/penv/bin"
    if [[ ":$PATH:" != *":$PIO_PATH:"* ]]; then
        echo "" >> "$HOME/.bashrc"
        echo "# PlatformIO" >> "$HOME/.bashrc"
        echo "export PATH=\"\$PATH:$PIO_PATH\"" >> "$HOME/.bashrc"
        export PATH="$PATH:$PIO_PATH"
    fi
    echo "PlatformIO installed."
else
    echo "PlatformIO already installed: $(pio --version)"
fi

# Install udev rules for ESP32 USB access (Linux only)
UDEV_RULES="/etc/udev/rules.d/99-esp32.rules"
if [ ! -f "$UDEV_RULES" ]; then
    echo "Installing ESP32 udev rules..."
    sudo tee "$UDEV_RULES" > /dev/null << 'RULES'
# ESP32-S3 native USB (JTAG + CDC)
SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="1001", MODE="0666", GROUP="plugdev"
# ESP32-S3 USB serial/JTAG
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", MODE="0666", GROUP="plugdev", SYMLINK+="esp32s3_%n"
# CP2102/CP2104 USB-UART bridge
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", MODE="0666", GROUP="plugdev"
# CH340/CH341 USB-UART bridge
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE="0666", GROUP="plugdev"
# FTDI USB-UART bridge
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", MODE="0666", GROUP="plugdev"
RULES
    sudo udevadm control --reload-rules
    sudo udevadm trigger
    echo "udev rules installed."
else
    echo "ESP32 udev rules already present."
fi

# Add user to dialout group for serial port access
if ! groups "$USER" | grep -qw dialout; then
    echo "Adding $USER to dialout group (re-login required)..."
    sudo usermod -aG dialout "$USER"
fi

if ! groups "$USER" | grep -qw plugdev; then
    echo "Adding $USER to plugdev group (re-login required)..."
    sudo usermod -aG plugdev "$USER"
fi

# Install ESP32 platform and required libraries
echo "Installing PlatformIO platform and libraries..."
cd "$(dirname "$0")"
pio pkg install --global -p "espressif32"

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Quick start:"
echo "  List connected boards:   ./scripts/identify.sh"
echo "  Build for 2.41-B board:  ./scripts/build.sh touch-amoled-241b"
echo "  Flash to 2.41-B board:   ./scripts/flash.sh touch-amoled-241b"
echo "  Monitor serial output:   ./scripts/monitor.sh"
echo ""
echo "Available build environments:"
echo "  touch-amoled-241b   - ESP32-S3-Touch-AMOLED-2.41-B (600x450)"
echo "  amoled-191m         - ESP32-S3-AMOLED-1.91-M (240x536)"
echo "  touch-amoled-18     - ESP32-S3-Touch-AMOLED-1.8 (368x448)"
echo "  touch-lcd-35bc      - ESP32-S3-Touch-LCD-3.5B-C (320x480)"
echo "  touch-lcd-43c-box   - ESP32-S3-Touch-LCD-4.3C-BOX (800x480)"
echo "  touch-lcd-349       - ESP32-S3-Touch-LCD-3.49 (172x640)"
echo ""
if ! groups "$USER" | grep -qw dialout; then
    echo "NOTE: Please log out and back in for serial port access."
fi
