# Porting SolarOS to the Clockwork PicoCalc (Pimoroni Pico Plus 2 W / RP2350)

Status: **in progress — compiles, never run on hardware.** See
[Hardware-validation gap](#hardware-validation-gap) before trusting anything here.

This document is the design record for a second hardware target living
*alongside* the existing ESP-IDF/ESP32-S3 firmware, not a replacement for it.
The ESP32 tree (`CMakeLists.txt` at the repo root, `src/CMakeLists.txt`,
`boards/manifests/*.toml` for ESP32 boards) is untouched in its behaviour; the
new target is a self-contained pico-sdk project under `targets/picocalc/`.

- Host board: Clockwork PicoCalc mainboard (STM32F103 keyboard/PMU
  co-processor, 320x320 ILI9488 SPI LCD, SPI micro-SD slot).
- Compute module: Pimoroni Pico Plus 2 W (RP2350B, CYW43439 radio, 16 MB
  flash).
- Toolchain: `arm-none-eabi-gcc` 13.2, CMake + Ninja, pico-sdk 2.3.1,
  FreeRTOS-Kernel V11.3.1.

---

## 1. RTOS choice: FreeRTOS-Kernel on pico-sdk

**Decision: keep FreeRTOS. Do not go bare-metal.**

The brief for this port is "minimal logic changes" for shell, jobs, streams and
ports. That requirement alone settles the question, because SolarOS's
concurrency model is not an incidental detail layered on top of portable logic —
it *is* FreeRTOS, all the way down:

| Layer | File | FreeRTOS dependency |
| --- | --- | --- |
| Task admission / placement | `src/solar_os_task.c` (514 lines), `src/solar_os_task.h` | Public API is typed in FreeRTOS primitives: `TaskFunction_t`, `TaskHandle_t`, `BaseType_t`, `UBaseType_t`, `tskNO_AFFINITY`. Callers pass FreeRTOS priorities. |
| Queues | `src/solar_os_queue.c`, `src/solar_os_queue.h` | Thin wrapper over `xQueueCreate`/`xQueueSend`/`xQueueReceive`. |
| Scheduler | `src/solar_os_scheduler.c` | FreeRTOS tasks + delays. |
| Jobs framework | `src/solar_os_jobs.c`, `src/jobs/*.c` | Every job is a FreeRTOS task; lifecycle uses task notifications (`ulTaskNotifyTake`, `xTaskNotifyGive`) and `solar_os_task_wait_done()`. |
| Drivers/services | e.g. `src/services/solar_os_cardkb.c` | Poll loops built on `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(...))` for interruptible sleeps. |

`solar_os_task.h` exposes `TaskFunction_t`/`TaskHandle_t`/`BaseType_t` in its
own signatures, so every one of the ~40 call sites in `src/jobs/` and
`src/services/` would have to change if the RTOS changed. Replacing this with a
bare-metal cooperative loop would mean rewriting the task admission accounting,
the job lifecycle protocol, the interruptible-sleep idiom, and every blocking
queue read in the shell/stream/port plumbing. That is a rewrite of the
concurrency model, which is exactly what "minimal logic changes" forbids.

Because FreeRTOS-Kernel has a first-class RP2350 port that integrates with
pico-sdk's CMake, keeping FreeRTOS costs almost nothing:

- `src/solar_os_queue.c` is portable as-is.
- `src/solar_os_task.c` needs only its PSRAM-vs-internal placement branches
  collapsed to "always internal" (see §2) — the admission bookkeeping,
  role accounting and wait protocol are pure FreeRTOS and carry over unchanged.

### 1.1 Which FreeRTOS RP2350 port, and why

FreeRTOS-Kernel V11.3.1 ships **three** candidate paths. The one selected is:

```
external/FreeRTOS-Kernel/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ
```

Reasoning:

1. `portable/ThirdParty/GCC/RP2040/` is present in the kernel tree but is
   **RP2040-only** — `grep -c RP2350 portable/ThirdParty/GCC/RP2040/port.c`
   returns 0. It is not a candidate for RP2350.
2. That same directory's `FreeRTOS_Kernel_import.cmake` is the file that tells
   you where the RP2350 ports live: when `PICO_PLATFORM` is an RP2350 variant it
   redirects to
   `portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ` (Arm) or
   `.../RP2350_RISC-V` (Hazard3). This is the pico-sdk-recommended integration
   path, asserted by the kernel's own SDK import shim rather than by us.
3. `RP2350_ARM_NTZ` is the Cortex-M33 **non-TrustZone** port, which matches how
   pico-sdk builds by default (no secure/non-secure split), and it wraps the
   generic `ARM_CM33_NTZ` port with the pico-sdk glue (SDK config adapter
   header, spinlock/`hardware_exception` integration) that the plain in-tree
   `portable/GCC/ARM_CM33_NTZ/non_secure` port lacks.

**Gotcha worth recording:** `Community-Supported-Ports` is a *nested git
submodule* of FreeRTOS-Kernel, not ordinary tree content. A plain
`git submodule update --init` of this repo will leave it empty and the RP2350
port will appear to be missing. It must be initialised recursively:

```sh
git submodule update --init --recursive
```

### 1.2 Vendoring

Both dependencies are git submodules pinned to released tags, not in-tree file
copies, so the repository stays small:

| Submodule | Path | Pinned tag |
| --- | --- | --- |
| pico-sdk | `external/pico-sdk` | `2.3.1` |
| FreeRTOS-Kernel | `external/FreeRTOS-Kernel` | `V11.3.1` |

pico-sdk 2.x is required for RP2350 support at all; 2.3.1 was the newest
release tag at the time of writing.

---

## 2. Memory / PSRAM budget check

**Finding: no PSRAM re-budget is needed for this flavour. Verified, not
assumed.**

What was checked:

- `src/solar_os_task.c` / `.h` offer three creation helpers —
  `solar_os_task_create_pinned()`, `..._pinned_internal()` and
  `..._pinned_external()` — where "external" means a PSRAM-backed stack. The
  distinction exists only to keep PSRAM stacks away from cache-disabled flash
  operations (documented in the `solar_os_task.h` comment block). On a target
  with no usable PSRAM the external variant collapses into the internal one and
  the `external_stacks_supported` flag in
  `solar_os_task_admission_status_t` reports false. This is a placement
  decision, not logic: the admission accounting and role statistics are
  unaffected.
- PSRAM-backed *static* placement via `EXT_RAM_BSS_ATTR` (e.g.
  `src/services/solar_os_cardkb.c:38`) is an ESP-IDF attribute that expands to
  nothing once the compat shim defines it empty, moving those arrays back to
  ordinary `.bss`.
- Explicit `esp_heap_caps.h` / `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` use
  does **not** appear anywhere in the file set this flavour links. It is
  confined to the Python/Lua runtimes, the BLE stack and the audio pipeline —
  all four of which are out of scope for this pass (see
  [Deferred](#deferred--stubbed--not-supported-in-this-pass)). The shim can
  therefore implement `heap_caps_*` as plain `malloc`/`free` without changing
  behaviour for anything that is actually linked.

**Honest caveat about the hardware itself:** the Pimoroni Pico Plus 2 W does in
fact carry 8 MB of PSRAM (APS6404 on a dedicated chip select), and pico-sdk 2.x
has a `hardware_psram` driver for it. This port deliberately treats the target
as PSRAM-less — nothing in the linked file set needs it, and bringing it up
would add an unvalidated dependency for no in-scope benefit. If a later pass
wants PSRAM (large framebuffers, the Python runtime), that is a new piece of
work, not a regression of this one.

---

## 3. Board manifest and code generation

<!-- Filled in as the port progresses. -->
_TBD — see §5 mapping table._

---

## 4. Compatibility shim layer

<!-- Filled in as the port progresses. -->
_TBD._

---

## 5. Mapping table: ESP-IDF driver/service → pico-sdk equivalent

<!-- Filled in as the port progresses: one row per driver/service touched,
     with file paths on both sides. -->
_TBD._

---

## 6. Hardware sources used

<!-- Filled in as the port progresses: which register maps / pin assignments
     came from which primary source, and the confidence level of each. -->
_TBD._

---

## Deferred / stubbed / not supported in this pass

<!-- Filled in as the port progresses. -->
_TBD._

---

## Hardware-validation gap

**Nothing in this port has ever been run on a PicoCalc.** There was no
PicoCalc, no Pico Plus 2 W, and no SD card available while it was written. Every
claim in this document is one of:

- **Compiles / links** — verified by actually invoking `arm-none-eabi-gcc`
  through the vendored pico-sdk CMake build. This is the strongest claim made
  anywhere in this document.
- **Sourced** — a register number, pin assignment or init sequence taken from a
  named primary source, cited in §6 with its confidence level.
- **Untested logic** — believed correct by inspection only.

No statement here should be read as "works on hardware". Timing-sensitive
behaviour (SPI clock ceilings, the 10 kHz I2C requirement for the keyboard
co-processor, SD card initialisation retry windows, display refresh rates) is
exactly the class of thing that compiles perfectly and then fails on a bench,
and none of it has been exercised.
