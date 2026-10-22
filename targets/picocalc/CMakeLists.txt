# SolarOS on the Clockwork PicoCalc (Pimoroni Pico Plus 2 W, RP2350B).
#
# A self-contained pico-sdk project. It deliberately does NOT extend the
# repository's ESP-IDF root CMakeLists.txt: the two build systems disagree
# about almost everything (component model, toolchain file, sdkconfig), and
# entangling them would put the ESP32 firmware at risk for no benefit. What IS
# shared is the declarative layer - the same board manifest and flavour
# generators under scripts/ produce this target's source list too.
#
# Build:
#   cmake -S targets/picocalc -B build/picocalc -G Ninja
#   cmake --build build/picocalc
#
# See doc/ports/picocalc.md for what currently compiles and what does not.

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(SOLAR_OS_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SOLAR_OS_SRC_DIR "${SOLAR_OS_PROJECT_DIR}/src")
set(SOLAR_OS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

# --- Vendored SDKs ----------------------------------------------------------
# Submodules pinned to released tags; see doc/ports/picocalc.md section 1.2.
if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "${SOLAR_OS_PROJECT_DIR}/external/pico-sdk")
endif()
if(NOT FREERTOS_KERNEL_PATH)
    set(FREERTOS_KERNEL_PATH "${SOLAR_OS_PROJECT_DIR}/external/FreeRTOS-Kernel")
endif()

if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR
        "pico-sdk not found at ${PICO_SDK_PATH}. Run:\n"
        "  git submodule update --init --recursive")
endif()

# The RP2350 Arm port lives in FreeRTOS-Kernel's Community-Supported-Ports,
# which is itself a nested submodule - hence --recursive above. A plain
# --init leaves this directory empty and the port silently appears missing.
set(FREERTOS_RP2350_PORT
    "${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ")
if(NOT EXISTS "${FREERTOS_RP2350_PORT}/CMakeLists.txt")
    message(FATAL_ERROR
        "FreeRTOS RP2350 port not found at ${FREERTOS_RP2350_PORT}.\n"
        "It is a NESTED submodule of FreeRTOS-Kernel. Run:\n"
        "  git submodule update --init --recursive")
endif()

# Target the Pimoroni Pico Plus 2 W: RP2350B (48 GPIO), CYW43439, 16 MB flash.
set(PICO_BOARD pimoroni_pico_plus2_w_rp2350 CACHE STRING "Pico board")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")

include(pico_sdk_import.cmake)

project(solar_os_picocalc C CXX ASM)

pico_sdk_init()

# Prefer PSRAM for libc malloc/calloc/realloc/free. Keep pico_malloc's
# --wrap flags, but drop its SRAM/sbrk wrapper source; SolarOS supplies
# __wrap_* via solar_os_compat_pico_malloc.c (linked from solar_os_compat).
if(TARGET pico_malloc)
    set_property(TARGET pico_malloc PROPERTY INTERFACE_SOURCES "")
endif()

# FreeRTOS must be added after pico_sdk_init(); its library.cmake defines the
# FreeRTOS-Kernel* interface targets.
add_subdirectory("${FREERTOS_RP2350_PORT}" FreeRTOS-Kernel)

# --- Declarative configuration ---------------------------------------------
# Reuse the repository's own generators rather than reimplementing them. Both
# are plain, hardware-agnostic Python; the only thing that made them
# ESP-specific was a target whitelist, which now includes rp2350. Their OUTPUT
# is consumed here instead of by the ESP-IDF build. See doc/ports/picocalc.md
# section 3.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SOLAR_OS_BOARD_MANIFEST "${SOLAR_OS_PROJECT_DIR}/boards/manifests/picocalc.toml")
set(SOLAR_OS_DRIVER_CATALOG "${SOLAR_OS_PROJECT_DIR}/boards/expansion_drivers.toml")
set(SOLAR_OS_FLAVOR_FILE "${SOLAR_OS_PROJECT_DIR}/flavors/picocalc-core.toml")

set(SOLAR_OS_BOARD_CMAKE "${SOLAR_OS_GENERATED_DIR}/solar_os_board_profile.cmake")
# Must be this exact name: include/solar_os_board.h includes it by name when
# SOLAR_OS_BOARD_GENERATED is defined.
set(SOLAR_OS_BOARD_HEADER "${SOLAR_OS_GENERATED_DIR}/solar_os_board_generated.h")
set(SOLAR_OS_FLAVOR_CMAKE "${SOLAR_OS_GENERATED_DIR}/solar_os_flavor.cmake")
# Must be this exact name: the shared sources include "solar_os_config.h".
set(SOLAR_OS_FLAVOR_HEADER "${SOLAR_OS_GENERATED_DIR}/solar_os_config.h")

file(MAKE_DIRECTORY "${SOLAR_OS_GENERATED_DIR}")

