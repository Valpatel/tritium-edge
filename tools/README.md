# tools/

Development utilities for board detection and diagnostics.

## Available Tools

| Tool | Usage | Description |
|------|-------|-------------|
| `detect_boards.py` | `python3 tools/detect_boards.py [--json]` | Detects connected ESP32-S3 boards via USB. Queries udev for Espressif devices, sends `IDENTIFY` command to each, and displays board info. Supports `--json` for machine-readable output. |

## How It Works

1. Scans `/dev/ttyACM*` for Espressif USB devices via udev
2. Sends `IDENTIFY\n` to each port at 115200 baud
3. Parses JSON response from firmware: `{"board":"...","display":"...","interface":"...","app":"..."}`
4. Also checks MAC addresses against a known-boards table for boards not running our firmware

## Prerequisites

```
pip install pyserial
```

> **Note:** For quick board detection from the command line, use `./scripts/identify.sh` which wraps this tool.
