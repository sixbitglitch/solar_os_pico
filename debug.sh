#!/usr/bin/env bash
# Wait for the PicoCalc USB CDC serial port and attach a console.
# Flash firmware first (or reset the board), then run this in another terminal:
#   ./debug.sh
set -euo pipefail

BAUD="${SOLAR_OS_SERIAL_BAUD:-115200}"
TIMEOUT_S="${SOLAR_OS_SERIAL_TIMEOUT:-90}"
PORT="${SOLAR_OS_SERIAL_PORT:-}"

usage() {
  cat <<'EOF'
Usage: ./debug.sh [--port PATH] [--baud N] [--timeout S]

Waits for the PicoCalc USB serial device to appear, then opens a console.

  --port PATH   serial device (default: newest /dev/cu.usbmodem*)
  --baud N      baud rate (default: 115200; USB CDC ignores it)
  --timeout S   seconds to wait for the port (default: 90)

Environment overrides: SOLAR_OS_SERIAL_PORT, SOLAR_OS_SERIAL_BAUD,
SOLAR_OS_SERIAL_TIMEOUT.

Tip: run ./debug.sh *before* resetting/flashing so it catches the reboot
banner (firmware waits up to 5s for USB CDC to connect).
EOF
}

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --port)
      PORT="${2:-}"
      [[ -n "$PORT" ]] || die "--port needs a path"
      shift 2
      ;;
    --baud)
      BAUD="${2:-}"
      [[ -n "$BAUD" ]] || die "--baud needs a value"
      shift 2
      ;;
    --timeout)
      TIMEOUT_S="${2:-}"
      [[ -n "$TIMEOUT_S" ]] || die "--timeout needs a value"
      shift 2
      ;;
    *)
      usage >&2
      exit 1
      ;;
  esac
done

find_port() {
  if [[ -n "$PORT" ]]; then
    if [[ -e "$PORT" ]]; then
      printf '%s\n' "$PORT"
    fi
    return
  fi
  # Newest usbmodem device; ignore Bluetooth / debug-console nodes.
  local candidates=()
  shopt -s nullglob
  candidates=(/dev/cu.usbmodem*)
  shopt -u nullglob
  if [[ "${#candidates[@]}" -eq 0 ]]; then
    return
  fi
  ls -t "${candidates[@]}" 2>/dev/null | head -n1
}

echo "Waiting up to ${TIMEOUT_S}s for PicoCalc USB serial..."
if [[ -n "$PORT" ]]; then
  echo "Looking for: $PORT"
else
  echo "Looking for: /dev/cu.usbmodem*"
fi

deadline=$((SECONDS + TIMEOUT_S))
found=""
while (( SECONDS < deadline )); do
  found="$(find_port || true)"
  if [[ -n "$found" ]]; then
    break
  fi
  sleep 0.25
done

if [[ -z "$found" ]]; then
  die "Timed out waiting for serial port. Flash/reset the Pico, or set --port."
fi

echo "Found $found"
echo "Connecting at ${BAUD}..."
echo "----"

if command -v picocom >/dev/null 2>&1; then
  echo "(picocom: Ctrl-A Ctrl-X to quit)"
  exec picocom -b "$BAUD" --imap lfcrlf "$found"
fi

if python3 -c "import serial, serial.tools.miniterm" >/dev/null 2>&1; then
  echo "(pyserial miniterm: Ctrl-] to quit)"
  exec python3 -m serial.tools.miniterm "$found" "$BAUD"
fi

if command -v screen >/dev/null 2>&1; then
  echo "(screen: Ctrl-A k then y to quit)"
  echo "Tip: brew install picocom  OR  pip install pyserial"
  exec screen "$found" "$BAUD"
fi

die "No serial client found. Install one of: brew install picocom | pip install pyserial"