# Board profile: pins, capabilities, per-board defines, and the list of board
# driver fragments to include.
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${SOLAR_OS_PROJECT_DIR}/scripts/generate_board_profile.py"
            --manifest "${SOLAR_OS_BOARD_MANIFEST}"
            --manifest-dir "${SOLAR_OS_PROJECT_DIR}/boards/manifests"
            --drivers "${SOLAR_OS_DRIVER_CATALOG}"
            --cmake "${SOLAR_OS_BOARD_CMAKE}"
            --header "${SOLAR_OS_BOARD_HEADER}"
    RESULT_VARIABLE SOLAR_OS_BOARD_GEN_RESULT
    OUTPUT_VARIABLE SOLAR_OS_BOARD_GEN_OUTPUT
    ERROR_VARIABLE SOLAR_OS_BOARD_GEN_OUTPUT)
if(NOT SOLAR_OS_BOARD_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "board profile generation failed:\n${SOLAR_OS_BOARD_GEN_OUTPUT}")
endif()

include("${SOLAR_OS_BOARD_CMAKE}")

# Collect the board capabilities the profile just set, so the flavour
# generator can prune groups whose hardware this board does not have.
set(SOLAR_OS_BOARD_CAPABILITY_LIST "")
get_cmake_property(_solar_os_vars VARIABLES)
foreach(_var IN LISTS _solar_os_vars)
    if(_var MATCHES "^SOLAR_OS_BOARD_HAS_(.+)$" AND ${_var})
        string(TOLOWER "${CMAKE_MATCH_1}" _cap)
        list(APPEND SOLAR_OS_BOARD_CAPABILITY_LIST "${_cap}")
    endif()
endforeach()
string(REPLACE ";" "," SOLAR_OS_BOARD_CAPABILITY_ARG "${SOLAR_OS_BOARD_CAPABILITY_LIST}")
string(REPLACE ";" "," SOLAR_OS_BOARD_REQUIRED_PACKAGE_ARG "${SOLAR_OS_BOARD_REQUIRED_PACKAGES}")

# Flavour: resolves enabled groups into a concrete package and source list.
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${SOLAR_OS_PROJECT_DIR}/scripts/generate_flavor_config.py"
            --input "${SOLAR_OS_FLAVOR_FILE}"
            --target rp2350
            --layout single
            --board-capabilities "${SOLAR_OS_BOARD_CAPABILITY_ARG}"
            --board-required-packages "${SOLAR_OS_BOARD_REQUIRED_PACKAGE_ARG}"
            --cmake "${SOLAR_OS_FLAVOR_CMAKE}"
            --header "${SOLAR_OS_FLAVOR_HEADER}"
    RESULT_VARIABLE SOLAR_OS_FLAVOR_GEN_RESULT
    OUTPUT_VARIABLE SOLAR_OS_FLAVOR_GEN_OUTPUT
    ERROR_VARIABLE SOLAR_OS_FLAVOR_GEN_OUTPUT)
if(NOT SOLAR_OS_FLAVOR_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "flavour generation failed:\n${SOLAR_OS_FLAVOR_GEN_OUTPUT}")
endif()

include("${SOLAR_OS_FLAVOR_CMAKE}")

# Flavour package IDs arrive as a space-separated string; make a CMake list.
string(REPLACE " " ";" SOLAR_OS_PACKAGE_IDS "${SOLAR_OS_PACKAGE_ID_LIST}")

# Embedded manual. Same generator the ESP-IDF build uses; it turns doc/manual
# into a C header the `help`/`man` commands read.
set(SOLAR_OS_MANUAL_HEADER "${SOLAR_OS_GENERATED_DIR}/solar_os_manual_data.h")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${SOLAR_OS_PROJECT_DIR}/scripts/generate_manual.py"
            --input "${SOLAR_OS_PROJECT_DIR}/doc/manual"
            --packages "${SOLAR_OS_PROJECT_DIR}/packages/solar_os_packages.toml"
            --output "${SOLAR_OS_MANUAL_HEADER}"
    RESULT_VARIABLE SOLAR_OS_MANUAL_GEN_RESULT
    OUTPUT_VARIABLE SOLAR_OS_MANUAL_GEN_OUTPUT
    ERROR_VARIABLE SOLAR_OS_MANUAL_GEN_OUTPUT)
if(NOT SOLAR_OS_MANUAL_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "manual generation failed:\n${SOLAR_OS_MANUAL_GEN_OUTPUT}")
endif()

message(STATUS "SolarOS board   : ${SOLAR_OS_BOARD_NAME}")
message(STATUS "SolarOS flavour : ${SOLAR_OS_FLAVOR_NAME}")
message(STATUS "SolarOS groups  : ${SOLAR_OS_PACKAGE_GROUP_LIST}")

