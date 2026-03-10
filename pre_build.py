"""PlatformIO pre-build script for ESP-IDF framework.

- Auto-adds LVGL include paths when ENABLE_SHELL is set.
- Removes ARM-specific assembly files from LVGL that fail on Xtensa (ESP32).
"""
import os
Import("env")

cppdefines = env.get("CPPDEFINES", [])

def has_define(name):
    for d in cppdefines:
        if isinstance(d, tuple) and d[0] == name:
            return True
        if d == name:
            return True
    build_flags = env.get("BUILD_FLAGS", [])
    for flag in build_flags:
        if isinstance(flag, str) and (flag == f"-D{name}" or flag.startswith(f"-D{name}=")):
            return True
    return False

ldf_mode = env.GetProjectOption("lib_ldf_mode", "off")

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

# Replace ARM Helium and NEON assembly files with empty stubs.
# Can't delete them because CMake build files already reference them.
if os.path.isdir(lvgl_dir):
    for root, dirs, files in os.walk(lvgl_dir):
        for f in files:
            if f.endswith(".S"):
                path = os.path.join(root, f)
                try:
                    with open(path, 'w') as stub:
                        stub.write("/* ARM assembly removed — not compatible with Xtensa (ESP32) */\n")
                    print(f"Stubbed ARM assembly: {path}")
                except OSError:
                    pass
