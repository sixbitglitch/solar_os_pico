#!/usr/bin/env bash
# Install the macOS host tools needed to build the PicoCalc (RP2350) target
# with ./build_mac.sh. See doc/ports/picocalc.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

BREW_PACKAGES=(cmake ninja python)

usage() {
  cat <<'EOF'
Usage: ./setup_mac.sh

Prepares this Mac to build the SolarOS PicoCalc (pico-sdk) firmware:

  - Xcode Command Line Tools
  - Homebrew packages: cmake, ninja, python
  - Arm GNU Toolchain (gcc-arm-embedded cask; includes newlib)
  - git submodules (pico-sdk, FreeRTOS-Kernel, nested ports) via
    git submodule update --init --recursive

Then run ./build_mac.sh.

Note: Homebrew's arm-none-eabi-gcc formula is NOT enough — it lacks the
newlib headers pico-sdk needs. This script installs the official Arm
toolchain cask instead.
EOF
}

log() {
  printf '%s\n' "$*"
}

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

# Prefer the Arm GNU Toolchain (newlib) over Homebrew's compiler-only formula.
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
  # Arm GNU Toolchain layout: <root>/bin/arm-none-eabi-gcc,
  # headers at <root>/arm-none-eabi/include/stdint.h
  include="$(cd "$(dirname "$gcc")/../arm-none-eabi/include" 2>/dev/null && pwd || true)"
  [[ -n "$include" && -f "$include/stdint.h" ]]
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ "$#" -ne 0 ]]; then
  usage >&2
  exit 1
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
  die "setup_mac.sh is for macOS."
fi
if [[ "$(id -u)" -eq 0 ]]; then
  die "Do not run setup_mac.sh as root."
fi

if ! xcode-select -p >/dev/null 2>&1; then
  log "Installing Xcode Command Line Tools..."
  xcode-select --install
  die "Finish the Command Line Tools installer, then re-run ./setup_mac.sh."
fi

if ! command -v brew >/dev/null 2>&1; then
  die "Homebrew is required. Install it from https://brew.sh, then re-run ./setup_mac.sh."
fi

log "Installing Homebrew packages: ${BREW_PACKAGES[*]}"
brew install "${BREW_PACKAGES[@]}"

ARM_GCC="$(find_arm_gcc || true)"
if [[ -z "${ARM_GCC}" ]] || ! toolchain_has_newlib "$ARM_GCC"; then
  log "Installing Arm GNU Toolchain (gcc-arm-embedded cask)..."
  brew install --cask gcc-arm-embedded
  ARM_GCC="$(find_arm_gcc || true)"
fi
if [[ -z "${ARM_GCC}" ]]; then
  die "arm-none-eabi-gcc not found after installing gcc-arm-embedded."
fi
if ! toolchain_has_newlib "$ARM_GCC"; then
  die "Toolchain at ${ARM_GCC} has no newlib headers (stdint.h). Install the gcc-arm-embedded cask, not the arm-none-eabi-gcc formula."
fi

# Ensure cmake/ninja see this toolchain first (Homebrew formula may also be on PATH).
export PATH="$(dirname "$ARM_GCC"):$PATH"

need_cmd cmake
need_cmd ninja
need_cmd python3
need_cmd git

log "Using cmake $(cmake --version | head -n1 | awk '{print $3}')"
log "Using ninja $(ninja --version)"
log "Using $(python3 -c 'import sys; print(sys.executable + " (" + sys.version.split()[0] + ")")')"
log "Using $($ARM_GCC --version | head -n1)"
log "  at $ARM_GCC"

log "Initialising git submodules (recursive; required for FreeRTOS RP2350 port)..."
git submodule update --init --recursive

if [[ ! -f "$ROOT/external/pico-sdk/pico_sdk_init.cmake" ]]; then
  die "pico-sdk missing after submodule init at external/pico-sdk."
fi
if [[ ! -f "$ROOT/external/FreeRTOS-Kernel/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/CMakeLists.txt" ]]; then
  die "FreeRTOS RP2350 port missing. Re-run: git submodule update --init --recursive"
fi

log
log "PicoCalc build environment is ready."
log "Build firmware with: ./build_mac.sh"
log "Drivers/RTOS only:   ./build_mac.sh --drivers-only"
log "Serial console:      ./debug.sh   (wait for USB, then attach)"
if ! command -v picocom >/dev/null 2>&1; then
  log "Optional: brew install picocom   # nicer serial console for ./debug.sh"
fi
if [[ "$(command -v arm-none-eabi-gcc)" != "$ARM_GCC" ]]; then
  log "Tip: put $(dirname "$ARM_GCC") ahead of Homebrew on PATH so shells pick the Arm toolchain."
fi