# --- Shared include paths ---------------------------------------------------
# compat/ MUST come before the FreeRTOS kernel include directory: the shims
# under compat/freertos/ forward to the real headers by a different relative
# path, so ordering decides which one "freertos/task.h" resolves to.
set(SOLAR_OS_INCLUDE_DIRS
    "${SOLAR_OS_SRC_DIR}/port/pico/compat"
    "${SOLAR_OS_SRC_DIR}/port/pico"
    "${SOLAR_OS_GENERATED_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/config"
    "${SOLAR_OS_SRC_DIR}"
    "${SOLAR_OS_SRC_DIR}/apps"
    "${SOLAR_OS_SRC_DIR}/board"
    "${SOLAR_OS_SRC_DIR}/board/pico"
    "${SOLAR_OS_SRC_DIR}/drivers"
    "${SOLAR_OS_SRC_DIR}/drivers/pico"
    "${SOLAR_OS_SRC_DIR}/jobs"
    "${SOLAR_OS_SRC_DIR}/services"
    "${SOLAR_OS_SRC_DIR}/shell"
    "${SOLAR_OS_PROJECT_DIR}/include"
    "${SOLAR_OS_PROJECT_DIR}/components/u8g2/src/clib"
    "${SOLAR_OS_PROJECT_DIR}/components/fatfs/source"
    "${SOLAR_OS_PROJECT_DIR}/components/cJSON"
    "${SOLAR_OS_PROJECT_DIR}/components/miniz"
)

# The board profile's capabilities are CMake variables; the C sources test them
# as preprocessor macros, so they are mirrored into compile definitions here.
# This is what the ESP-IDF build does through its own component machinery.
set(SOLAR_OS_COMPILE_DEFINITIONS
    SOLAR_OS_BOARD_GENERATED=1
    ${SOLAR_OS_BOARD_DEFINE}=1
)
foreach(_cap IN LISTS SOLAR_OS_BOARD_CAPABILITY_LIST)
    string(TOUPPER "${_cap}" _cap_upper)
    list(APPEND SOLAR_OS_COMPILE_DEFINITIONS "SOLAR_OS_BOARD_HAS_${_cap_upper}=1")
endforeach()

set(SOLAR_OS_COMPILE_OPTIONS
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-format
    -ffunction-sections
    -fdata-sections
)

# --- u8g2 -------------------------------------------------------------------
# Vendored as portable, hardware-agnostic C. Reused verbatim: the ILI9488
# driver supplies u8x8 byte/gpio callbacks rather than replacing the renderer.
file(GLOB SOLAR_OS_U8G2_SRCS "${SOLAR_OS_PROJECT_DIR}/components/u8g2/src/clib/*.c")
# Font data lives outside clib: the SolarOS terminal faces are generated into
# fonts/build/u8g2/, and the launcher's icon glyphs come from u8g2's own
# open-iconic set under components/u8g2/src/fonts/.
file(GLOB SOLAR_OS_FONT_SRCS
    "${SOLAR_OS_PROJECT_DIR}/fonts/build/u8g2/*.c"
    "${SOLAR_OS_PROJECT_DIR}/components/u8g2/src/fonts/*.c")
add_library(solar_os_u8g2 STATIC ${SOLAR_OS_U8G2_SRCS} ${SOLAR_OS_FONT_SRCS})
target_include_directories(solar_os_u8g2 PUBLIC
    "${SOLAR_OS_PROJECT_DIR}/components/u8g2/src/clib")
target_compile_options(solar_os_u8g2 PRIVATE -w)

# --- FatFs ------------------------------------------------------------------
# elm-chan FatFs, same filesystem ESP-IDF's fatfs component wraps, so on-disk
# layout and behaviour stay comparable across the two targets.
if(SOLAR_OS_BOARD_NEEDS_FATFS)
    add_library(solar_os_fatfs STATIC
        "${SOLAR_OS_PROJECT_DIR}/components/fatfs/source/ff.c"
        "${SOLAR_OS_PROJECT_DIR}/components/fatfs/source/ffunicode.c"
        "${SOLAR_OS_PROJECT_DIR}/components/fatfs/source/ffsystem.c"
        "${SOLAR_OS_PROJECT_DIR}/components/fatfs/port/diskio_sd_spi_pico.c"
    )
    target_include_directories(solar_os_fatfs PUBLIC
        "${SOLAR_OS_PROJECT_DIR}/components/fatfs/source"
        "${SOLAR_OS_PROJECT_DIR}/components/fatfs/port")
    target_include_directories(solar_os_fatfs PRIVATE ${SOLAR_OS_INCLUDE_DIRS})
    target_compile_definitions(solar_os_fatfs PRIVATE ${SOLAR_OS_COMPILE_DEFINITIONS})
    # FreeRTOS-Kernel is needed by ffsystem.c: FF_FS_REENTRANT is on and
    # ff_mutex_* is implemented with xSemaphoreCreateMutex.
    target_link_libraries(solar_os_fatfs PUBLIC
        pico_stdlib
        hardware_spi
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4)
endif()

# --- cJSON / miniz ----------------------------------------------------------
# Vendored verbatim (MIT). ESP-IDF pulls these in as managed components; this
# target carries them under components/ instead.
add_library(solar_os_cjson STATIC "${SOLAR_OS_PROJECT_DIR}/components/cJSON/cJSON.c")
target_include_directories(solar_os_cjson PUBLIC "${SOLAR_OS_PROJECT_DIR}/components/cJSON")
target_compile_options(solar_os_cjson PRIVATE -w)

