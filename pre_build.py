"""PlatformIO pre-build script.

- Removes ARM assembly files from LVGL (fail on Xtensa/ESP32)
- Adds Arduino framework library include paths for networking HALs
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

# Add Arduino framework library include paths when ENABLE_WIFI is defined.
# With lib_ldf_mode=off, PlatformIO won't auto-discover WiFi, Network, etc.
build_flags = env.get("BUILD_FLAGS", [])
cppdefines = env.get("CPPDEFINES", [])

# Check which services are enabled
needs_networking = False
needs_ble = False
all_flags_str = " ".join(str(f) for f in build_flags)
all_defs_str = " ".join(str(d) for d in cppdefines)
combined = all_flags_str + " " + all_defs_str

needs_networking = "ENABLE_WIFI" in combined
needs_ble = "ENABLE_BLE_SCANNER" in combined

if needs_networking:
    fw_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    if fw_dir:
        libs_dir = os.path.join(fw_dir, "libraries")
        needed_libs = [
            "WiFi", "Network", "NetworkClientSecure",
            "HTTPClient", "LittleFS", "FS", "Preferences",
            "SD_MMC", "SD", "SPI",
        ]
        for lib in needed_libs:
            src_dir = os.path.join(libs_dir, lib, "src")
            if os.path.isdir(src_dir):
                env.Append(CPPPATH=[src_dir])
                # Also compile source files from these framework libraries
                env.BuildSources(
                    os.path.join("$BUILD_DIR", "fwlib_" + lib),
                    src_dir
                )
                print(f"  + Framework lib: {lib}")
