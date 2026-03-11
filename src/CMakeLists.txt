# Tritium-Edge main ESP-IDF component.
# All src/ and lib/ sources are registered here as a single component.
# PlatformIO is told to ignore lib/ dirs (via lib_ignore in platformio.ini)
# to prevent double compilation — CMake handles everything.

file(GLOB MAIN_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

set(LIB_SRCS "")
set(LIB_INCS "")

macro(add_tritium_lib dir)
    file(GLOB _lib_srcs "${CMAKE_SOURCE_DIR}/lib/${dir}/*.cpp" "${CMAKE_SOURCE_DIR}/lib/${dir}/*.c")
    list(APPEND LIB_SRCS ${_lib_srcs})
    list(APPEND LIB_INCS "${CMAKE_SOURCE_DIR}/lib/${dir}")
endmacro()

# Core libs
add_tritium_lib(display)
add_tritium_lib(board_fingerprint)
add_tritium_lib(tritium_splash)

# App sources
file(GLOB APP_SRCS "${CMAKE_SOURCE_DIR}/apps/starfield/*.cpp")
list(APPEND LIB_SRCS ${APP_SRCS})
list(APPEND LIB_INCS "${CMAKE_SOURCE_DIR}/apps/starfield")
list(APPEND LIB_INCS "${CMAKE_SOURCE_DIR}/lib/StarField")

# OS service libs
add_tritium_lib(hal_ble_scanner)
add_tritium_lib(hal_wifi)
add_tritium_lib(hal_heartbeat)
add_tritium_lib(hal_provision)
add_tritium_lib(hal_debug)
add_tritium_lib(hal_fs)
add_tritium_lib(hal_webserver)
add_tritium_lib(hal_diag)
add_tritium_lib(hal_diaglog)
add_tritium_lib(hal_espnow)
add_tritium_lib(hal_ota)
add_tritium_lib(os_settings)
add_tritium_lib(os_events)
add_tritium_lib(hal_sdcard)
add_tritium_lib(hal_touch)
add_tritium_lib(os_shell)
add_tritium_lib(hal_ntp)
add_tritium_lib(hal_watchdog)
add_tritium_lib(hal_power)
add_tritium_lib(hal_imu)
add_tritium_lib(hal_rtc)
add_tritium_lib(hal_audio)
add_tritium_lib(hal_io_expander)
add_tritium_lib(hal_wifi_scanner)
add_tritium_lib(hal_sighting_buffer)
add_tritium_lib(hal_cot)
add_tritium_lib(hal_lora)
add_tritium_lib(hal_seed)
add_tritium_lib(hal_gis)
add_tritium_lib(hal_sleep)
add_tritium_lib(hal_camera)
add_tritium_lib(hal_mqtt)
add_tritium_lib(hal_sighting_logger)

# SQLite3 amalgamation — compiled via wrapper that force-includes config header.
# Only compile the wrapper (sqlite3_esp32.c) and the VFS shim, NOT sqlite3.c directly.
list(APPEND LIB_SRCS
    "${CMAKE_SOURCE_DIR}/lib/sqlite3/sqlite3_esp32.c"
    "${CMAKE_SOURCE_DIR}/lib/sqlite3/sqlite3_vfs_esp32.c"
)
list(APPEND LIB_INCS "${CMAKE_SOURCE_DIR}/lib/sqlite3")

# StarField lib
file(GLOB STARFIELD_SRCS "${CMAKE_SOURCE_DIR}/lib/StarField/*.cpp")
list(APPEND LIB_SRCS ${STARFIELD_SRCS})

# Display driver sub-dirs
file(GLOB DISPLAY_DRV_SRCS
    "${CMAKE_SOURCE_DIR}/lib/display/drivers/*.cpp"
    "${CMAKE_SOURCE_DIR}/lib/display/boards/*.cpp")
list(APPEND LIB_SRCS ${DISPLAY_DRV_SRCS})
list(APPEND LIB_INCS
    "${CMAKE_SOURCE_DIR}/lib/display/drivers"
    "${CMAKE_SOURCE_DIR}/lib/display/boards")

idf_component_register(
    SRCS ${MAIN_SRCS} ${LIB_SRCS}
    INCLUDE_DIRS
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/lib/display"
        "${CMAKE_SOURCE_DIR}/lib/display/drivers"
        "${CMAKE_SOURCE_DIR}/lib/display/boards"
        ${LIB_INCS}
    REQUIRES
        driver
        esp_lcd
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_http_server
        esp_psram
        fatfs
        sdmmc
        vfs
        esp_adc
        esp_driver_usb_serial_jtag
        esp_partition
        app_update
        json
        lwip
        esp_http_client
        mdns
        littlefs
        mqtt
        bt
    PRIV_REQUIRES
        spi_flash
)

# LVGL include paths
if(EXISTS "${CMAKE_SOURCE_DIR}/.pio/libdeps/${PIOENV}/lvgl")
    target_include_directories(${COMPONENT_LIB} PUBLIC
        "${CMAKE_SOURCE_DIR}/.pio/libdeps/${PIOENV}/lvgl"
        "${CMAKE_SOURCE_DIR}/.pio/libdeps/${PIOENV}/lvgl/src"
    )
endif()

# NimBLE-Arduino: only include if installed for THIS env (PIOENV).
# Previous wildcard glob pulled NimBLE from sibling envs, causing crashes.
if(EXISTS "${CMAKE_SOURCE_DIR}/.pio/libdeps/${PIOENV}/NimBLE-Arduino/src")
    set(_nimble_dir "${CMAKE_SOURCE_DIR}/.pio/libdeps/${PIOENV}/NimBLE-Arduino/src")
    message(STATUS "[tritium] NimBLE-Arduino found: ${_nimble_dir}")
    file(GLOB _nimble_srcs "${_nimble_dir}/*.cpp")
    target_sources(${COMPONENT_LIB} PRIVATE ${_nimble_srcs})
    target_include_directories(${COMPONENT_LIB} PUBLIC "${_nimble_dir}")
endif()