# miniz upstream is split across four translation units; the single-file
# amalgamation is generated at release time and is not in the repository, so
# the split sources are carried instead.
add_library(solar_os_miniz STATIC
    "${SOLAR_OS_PROJECT_DIR}/components/miniz/miniz.c"
    "${SOLAR_OS_PROJECT_DIR}/components/miniz/miniz_tdef.c"
    "${SOLAR_OS_PROJECT_DIR}/components/miniz/miniz_tinfl.c"
    "${SOLAR_OS_PROJECT_DIR}/components/miniz/miniz_zip.c"
)
target_include_directories(solar_os_miniz PUBLIC "${SOLAR_OS_PROJECT_DIR}/components/miniz")
target_compile_options(solar_os_miniz PRIVATE -w)
# miniz defaults to using stdio for whole-file helpers; this target has a
# filesystem through the VFS shim, so they are left enabled.
target_compile_definitions(solar_os_miniz PUBLIC MINIZ_NO_TIME=1)

# --- Compatibility shim -----------------------------------------------------
list(APPEND SOLAR_OS_COMPAT_SRCS
    "${SOLAR_OS_SRC_DIR}/port/pico/compat/solar_os_compat_pico.c"
    "${SOLAR_OS_SRC_DIR}/port/pico/compat/solar_os_compat_pico_gpio.c"
    "${SOLAR_OS_SRC_DIR}/port/pico/compat/solar_os_compat_pico_periph.c"
    "${SOLAR_OS_SRC_DIR}/port/pico/compat/solar_os_compat_pico_vfs.c"
    "${SOLAR_OS_SRC_DIR}/port/pico/compat/solar_os_compat_pico_deferred_stubs.c"
    "${SOLAR_OS_SRC_DIR}/port/pico/compat/solar_os_compat_pico_malloc.c"
    "${SOLAR_OS_SRC_DIR}/port/pico/solar_os_psram_heap.c"
)
# Networking shims (ESP-IDF API surface on lwIP + pico_mbedtls).
foreach(_shim IN ITEMS
        solar_os_compat_pico_http_client.c
        solar_os_compat_pico_mqtt.c
        solar_os_compat_pico_httpd.c
        solar_os_compat_pico_transport.c
        solar_os_compat_pico_ping.c)
    if(EXISTS "${SOLAR_OS_SRC_DIR}/port/pico/compat/${_shim}")
        list(APPEND SOLAR_OS_COMPAT_SRCS "${SOLAR_OS_SRC_DIR}/port/pico/compat/${_shim}")
    endif()
endforeach()

