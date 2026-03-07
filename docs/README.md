# Documentation Index

Project documentation for the ESP32 multi-board display firmware.

## How the Docs Relate

```mermaid
graph TD
    GS[GETTING_STARTED.md<br>Setup &amp; First Build] --> ARCH[ARCHITECTURE.md<br>System Design]
    ARCH --> APP[ADDING_AN_APP.md<br>New App Guide]
    ARCH --> BOARD[ADDING_A_BOARD.md<br>New Board Guide]
    ARCH --> BOARDS[boards.md<br>Board Specs &amp; Pins]
```

## Documents

| Document | Description |
|----------|-------------|
| [GETTING_STARTED.md](GETTING_STARTED.md) | Environment setup, first build, first flash, troubleshooting |
| [ARCHITECTURE.md](ARCHITECTURE.md) | System design: board abstraction, app interface, build system, HALs |
| [ADDING_AN_APP.md](ADDING_AN_APP.md) | Step-by-step guide to creating a new app with app presets |
| [ADDING_A_BOARD.md](ADDING_A_BOARD.md) | How to add support for a new ESP32-S3 display board |
| [boards.md](boards.md) | Detailed specs, pin assignments, and wiki links for all 6 boards |

## Quick Links

- [Project README](../README.md)
- [CLAUDE.md Dev Notes](../CLAUDE.md)
