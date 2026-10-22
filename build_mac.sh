#!/usr/bin/env bash
# Build SolarOS PicoCalc (RP2350) firmware on macOS and copy images into dist/.
# See doc/ports/picocalc.md and targets/picocalc/CMakeLists.txt.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

DIST="$ROOT/dist"
BUILD_DIR="$ROOT/build/picocalc"
SOURCE_DIR="$ROOT/targets/picocalc"
FULL_FIRMWARE=1

usage() {
  cat <<'EOF'
Usage: ./build_mac.sh [--drivers-only] [--reconfigure]

Builds the PicoCalc pico-sdk target with CMake + Ninja.

  (no args)         configure with SOLAR_OS_PICOCALC_FULL_FIRMWARE=ON and
                    build the picocalc-core firmware image
  --drivers-only    build drivers + RTOS libraries only (default cmake mode
                    in doc/ports/picocalc.md; no uf2)
  --reconfigure     wipe build/picocalc and re-run cmake before building

Requires ./setup_mac.sh first (cmake, ninja, Arm GNU Toolchain, submodules).

Each successful full-firmware build writes:

  dist/picocalc-core.uf2         flashable UF2 (primary)
  dist/picocalc-core.bin         raw binary
  dist/picocalc-core.elf         ELF with symbols
  dist/firmware.uf2              convenience copy of the UF2
  dist/version.txt               firmware version
EOF
}

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1. Run ./setup_mac.sh first."
}

find_arm_gcc() {
  local candidate dir
  shopt -s nullglob
  for dir in /Applications/ArmGNUToolchain/*/arm-none-eabi/bin; do
    candidate="$dir/arm-none-eabi-gcc"
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      shopt -u nullglob
      return
    fi
  done
  shopt -u nullglob

  if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    command -v arm-none-eabi-gcc
  fi
}

toolchain_has_newlib() {
  local gcc="$1"
  local prefix include
  prefix="$("$gcc" -print-sysroot 2>/dev/null || true)"
  if [[ -n "$prefix" && -f "$prefix/include/stdint.h" ]]; then
    return 0
  fi
  include="$(cd "$(dirname "$gcc")/../arm-none-eabi/include" 2>/dev/null && pwd || true)"
  [[ -n "$include" && -f "$include/stdint.h" ]]
}

copy_firmware() {
  local stem="$BUILD_DIR/picocalc-core"
  local ext
  if [[ ! -f "${stem}.uf2" ]]; then
    die "Build succeeded but firmware is missing: ${stem}.uf2"
  fi
  mkdir -p "$DIST"
  for ext in uf2 bin elf; do
    if [[ -f "${stem}.${ext}" ]]; then
      cp -f "${stem}.${ext}" "$DIST/picocalc-core.${ext}"
      echo "Copied ${stem}.${ext} -> dist/picocalc-core.${ext}"
    fi
  done
  cp -f "${stem}.uf2" "$DIST/firmware.uf2"
  cp -f "$ROOT/version.txt" "$DIST/version.txt"
  echo "Copied ${stem}.uf2 -> dist/firmware.uf2"
}

reconfigure=0
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --drivers-only)
      FULL_FIRMWARE=0
      shift
      ;;
    --reconfigure)
      reconfigure=1
      shift
      ;;
    *)
      usage >&2
      exit 1
      ;;
  esac
done

need_cmd cmake
need_cmd ninja
need_cmd python3

ARM_GCC="$(find_arm_gcc || true)"
if [[ -z "${ARM_GCC}" ]]; then
  die "arm-none-eabi-gcc not found. Run ./setup_mac.sh first."
fi
if ! toolchain_has_newlib "$ARM_GCC"; then
  die "Toolchain at ${ARM_GCC} has no newlib headers. Run ./setup_mac.sh (installs the gcc-arm-embedded cask, not Homebrew's arm-none-eabi-gcc formula)."
fi
export PATH="$(dirname "$ARM_GCC"):$PATH"
echo "Using $($ARM_GCC --version | head -n1)"

if [[ ! -f "$ROOT/external/pico-sdk/pico_sdk_init.cmake" ]]; then
  die "pico-sdk not found. Run ./setup_mac.sh (or: git submodule update --init --recursive)."
fi
if [[ ! -f "$ROOT/external/FreeRTOS-Kernel/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/CMakeLists.txt" ]]; then
  die "FreeRTOS RP2350 port not found. Run: git submodule update --init --recursive"
fi

if [[ "$reconfigure" -eq 1 ]]; then
  rm -rf "$BUILD_DIR"
fi

cmake_args=(-S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja)
if [[ "$FULL_FIRMWARE" -eq 1 ]]; then
  cmake_args+=(-DSOLAR_OS_PICOCALC_FULL_FIRMWARE=ON)
else
  cmake_args+=(-DSOLAR_OS_PICOCALC_FULL_FIRMWARE=OFF)
fi

if [[ ! -f "$BUILD_DIR/build.ninja" || "$reconfigure" -eq 1 ]]; then
  echo "Configuring PicoCalc build in $BUILD_DIR..."
  cmake "${cmake_args[@]}"
elif [[ "$FULL_FIRMWARE" -eq 1 ]]; then
  cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -DSOLAR_OS_PICOCALC_FULL_FIRMWARE=ON
elif [[ "$FULL_FIRMWARE" -eq 0 ]]; then
  cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -DSOLAR_OS_PICOCALC_FULL_FIRMWARE=OFF
fi

rm -rf "$DIST"
mkdir -p "$DIST"

if [[ "$FULL_FIRMWARE" -eq 1 ]]; then
  echo "Building picocalc-core..."
  cmake --build "$BUILD_DIR" --target picocalc-core
  copy_firmware
else
  echo "Building PicoCalc drivers + RTOS layer..."
  cmake --build "$BUILD_DIR"
  echo "Drivers/RTOS build complete (no firmware image; omit --drivers-only for uf2)."
fi