add_library(solar_os_compat STATIC ${SOLAR_OS_COMPAT_SRCS})
target_include_directories(solar_os_compat PUBLIC ${SOLAR_OS_INCLUDE_DIRS})
target_compile_options(solar_os_compat PRIVATE ${SOLAR_OS_COMPILE_OPTIONS})
target_compile_definitions(solar_os_compat PUBLIC ${SOLAR_OS_COMPILE_DEFINITIONS})
target_link_libraries(solar_os_compat PUBLIC
    pico_stdlib
    pico_rand
    hardware_gpio
    hardware_watchdog
    hardware_i2c
    hardware_pwm
    hardware_clocks
    hardware_psram
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
if(SOLAR_OS_BOARD_NEEDS_FATFS)
    target_link_libraries(solar_os_compat PUBLIC solar_os_fatfs)
endif()
if("wifi" IN_LIST SOLAR_OS_BOARD_CAPABILITY_LIST)
    target_link_libraries(solar_os_compat PUBLIC pico_mbedtls)
endif()
if("wifi" IN_LIST SOLAR_OS_BOARD_CAPABILITY_LIST)
    target_link_libraries(solar_os_compat PUBLIC
        pico_cyw43_arch_lwip_sys_freertos
        pico_mbedtls
    )
    target_include_directories(solar_os_compat PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/config")
    target_compile_definitions(solar_os_compat PRIVATE
        MBEDTLS_CONFIG_FILE=\"mbedtls_config.h\")
endif()

# --- Board drivers ----------------------------------------------------------
# SOLAR_OS_BOARD_SRCS is populated by the board driver fragments under
# boards/drivers/ that the generated board profile included. Paths are
# relative to src/.
set(SOLAR_OS_BOARD_SRC_PATHS "")
foreach(_src IN LISTS SOLAR_OS_BOARD_SRCS)
    list(APPEND SOLAR_OS_BOARD_SRC_PATHS "${SOLAR_OS_SRC_DIR}/${_src}")
endforeach()
list(REMOVE_DUPLICATES SOLAR_OS_BOARD_SRC_PATHS)
list(REMOVE_DUPLICATES SOLAR_OS_BOARD_PICO_LIBS)

# This is the target that must always build: the four new PicoCalc drivers and
# their board glue, compiled against the real pico-sdk headers. Keeping it as
# its own library means driver breakage is reported separately from breakage
# in the much larger shared source set.
# The keyboard transport is a board driver in everything but name: the board
# profile routes it through the expansion package rather than a boards/drivers
# fragment, so it is added here explicitly to keep all four new drivers in one
# target.
add_library(solar_os_picocalc_drivers STATIC
    ${SOLAR_OS_BOARD_SRC_PATHS}
    "${SOLAR_OS_SRC_DIR}/drivers/pico/picocalc_keyboard.c"
)
target_include_directories(solar_os_picocalc_drivers PUBLIC ${SOLAR_OS_INCLUDE_DIRS})
target_compile_options(solar_os_picocalc_drivers PRIVATE ${SOLAR_OS_COMPILE_OPTIONS})
target_compile_definitions(solar_os_picocalc_drivers PUBLIC ${SOLAR_OS_COMPILE_DEFINITIONS})
target_link_libraries(solar_os_picocalc_drivers PUBLIC
    solar_os_compat
    solar_os_u8g2
    pico_stdlib
    ${SOLAR_OS_BOARD_PICO_LIBS}
)
if(SOLAR_OS_BOARD_NEEDS_FATFS)
    target_link_libraries(solar_os_picocalc_drivers PUBLIC solar_os_fatfs)
endif()

# --- Shared SolarOS sources -------------------------------------------------
# SOLAR_OS_PACKAGE_SRCS comes from the flavour generator. Sources that are not
# yet buildable on this target are excluded by name below rather than silently
# dropped, so the exclusion list doubles as the port's to-do list.
set(SOLAR_OS_DEFERRED_SRCS
    # ESP-IDF gptimer + LEDC PWM audio; RP2350 uses drivers/pico/audio_pwm_pico.c.
    "drivers/audio_pwm.c"
    # Needs an RP2350 flash/partition abstraction (esp_partition + wear
    # levelling). Deferred with NVS persistence - see doc/ports/picocalc.md.
    "drivers/flash_storage.c"
    "services/solar_os_nvs_backup.c"
    # Needs a VFS layer; RP2350 has no esp_vfs equivalent yet.
    "services/solar_os_ramfs.c"
    # USB/JTAG console: pico-sdk stdio covers this differently.
    "services/solar_os_cdc.c"
    # ESP-IDF Wi-Fi / L2 repeater / custom lwIP route hook. Replaced by
    # services/solar_os_wifi_pico.c (CYW43439 + pico_cyw43_arch_lwip_sys_freertos).
    "services/solar_os_wifi.c"
    "services/solar_os_wifi_repeater.c"
    "services/solar_os_lwip_route.c"
)
# main.c defines app_main() - the actual boot sequence (board bring-up,
# service init, shell/jobs start) that src/port/pico/solar_os_pico_main.c
# calls into as its entry point. It is NOT deferred: its few ESP32-only calls
# (deep sleep wake sources, the KEY button's GPIO ISR) are covered by
# compatibility shims that honestly report "unsupported" rather than
# reimplementing deep sleep or GPIO interrupts - see doc/ports/picocalc.md.

set(SOLAR_OS_CORE_SRC_PATHS "")
set(SOLAR_OS_EXCLUDED_SRCS "")
foreach(_src IN LISTS SOLAR_OS_PACKAGE_SRCS)
    list(FIND SOLAR_OS_DEFERRED_SRCS "${_src}" _deferred)
    if(_deferred GREATER -1)
        list(APPEND SOLAR_OS_EXCLUDED_SRCS "${_src}")
    elseif(EXISTS "${SOLAR_OS_SRC_DIR}/${_src}")
        list(APPEND SOLAR_OS_CORE_SRC_PATHS "${SOLAR_OS_SRC_DIR}/${_src}")
    endif()
endforeach()
list(REMOVE_DUPLICATES SOLAR_OS_CORE_SRC_PATHS)

# The board driver sources already live in solar_os_picocalc_drivers.
foreach(_path IN LISTS SOLAR_OS_BOARD_SRC_PATHS)
    list(REMOVE_ITEM SOLAR_OS_CORE_SRC_PATHS "${_path}")
endforeach()

list(LENGTH SOLAR_OS_CORE_SRC_PATHS SOLAR_OS_CORE_SRC_COUNT)
message(STATUS "SolarOS shared sources: ${SOLAR_OS_CORE_SRC_COUNT}")
message(STATUS "SolarOS deferred      : ${SOLAR_OS_EXCLUDED_SRCS}")

# Pico Wi-Fi backend replaces the deferred ESP-IDF wifi/repeater/route sources.
if("wifi" IN_LIST SOLAR_OS_BOARD_CAPABILITY_LIST)
    list(APPEND SOLAR_OS_CORE_SRC_PATHS
        "${SOLAR_OS_SRC_DIR}/services/solar_os_wifi_pico.c")
    list(REMOVE_DUPLICATES SOLAR_OS_CORE_SRC_PATHS)
endif()

# --- RTOS layer -------------------------------------------------------------
# The concurrency core, built on its own. These four files are the whole
# argument for keeping FreeRTOS (doc/ports/picocalc.md section 1), and they
# compile with ZERO source edits - every PSRAM branch in them is already
# behind #if CONFIG_SPIRAM, which sdkconfig.h defines as 1 once the Pico Plus
# 2 W APS6404 heap is live (see solar_os_psram_heap).
add_library(solar_os_rtos STATIC
    "${SOLAR_OS_SRC_DIR}/solar_os_task.c"
    "${SOLAR_OS_SRC_DIR}/solar_os_queue.c"
)
target_include_directories(solar_os_rtos PUBLIC ${SOLAR_OS_INCLUDE_DIRS})
target_compile_options(solar_os_rtos PRIVATE ${SOLAR_OS_COMPILE_OPTIONS})
target_compile_definitions(solar_os_rtos PUBLIC ${SOLAR_OS_COMPILE_DEFINITIONS})
target_link_libraries(solar_os_rtos PUBLIC solar_os_compat)

# --- Lua --------------------------------------------------------------------
set(SOLAR_OS_LUA_DIR "${SOLAR_OS_PROJECT_DIR}/components/lua/lua/src")
if(EXISTS "${SOLAR_OS_LUA_DIR}/lapi.c" AND
   "app_lua" IN_LIST SOLAR_OS_PACKAGE_IDS)
    file(GLOB SOLAR_OS_LUA_SRCS "${SOLAR_OS_LUA_DIR}/*.c")
    list(FILTER SOLAR_OS_LUA_SRCS EXCLUDE REGEX ".*/lua\\.c$")
    list(FILTER SOLAR_OS_LUA_SRCS EXCLUDE REGEX ".*/luac\\.c$")
    add_library(solar_os_lua STATIC ${SOLAR_OS_LUA_SRCS})
    target_include_directories(solar_os_lua PUBLIC "${SOLAR_OS_LUA_DIR}")
    target_compile_options(solar_os_lua PRIVATE -w)
    target_compile_definitions(solar_os_lua PUBLIC LUA_32BITS=1)
    set(SOLAR_OS_HAS_LUA 1)
else()
    set(SOLAR_OS_HAS_LUA 0)
endif()

# --- MicroPython embed ------------------------------------------------------
set(SOLAR_OS_MPY_DIR "${SOLAR_OS_PROJECT_DIR}/components/micropython_embed")
if(EXISTS "${SOLAR_OS_MPY_DIR}/micropython_embed/py/mpstate.c" AND
   "app_python" IN_LIST SOLAR_OS_PACKAGE_IDS)
    file(GLOB SOLAR_OS_MPY_SRCS
        "${SOLAR_OS_MPY_DIR}/micropython_embed/py/*.c"
        "${SOLAR_OS_MPY_DIR}/micropython_embed/extmod/*.c"
        "${SOLAR_OS_MPY_DIR}/micropython_embed/shared/runtime/*.c"
        "${SOLAR_OS_MPY_DIR}/micropython_embed/port/*.c"
    )
    list(FILTER SOLAR_OS_MPY_SRCS EXCLUDE REGEX
        ".*/(nlrthumb|nlrx86|nlrx64|nlrxtensa|nlrmips|nlraarch64|nlrrv32|nlrrv64|nlrloong64|nlrpowerpc)\\.c$")
    add_library(solar_os_micropython STATIC ${SOLAR_OS_MPY_SRCS})
    target_include_directories(solar_os_micropython PUBLIC
        "${SOLAR_OS_MPY_DIR}"
        "${SOLAR_OS_MPY_DIR}/micropython_embed"
        "${SOLAR_OS_MPY_DIR}/micropython_embed/port"
        "${SOLAR_OS_MPY_DIR}/micropython_embed/genhdr"
        "${SOLAR_OS_MPY_DIR}/micropython_embed/py"
        ${SOLAR_OS_INCLUDE_DIRS})
    target_compile_options(solar_os_micropython PRIVATE
        -Wno-error -Wno-unused-function -Wno-unused-variable
        -Wno-unused-but-set-variable -w)
    target_compile_definitions(solar_os_micropython PUBLIC
        ${SOLAR_OS_COMPILE_DEFINITIONS})
    target_link_libraries(solar_os_micropython PUBLIC solar_os_compat)
    set(SOLAR_OS_HAS_MICROPYTHON 1)
else()
    set(SOLAR_OS_HAS_MICROPYTHON 0)
endif()

# --- libssh2 (SSH/SCP) ------------------------------------------------------
# Built as a plain static library against pico_mbedtls. Upstream libssh2's own
# CMake looks for a host-installed mbedTLS and is unsuitable for cross builds.
set(LIBSSH2_ROOT "${SOLAR_OS_PROJECT_DIR}/external/libssh2")
if(EXISTS "${LIBSSH2_ROOT}/include/libssh2.h" AND
   "service_ssh" IN_LIST SOLAR_OS_PACKAGE_IDS)
    set(LIBSSH2_SRCS
        "${LIBSSH2_ROOT}/src/agent.c"
        "${LIBSSH2_ROOT}/src/bcrypt_pbkdf.c"
        "${LIBSSH2_ROOT}/src/blowfish.c"
        "${LIBSSH2_ROOT}/src/chacha.c"
        "${LIBSSH2_ROOT}/src/cipher-chachapoly.c"
        "${LIBSSH2_ROOT}/src/channel.c"
        "${LIBSSH2_ROOT}/src/comp.c"
        "${LIBSSH2_ROOT}/src/crypt.c"
        "${LIBSSH2_ROOT}/src/crypto.c"
        "${LIBSSH2_ROOT}/src/global.c"
        "${LIBSSH2_ROOT}/src/hostkey.c"
        "${LIBSSH2_ROOT}/src/keepalive.c"
        "${LIBSSH2_ROOT}/src/kex.c"
        "${LIBSSH2_ROOT}/src/knownhost.c"
        "${LIBSSH2_ROOT}/src/mac.c"
        "${LIBSSH2_ROOT}/src/misc.c"
        "${LIBSSH2_ROOT}/src/packet.c"
        "${LIBSSH2_ROOT}/src/poly1305.c"
        "${LIBSSH2_ROOT}/src/pem.c"
        "${LIBSSH2_ROOT}/src/poly1305.c"
        "${LIBSSH2_ROOT}/src/publickey.c"
        "${LIBSSH2_ROOT}/src/scp.c"
        "${LIBSSH2_ROOT}/src/session.c"
        "${LIBSSH2_ROOT}/src/sftp.c"
        "${LIBSSH2_ROOT}/src/transport.c"
        "${LIBSSH2_ROOT}/src/userauth.c"
        "${LIBSSH2_ROOT}/src/userauth_kbd_packet.c"
        "${LIBSSH2_ROOT}/src/version.c"
        "${LIBSSH2_ROOT}/src/mbedtls.c"
    )
    add_library(solar_os_libssh2 STATIC ${LIBSSH2_SRCS})
    target_include_directories(solar_os_libssh2 PUBLIC
        "${LIBSSH2_ROOT}/include"
        "${LIBSSH2_ROOT}/src"
        "${CMAKE_CURRENT_LIST_DIR}/config"
        "${SOLAR_OS_SRC_DIR}/port/pico/compat")
    target_compile_definitions(solar_os_libssh2 PRIVATE
        HAVE_CONFIG_H
        LIBSSH2_MBEDTLS=1
        HAVE_UNISTD_H=1
        HAVE_INTTYPES_H=1
        HAVE_SYS_TIME_H=1
        HAVE_SNPRINTF=1
        HAVE_GETTIMEOFDAY=1
        MBEDTLS_CONFIG_FILE=\"mbedtls_config.h\")
    target_compile_options(solar_os_libssh2 PRIVATE -w)
    # Do not link pico_cyw43_arch_* here: it is an INTERFACE library with
    # sources, and would compile lwIP into this archive (duplicate at final
    # link). Headers alone are enough for send/recv prototypes; the firmware
    # executable already links the arch library.
    target_link_libraries(solar_os_libssh2 PUBLIC pico_stdlib pico_rand)
    target_include_directories(solar_os_libssh2 PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/config"
        "${PICO_SDK_PATH}/lib/mbedtls/include"
        "${PICO_SDK_PATH}/lib/lwip/src/include"
        "${PICO_SDK_PATH}/src/rp2_common/pico_lwip/include"
        "${PICO_SDK_PATH}/lib/lwip/contrib/ports/freertos/include")
    target_compile_definitions(solar_os_libssh2 PRIVATE LWIP_PROVIDE_ERRNO=1)
    set(SOLAR_OS_HAS_LIBSSH2 1)
else()
    set(SOLAR_OS_HAS_LIBSSH2 0)
endif()

# --- Optional media codecs already vendored under components/ ---------------
if(EXISTS "${SOLAR_OS_PROJECT_DIR}/components/minimp3" AND
   "service_audio_codecs" IN_LIST SOLAR_OS_PACKAGE_IDS)
    add_library(solar_os_minimp3 STATIC
        "${SOLAR_OS_PROJECT_DIR}/components/minimp3/minimp3_impl.c")
    target_include_directories(solar_os_minimp3 PUBLIC
        "${SOLAR_OS_PROJECT_DIR}/components/minimp3/include")
    target_compile_definitions(solar_os_minimp3 PRIVATE
        MINIMP3_NO_SIMD MINIMP3_ONLY_MP3)
    target_compile_options(solar_os_minimp3 PRIVATE -w)
    set(SOLAR_OS_HAS_MINIMP3 1)
else()
    set(SOLAR_OS_HAS_MINIMP3 0)
endif()

if(EXISTS "${SOLAR_OS_PROJECT_DIR}/components/stb_image" AND
   "service_image" IN_LIST SOLAR_OS_PACKAGE_IDS)
    add_library(solar_os_stb_image STATIC
        "${SOLAR_OS_PROJECT_DIR}/components/stb_image/stb_image_port.c")
    target_include_directories(solar_os_stb_image PUBLIC
        "${SOLAR_OS_PROJECT_DIR}/components/stb_image/include")
    target_compile_options(solar_os_stb_image PRIVATE -w)
    target_link_libraries(solar_os_stb_image PUBLIC solar_os_compat)
    set(SOLAR_OS_HAS_STB_IMAGE 1)
else()
    set(SOLAR_OS_HAS_STB_IMAGE 0)
endif()

if(EXISTS "${SOLAR_OS_PROJECT_DIR}/components/webp_decoder" AND
   "service_image" IN_LIST SOLAR_OS_PACKAGE_IDS)
    set(SOLAR_OS_WEBP_DIR "${SOLAR_OS_PROJECT_DIR}/components/webp_decoder")
    add_library(solar_os_webp STATIC
        "${SOLAR_OS_WEBP_DIR}/solar_os_webp_decoder.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dec/alpha_dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dec/buffer_dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dec/frame_dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dec/idec_dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dec/io_dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dec/quant_dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dec/tree_dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dec/vp8_dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dec/vp8l_dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dec/webp_dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dsp/alpha_processing.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dsp/cpu.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dsp/dec.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dsp/dec_clip_tables.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dsp/filters.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dsp/lossless.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dsp/rescaler.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dsp/upsampling.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/dsp/yuv.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/utils/bit_reader_utils.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/utils/color_cache_utils.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/utils/filters_utils.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/utils/huffman_utils.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/utils/palette.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/utils/quant_levels_dec_utils.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/utils/rescaler_utils.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/utils/random_utils.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/utils/thread_utils.c"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src/utils/utils.c"
    )
    target_include_directories(solar_os_webp PUBLIC
        "${SOLAR_OS_WEBP_DIR}/include"
        "${SOLAR_OS_WEBP_DIR}/libwebp"
        "${SOLAR_OS_WEBP_DIR}/libwebp/src")
    target_include_directories(solar_os_webp PRIVATE ${SOLAR_OS_INCLUDE_DIRS})
    target_compile_definitions(solar_os_webp PRIVATE
        WEBP_NO_THREAD ${SOLAR_OS_COMPILE_DEFINITIONS})
    target_compile_options(solar_os_webp PRIVATE -w)
    target_link_libraries(solar_os_webp PUBLIC solar_os_compat)
    set(SOLAR_OS_HAS_WEBP 1)
else()
    set(SOLAR_OS_HAS_WEBP 0)
endif()

# --- Firmware ---------------------------------------------------------------
# Guarded: the shared source set still needs shims that are not written yet
# (see doc/ports/picocalc.md). Off by default so a normal build proves the
# drivers and RTOS layer; turn on to work the remaining errors.
option(SOLAR_OS_PICOCALC_FULL_FIRMWARE
       "Attempt to link the complete picocalc-core firmware" OFF)

if(SOLAR_OS_PICOCALC_FULL_FIRMWARE)
    # solar_os_pico_main.c supplies main() and bridges to ESP-IDF's app_main().
    add_executable(picocalc-core
        ${SOLAR_OS_CORE_SRC_PATHS}
        "${SOLAR_OS_SRC_DIR}/port/pico/solar_os_pico_main.c"
    )
    target_include_directories(picocalc-core PRIVATE ${SOLAR_OS_INCLUDE_DIRS})
    target_compile_options(picocalc-core PRIVATE ${SOLAR_OS_COMPILE_OPTIONS})
    target_compile_definitions(picocalc-core PRIVATE ${SOLAR_OS_COMPILE_DEFINITIONS})
    target_link_libraries(picocalc-core
        solar_os_picocalc_drivers
        solar_os_rtos
        solar_os_compat
        solar_os_u8g2
        solar_os_cjson
        solar_os_miniz
        pico_stdlib
        pico_multicore
        hardware_adc
        hardware_dma
        hardware_flash
        hardware_pwm
        hardware_psram
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
    )
    if("wifi" IN_LIST SOLAR_OS_BOARD_CAPABILITY_LIST)
        target_link_libraries(picocalc-core
            pico_cyw43_arch_lwip_sys_freertos
            pico_mbedtls
        )
        # pico_lwip looks for lwipopts.h on the target include path.
        target_include_directories(picocalc-core PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/config")
    endif()
    if(SOLAR_OS_HAS_LUA)
        target_link_libraries(picocalc-core solar_os_lua)
    endif()
    if(SOLAR_OS_HAS_MICROPYTHON)
        target_link_libraries(picocalc-core solar_os_micropython)
    endif()
    if(SOLAR_OS_HAS_LIBSSH2)
        target_link_libraries(picocalc-core solar_os_libssh2)
    endif()
    if(SOLAR_OS_HAS_MINIMP3)
        target_link_libraries(picocalc-core solar_os_minimp3)
    endif()
    if(SOLAR_OS_HAS_STB_IMAGE)
        target_link_libraries(picocalc-core solar_os_stb_image)
    endif()
    if(SOLAR_OS_HAS_WEBP)
        target_link_libraries(picocalc-core solar_os_webp)
    endif()
    pico_enable_stdio_uart(picocalc-core 1)
    pico_enable_stdio_usb(picocalc-core 1)
    pico_add_extra_outputs(picocalc-core)
endif()
