"""PlatformIO pre-build script.

- Auto-adds Arduino framework library include + source paths when ENABLE_WIFI is set
  (lib_ldf_mode=off requires explicit paths for framework built-ins).
- Auto-adds LVGL include paths when ENABLE_SHELL is set.
- Removes ARM-specific assembly files from LVGL that fail on Xtensa (ESP32).
"""
import os
Import("env")

# Auto-add Arduino framework library paths for WiFi/Network/WebServer
# when ENABLE_WIFI is defined. With lib_ldf_mode=off, PlatformIO won't
# resolve these transitive dependencies automatically.
# We use BUILD_FLAGS -I instead of CPPPATH so it applies to lib builds too.
cppdefines = env.get("CPPDEFINES", [])

def has_define(name):
    # Check CPPDEFINES first
    for d in cppdefines:
        if isinstance(d, tuple) and d[0] == name:
            return True
        if d == name:
            return True
    # Also check BUILD_FLAGS (defines passed as -D flags)
    build_flags = env.get("BUILD_FLAGS", [])
    for flag in build_flags:
        if isinstance(flag, str) and (flag == f"-D{name}" or flag.startswith(f"-D{name}=")):
            return True
    return False
ldf_mode = env.GetProjectOption("lib_ldf_mode", "off")

if has_define("ENABLE_WIFI"):
    fw_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    fw_libs = os.path.join(fw_dir, "libraries")
    needed = ["Network", "WiFi", "WebServer", "ESPmDNS", "NetworkClientSecure",
              "Preferences", "SD_MMC", "FS", "SD", "SPI", "HTTPClient",
              "Update", "LittleFS", "Hash", "Ticker", "DNSServer", "AsyncUDP"]
    # Note: Wire is NOT listed here — it's handled via lib_deps to avoid double-compile

    for lib in needed:
        lib_src = os.path.join(fw_libs, lib, "src")
        if os.path.isdir(lib_src):
            # Always add include paths
            env.Append(BUILD_FLAGS=[f"-I{lib_src}"])
            # Only build sources when lib_ldf_mode=off (chain discovers them)
            if ldf_mode == "off":
                env.BuildSources(
                    os.path.join("$BUILD_DIR", "fw_lib", lib),
                    lib_src
                )

# Auto-add LVGL include + source paths when ENABLE_SHELL is set.
# With lib_ldf_mode=off, the lvgl lib_dep is installed but not auto-discovered.
libdeps_dir = os.path.join(env["PROJECT_LIBDEPS_DIR"], env["PIOENV"])
lvgl_dir = os.path.join(libdeps_dir, "lvgl")

if has_define("ENABLE_SHELL") and os.path.isdir(lvgl_dir):
    lvgl_src = os.path.join(lvgl_dir, "src")
    env.Append(BUILD_FLAGS=[f"-I{lvgl_dir}", f"-I{lvgl_src}"])
    if ldf_mode == "off":
        env.BuildSources(
            os.path.join("$BUILD_DIR", "lvgl"),
            lvgl_src
        )

# Remove ARM Helium and NEON assembly files that can't compile on ESP32

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
