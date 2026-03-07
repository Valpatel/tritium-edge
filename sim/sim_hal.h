#pragma once

// Mock HAL stubs for desktop simulator.
// Provides Arduino-compatible functions using SDL2.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <lgfx/v1/platforms/sdl/common.hpp>

// Arduino-compatible time functions (provided by LovyanGFX SDL backend)
using lgfx::millis;
using lgfx::delay;

// Serial mock
struct SerialMock {
    void begin(unsigned long) {}
    void println(const char* s) { printf("%s\n", s); }
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
};
inline SerialMock Serial;

// Board defines (set via build flags, provide defaults for generic sim)
#ifndef DISPLAY_DRIVER
#define DISPLAY_DRIVER "Simulator"
#endif
#ifndef DISPLAY_IF
#define DISPLAY_IF "SDL"
#endif
#ifndef DISPLAY_BPP
#define DISPLAY_BPP 16
#endif

// TFT color constants
#ifndef TFT_BLACK
#define TFT_BLACK 0x0000
#endif
#ifndef TFT_WHITE
#define TFT_WHITE 0xFFFF
#endif
