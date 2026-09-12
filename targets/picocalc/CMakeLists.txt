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
set(SOLAR_OS_FLAVOR_HEADER "${SOLAR_OS_GENERATED_DIR}/solar_os_flavor.h")

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

message(STATUS "SolarOS board   : ${SOLAR_OS_BOARD_NAME}")
message(STATUS "SolarOS flavour : ${SOLAR_OS_FLAVOR_NAME}")
message(STATUS "SolarOS groups  : ${SOLAR_OS_PACKAGE_GROUP_LIST}")

# --- Shared include paths ---------------------------------------------------
# compat/ MUST come before the FreeRTOS kernel include directory: the shims
# under compat/freertos/ forward to the real headers by a different relative
# path, so ordering decides which one "freertos/task.h" resolves to.
set(SOLAR_OS_INCLUDE_DIRS
    "${SOLAR_OS_SRC_DIR}/port/pico/compat"
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
add_library(solar_os_u8g2 STATIC ${SOLAR_OS_U8G2_SRCS})
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

# --- Compatibility shim -----------------------------------------------------
add_library(solar_os_compat STATIC
    "${SOLAR_OS_SRC_DIR}/port/pico/compat/solar_os_compat_pico.c"
    "${SOLAR_OS_SRC_DIR}/port/pico/compat/solar_os_compat_pico_gpio.c"
)
target_include_directories(solar_os_compat PUBLIC ${SOLAR_OS_INCLUDE_DIRS})
target_compile_options(solar_os_compat PRIVATE ${SOLAR_OS_COMPILE_OPTIONS})
target_compile_definitions(solar_os_compat PUBLIC ${SOLAR_OS_COMPILE_DEFINITIONS})
target_link_libraries(solar_os_compat PUBLIC
    pico_stdlib
    pico_rand
    hardware_gpio
    hardware_watchdog
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

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
    # Needs an RP2350 flash/partition abstraction (esp_partition + wear
    # levelling). Deferred with NVS persistence - see doc/ports/picocalc.md.
    "drivers/flash_storage.c"
    "services/solar_os_nvs_backup.c"
    # Needs a VFS layer; RP2350 has no esp_vfs equivalent yet.
    "services/solar_os_ramfs.c"
    # USB/JTAG console: pico-sdk stdio covers this differently.
    "services/solar_os_cdc.c"
)

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

# --- RTOS layer -------------------------------------------------------------
# The concurrency core, built on its own. These four files are the whole
# argument for keeping FreeRTOS (doc/ports/picocalc.md section 1), and they
# compile with ZERO source edits - every PSRAM branch in them is already
# behind #if CONFIG_SPIRAM, which sdkconfig.h defines as 0.
add_library(solar_os_rtos STATIC
    "${SOLAR_OS_SRC_DIR}/solar_os_task.c"
    "${SOLAR_OS_SRC_DIR}/solar_os_queue.c"
)
target_include_directories(solar_os_rtos PUBLIC ${SOLAR_OS_INCLUDE_DIRS})
target_compile_options(solar_os_rtos PRIVATE ${SOLAR_OS_COMPILE_OPTIONS})
target_compile_definitions(solar_os_rtos PUBLIC ${SOLAR_OS_COMPILE_DEFINITIONS})
target_link_libraries(solar_os_rtos PUBLIC solar_os_compat)

# --- Firmware ---------------------------------------------------------------
# Guarded: the shared source set still needs shims that are not written yet
# (see doc/ports/picocalc.md). Off by default so a normal build proves the
# drivers and RTOS layer; turn on to work the remaining errors.
option(SOLAR_OS_PICOCALC_FULL_FIRMWARE
       "Attempt to link the complete picocalc-core firmware" OFF)

if(SOLAR_OS_PICOCALC_FULL_FIRMWARE)
    add_executable(picocalc-core ${SOLAR_OS_CORE_SRC_PATHS})
    target_include_directories(picocalc-core PRIVATE ${SOLAR_OS_INCLUDE_DIRS})
    target_compile_options(picocalc-core PRIVATE ${SOLAR_OS_COMPILE_OPTIONS})
    target_compile_definitions(picocalc-core PRIVATE ${SOLAR_OS_COMPILE_DEFINITIONS})
    target_link_libraries(picocalc-core
        solar_os_picocalc_drivers
        solar_os_rtos
        solar_os_compat
        solar_os_u8g2
        pico_stdlib
        pico_multicore
        hardware_adc
        hardware_dma
        hardware_flash
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
    )
    pico_enable_stdio_uart(picocalc-core 1)
    pico_enable_stdio_usb(picocalc-core 1)
    pico_add_extra_outputs(picocalc-core)
endif()
