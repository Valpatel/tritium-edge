"""PlatformIO pre-build script.

Removes ARM-specific assembly files from LVGL that fail on Xtensa (ESP32).
"""
import os
Import("env")

# Remove ARM Helium and NEON assembly files that can't compile on ESP32
libdeps_dir = os.path.join(env["PROJECT_LIBDEPS_DIR"], env["PIOENV"])
lvgl_dir = os.path.join(libdeps_dir, "lvgl")

if os.path.isdir(lvgl_dir):
    for root, dirs, files in os.walk(lvgl_dir):
        for f in files:
            if f.endswith(".S"):
                path = os.path.join(root, f)
                try:
                    os.remove(path)
                    print(f"Removed ARM assembly: {path}")
                except OSError:
                    pass
