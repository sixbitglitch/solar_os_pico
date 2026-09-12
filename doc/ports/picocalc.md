# Porting SolarOS to the Clockwork PicoCalc (Pimoroni Pico Plus 2 W / RP2350)

Status: **compiles; never run on hardware.** Read
[Hardware-validation gap](#hardware-validation-gap) before trusting anything
here. Current build state is in [§7](#7-build-status).

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

**Build:**

```sh
git submodule update --init --recursive      # --recursive is required, see §1.1
cmake -S targets/picocalc -B build/picocalc -G Ninja
cmake --build build/picocalc                 # drivers + RTOS layer
```

Add `-DSOLAR_OS_PICOCALC_FULL_FIRMWARE=ON` to attempt the complete firmware.

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

**This prediction held exactly.** `src/solar_os_task.c` and
`src/solar_os_queue.c` compile for RP2350 with **zero source edits** — see
[§4.2](#42-edits-to-pre-existing-files).

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
   pico-sdk builds by default, and it wraps the generic `ARM_CM33_NTZ` port with
   the pico-sdk glue (SDK config adapter header, spinlock/`hardware_exception`
   integration) that the plain in-tree `portable/GCC/ARM_CM33_NTZ/non_secure`
   port lacks.

**Gotcha worth recording:** `Community-Supported-Ports` is a *nested git
submodule* of FreeRTOS-Kernel, not ordinary tree content. A plain
`git submodule update --init` leaves it empty and the RP2350 port appears to be
missing. It must be initialised recursively. `targets/picocalc/CMakeLists.txt`
checks for this explicitly and fails with that instruction rather than with a
confusing downstream error.

### 1.2 Vendoring

Dependencies are git submodules pinned to released tags, not in-tree file
copies, so the repository stays small:

| Submodule | Path | Pinned tag |
| --- | --- | --- |
| pico-sdk | `external/pico-sdk` | `2.3.1` |
| FreeRTOS-Kernel | `external/FreeRTOS-Kernel` | `V11.3.1` |

pico-sdk 2.x is required for RP2350 support at all; 2.3.1 was the newest
release tag at the time of writing.

Three third-party libraries that ESP-IDF pulls in as *managed components* have
no such mechanism here, so they are vendored under `components/` as files (all
permissively licensed, licence text included):

| Library | Path | Version | Licence |
| --- | --- | --- | --- |
| elm-chan FatFs | `components/fatfs/` | R0.16 patch 2 | BSD-1-clause-like (ChaN) |
| cJSON | `components/cJSON/` | 1.7.19 | MIT |
| miniz | `components/miniz/` | 11.3.2 | MIT |

FatFs matters for compatibility, not just convenience: it is the *same*
filesystem ESP-IDF's `fatfs` component wraps, so a card written by the ESP32
firmware and a card written here have the same on-disk layout and semantics.

### 1.3 FreeRTOS configuration notes

`targets/picocalc/config/FreeRTOSConfig.h` is based on pico-sdk's own RP2350
reference configuration. The settings that are *not* defaults:

| Setting | Value | Why |
| --- | --- | --- |
| `configNUMBER_OF_CORES` | 2 | RP2350 is dual-core and the jobs framework expects real concurrency. This is also what makes the core-pinning shim's affinity argument meaningful rather than ignored. |
| `configUSE_CORE_AFFINITY` | 1 | Required for `vTaskCoreAffinitySet`, which `xTaskCreatePinnedToCore` maps onto. |
| `configTOTAL_HEAP_SIZE` | 256 KB | Of RP2350's 520 KB SRAM. **A starting point, not a measured budget** — see [§7.3](#73-numbers-that-are-guesses). |
| `configENABLE_FPU` | 1 | The RP2350_ARM_NTZ port refuses to compile without an explicit choice. |
| `configMAX_SYSCALL_INTERRUPT_PRIORITY` | 16 | Matches pico-sdk's reference config; RP2350's M33 has 4 priority bits, so this leaves the top level for handlers that never call kernel APIs. |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | On during bring-up: this port has never run, so failing loudly beats corrupting memory. |
| `configUSE_MALLOC_FAILED_HOOK` | 1 | Same reasoning. |
| `configSUPPORT_PICO_SYNC_INTEROP` | 1 | The SD and display drivers call pico-sdk sleep helpers from tasks; this makes those block the task rather than the core. |

---

## 2. Memory / PSRAM budget check

**Finding: no PSRAM re-budget is needed for this flavour. Verified, not
assumed.**

What was checked:

- `src/solar_os_task.c` / `.h` offer three creation helpers —
  `solar_os_task_create_pinned()`, `..._pinned_internal()` and
  `..._pinned_external()` — where "external" means a PSRAM-backed stack. The
  distinction exists only to keep PSRAM stacks away from cache-disabled flash
  operations (documented in the `solar_os_task.h` comment block).
- Crucially, **every PSRAM path in those two files is already behind
  `#if CONFIG_SPIRAM`** (via `SOLAR_OS_FREERTOS_EXTERNAL_MEMORY`). With the
  shim's `sdkconfig.h` defining `CONFIG_SPIRAM 0`,
  `solar_os_task_create_pinned_external()` tail-calls the internal variant and
  `solar_os_queue_create()` calls plain `xQueueCreate`. The admission
  accounting, role statistics and wait protocol carry over untouched. Both
  files compile **unmodified**.
- `MALLOC_CAP_SPIRAM` does appear in five linked files (`main.c`,
  `solar_os_queue.c`, `solar_os_task.c`, `services/solar_os_memory.c`,
  `shell/solar_os_shell_system.c`), but only in two roles: inside those
  `#if CONFIG_SPIRAM` blocks, and in `heap_caps_get_free_size()` /
  `heap_caps_get_total_size()` *reporting* calls. The shim answers 0 free and
  0 total for that capability, which is the truthful answer for a target that
  does not bring its PSRAM up. No linked file ever *allocates* with it.
- PSRAM-backed static placement via `EXT_RAM_BSS_ATTR` (7 uses, e.g.
  `src/services/solar_os_cardkb.c:38`) is an ESP-IDF attribute the shim defines
  empty, moving those arrays back to ordinary `.bss`.

**Honest caveat about the hardware itself:** the Pimoroni Pico Plus 2 W does in
fact carry 8 MB of PSRAM (APS6404 on GPIO47), and pico-sdk 2.x has a
`hardware_psram` driver for it. This port deliberately treats the target as
PSRAM-less — nothing in the linked file set needs it, and bringing it up would
add an unvalidated dependency for no in-scope benefit. If a later pass wants
PSRAM (large framebuffers, the Python runtime), that is new work, not a
regression of this one.

---

## 3. Board manifest and code generation

**Both existing generator scripts are reused, not reimplemented.** They are
plain, hardware-agnostic Python; the only thing that made them ESP-specific was
a target whitelist. Their *output* is consumed by the new pico-sdk project
instead of by the ESP-IDF one.

```
boards/manifests/picocalc.toml ─┐
boards/expansion_drivers.toml  ─┴─> generate_board_profile.py ─> solar_os_board_generated.h
                                                              └─> board profile .cmake
                                                                    │ (capabilities)
flavors/picocalc-core.toml ────────> generate_flavor_config.py ─> solar_os_config.h
packages/solar_os_packages.toml ──┘                            └─> source + package list
doc/manual/*.md ───────────────────> generate_manual.py ───────> solar_os_manual_data.h
```

Two naming details are load-bearing: the board header must be emitted as
`solar_os_board_generated.h` (that is what `include/solar_os_board.h` includes
when `SOLAR_OS_BOARD_GENERATED` is defined) and the flavour header as
`solar_os_config.h` (what the shared sources include). The board profile's
capabilities are CMake variables, so `targets/picocalc/CMakeLists.txt` mirrors
them into `-DSOLAR_OS_BOARD_HAS_*=1` compile definitions, which is what the
ESP-IDF build does through its own component machinery.

The generated header emits ESP-IDF-shaped output — `#include "driver/gpio.h"`,
pin masks as `(1ULL << GPIO_NUM_40)`. Rather than add an RP2350 output mode to
the generator, the shim provides `driver/gpio.h` with `GPIO_NUM_0..47`. That
keeps the generator reusable verbatim, which was the stated preference.

The flavour resolves to **95 source files across 22 groups**.

---

## 4. Compatibility shim layer

`src/port/pico/compat/` lets the hardware-agnostic sources compile against
pico-sdk with no edits. The guiding rule throughout: **where an ESP-IDF facility
has a real RP2350 counterpart, wire it to it; where it does not, say so
honestly rather than return a plausible lie.** A caller told
`ESP_ERR_NOT_SUPPORTED` can adapt; a caller that thinks it armed a wake source
and then sleeps forever cannot.

### 4.1 What the shim contains

| Shim | Backing | Notes |
| --- | --- | --- |
| `esp_err.h` | — | `esp_err_t` as `int`, `ESP_OK`/`ESP_FAIL`/`ESP_ERR_*` with ESP-IDF's numeric values. |
| `esp_log.h` | `printf` | ESP-IDF's `"I (ms) tag: msg"` format kept so log output stays recognisable across targets. Runtime level filter; no per-tag filtering. |
| `esp_check.h` | — | `ESP_RETURN_ON_ERROR` and friends. `ESP_ERROR_CHECK` logs instead of aborting: on RP2350 an abort reboots with no diagnostics. |
| `esp_attr.h` | — | All placement attributes expand to nothing. |
| `sdkconfig.h` | — | Hand-written, and load-bearing: `CONFIG_SPIRAM 0` is what collapses the PSRAM paths (§2). |
| `esp_heap_caps.h` | FreeRTOS heap_4 | **Not a stub.** `solar_os_task_can_create()` gates every task launch on free-size answers, so these report real heap_4 statistics. `MALLOC_CAP_SPIRAM` reports zero. |
| `esp_timer.h` | `time_us_64()` | Same contract: monotonic microseconds since boot. |
| `esp_system.h` | `hardware_watchdog` | `esp_restart()` reboots via the watchdog; `esp_reset_reason()` maps `watchdog_caused_reboot()`. |
| `esp_random.h` | `pico_rand` | RP2350 hardware TRNG. |
| `esp_chip_info.h` | — | Reports `CHIP_RP2350`, 2 cores. Wi-Fi/BLE features are **not** reported even though the CYW43439 is physically present, because no driver is linked — claiming the feature would make `hw` lie. |
| `nvs.h`, `nvs_flash.h` | RAM | A *working* key/value store, not NOT_FOUND stubs — see §4.3. |
| `freertos/*.h` | real kernel | Forwards to the kernel headers by a distinct relative path, then re-adds the two ESP-IDF divergences the sources use: `portENTER_CRITICAL(&mux)` taking a spinlock, and `tskNO_AFFINITY`. |
| `freertos/idf_additions.h` | SMP port | `xTaskCreatePinnedToCore` → `xTaskCreate` + `vTaskCoreAffinitySet`. Real pinning, since the port is built SMP. The `*WithCaps` variants are deliberately **absent** so that switching PSRAM on fails loudly instead of silently using the wrong heap. |
| `driver/gpio.h` | `hardware_gpio` | Open-drain modes return `ESP_ERR_NOT_SUPPORTED` rather than silently configuring push-pull onto a bus expecting open-drain, which can contend. GPIO interrupts are accepted and ignored (nothing in this flavour registers one). |
| `driver/i2c_master.h` | `hardware_i2c` | Handle table bridging ESP-IDF's bus/device object model to RP2350's per-transfer addressing. Per-device SCL speeds cannot be honoured; the shim warns at runtime if asked. |
| `driver/ledc.h` | `hardware_pwm` | Translates `(speed_mode, timer, channel)` addressing to RP2350's per-GPIO slices, deriving wrap/divider from requested frequency and duty resolution. |
| `driver/spi_master.h` | — | Types only; the real transfers are in the panel and SD drivers. |
| `driver/uart.h`, `driver/rtc_io.h`, `soc/soc_caps.h`, `hal/adc_types.h` | — | Enums and RP2350B peripheral counts. |
| `esp_sleep.h`, `esp_pm.h` | — | `ESP_ERR_NOT_SUPPORTED`. RP2350's equivalent is `hardware_powman`; deferred. |
| `esp_flash.h`, `esp_partition.h`, `esp_ota_ops.h` | — | No partition table exists; lookups find nothing and writes refuse. |
| `esp_bt.h`, `esp_netif.h`, `esp_netif_sntp.h` | — | No radio and no network stack are linked. |
| `dirent.h` + `solar_os_compat_pico_vfs.c` | FatFs | POSIX filesystem layer — see §4.4. |
| `inttypes.h` | — | Toolchain gap filler, **not** an ESP-IDF shim — see §4.5. |
| `solar_os_pico_main.c` | — | `main()` → task → `app_main()` → `vTaskStartScheduler()`, so `src/main.c` needs no edits. |

### 4.2 Edits to pre-existing files

"Minimal logic changes" was a hard requirement, so here is the complete,
audited list. **No file under `src/` was modified at all.** Four files changed,
totalling +63/−4 lines, every one of them additive build metadata:

| File | Change | Kind |
| --- | --- | --- |
| `scripts/solaros_board_manifest.py` | +20/−4 | `TARGET_GPIO_MAX` gains `"rp2350": 47`; two hardcoded "must be esp32 or esp32s3" messages now derive from that map; `platformio_board` is required only for `esp32*` targets, since rp2350 builds through pico-sdk and must not invent a PlatformIO board. |
| `boards/expansion_drivers.toml` | +24 | New `[drivers.picocalc-kbd]` entry, `targets = ["rp2350"]`. Purely additive; no ESP32 board references it. |
| `packages/solar_os_packages.toml` | +20 | New `picocalc_keyboard` group and `expansion_picocalc_keyboard` package, `targets = ["rp2350"]`. Purely additive. |
| `.gitignore` | +3 | Ignore `build/`. |

Everything else is new files. `version.txt` and `CHANGELOG.md` are untouched,
as instructed.

Two vendored FatFs files were *configured* after vendoring, which is normal for
FatFs (`ffconf.h` is a template): `FF_USE_MKFS`/`LABEL`/`FIND`/`STRFUNC` on,
`FF_CODE_PAGE` 437 to match ESP-IDF's default, `FF_USE_LFN 3`, `FF_FS_RPATH 2`
(the shell has a cwd), `FF_VOLUMES 2`, `FF_FS_REENTRANT 1`; and `ffsystem.c`'s
`OS_TYPE` set to 3 (FreeRTOS) so `ff_mutex_*` maps onto `xSemaphoreCreateMutex`.

### 4.3 NVS is RAM-backed, and why that choice

Thirteen linked files use NVS (terminal preferences, identity, display and
input config, timezone, log config, battery calibration, …). The shim
implements a **complete, working** key/value store — namespaces, all scalar and
variable-length types, iteration, statistics — backed by RAM.

Reads and writes are fully correct *within a boot*. **Nothing persists across a
reset.**

This is deliberately a working store rather than a set of
`ESP_ERR_NVS_NOT_FOUND` stubs: stubs would make every settings read fail and
send a dozen services down error paths that have never been exercised, hiding
real problems behind a fake one. Flash persistence is deferred
([§8](#deferred--stubbed--not-supported-in-this-pass)).

### 4.4 The POSIX filesystem layer

newlib for arm-none-eabi ships **no `<dirent.h>` at all** (`#error <dirent.h>
not supported`), because bare-metal newlib has no filesystem. ESP-IDF supplies
one through its VFS; `src/port/pico/compat/solar_os_compat_pico_vfs.c` is the
equivalent here, over FatFs. Without it, five files would not build — the file
manager, the shell's fs commands, the shell app, the zip service and the
command table — which is to say `ls`, `cd` and `cat`.

It provides `opendir`/`readdir`/`closedir`/`rewinddir`,
`stat`/`mkdir`/`rmdir`/`unlink`/`rename`/`access`, and the newlib syscall hooks
(`_open`/`_close`/`_read`/`_write`/`_lseek`/`_fstat`) so `fopen`/`fread`/
`fprintf` work on files. pico-sdk declares those syscalls `__weak`, so
overriding them is clean; descriptors 0/1/2 are passed through to `pico_stdio`
so console I/O is unchanged.

**Path mapping preserves the existing on-disk layout.** `/sdcard/...` becomes
`0:/...` for FatFs. That translation is the only place the mount point is
interpreted, so the shell's conventions — the `.shell`, `.ssh` and `.reader`
directories, which are relative paths inside the mounted volume — are valid
exactly as before. Only the block and filesystem plumbing underneath changed.

Deliberately absent, and documented in the file: no symlinks (FAT has none);
`st_mode` reports type plus a fixed mode, because FAT has one read-only bit and
inventing permissions would make `ls -l` look more meaningful than it is; and
`st_atime`/`st_ctime` report `st_mtime`, because FAT stores one timestamp and
"unknown" is closer to mtime than to the epoch.

One collision worth noting: FatFs also names its directory object `DIR`. It is
renamed for the duration of `ff.h` rather than renaming the POSIX one.

### 4.5 A toolchain quirk, not an ESP-IDF shim

`compat/inttypes.h` exists for a different reason from everything else here.
About a dozen sources print 64-bit values and failed on `PRIu64` **despite
correctly including `<inttypes.h>`**. Cause: newlib guards its 64-bit format
macros behind `__int64_t_defined`, which newlib's own `<stdint.h>` sets — but
on this toolchain GCC's `<stdint.h>` wins the include search:

```
.. /usr/lib/gcc/arm-none-eabi/13.2.1/include/stdint.h
... /usr/include/newlib/inttypes.h
```

so the guard never opens. Rather than edit correct sources, or define newlib
internals on the command line, the shim forwards to the real header and fills
in only what is missing. On a correctly packaged toolchain every `#ifndef` is
already satisfied and the file adds nothing.

---

## 5. Mapping table: ESP-IDF driver/service → pico-sdk equivalent

### 5.1 The four new drivers

| Concern | ESP32 / ESP-IDF side | RP2350 / PicoCalc side |
| --- | --- | --- |
| Colour TFT panel | `src/drivers/tft_ili9341.c/.h` (ILI9341/ST7796)<br>`src/services/solar_os_tft_display.c` | `src/drivers/pico/tft_ili9488_picocalc.c/.h`<br>`src/board/pico/solar_os_board_display_ili9488_picocalc.c` |
| Board display vtable | `src/board/solar_os_board_display.h` (contract)<br>`src/board/solar_os_board_display_expansion.c` (wrappers) | **unchanged** — same header, same wrapper file, new ops implementation |
| Renderer | `components/u8g2/src/clib/` | **unchanged**, reused verbatim via new u8x8 callbacks |
| Keyboard co-processor | `src/services/solar_os_cardkb.c`<br>`solar_os_cardkb_codec.c`<br>`solar_os_cardkb_driver.c` | `src/drivers/pico/picocalc_keyboard.c/.h` (transport)<br>`src/services/solar_os_picocalc_keyboard.c/.h` (service + codec)<br>`src/services/solar_os_picocalc_keyboard_driver.c` (descriptor) |
| Keyboard → input path | `src/services/solar_os_input.c` | **unchanged** — same `solar_os_input_keyboard_source_open()` / `solar_os_input_write_char()` handoff |
| SD block device | `src/drivers/sd_card.c/.h` (ESP-IDF sdspi + esp_vfs_fat) | `src/drivers/pico/sd_spi_pico.c/.h` (SD SPI protocol, from spec) |
| Filesystem | ESP-IDF `fatfs` component | `components/fatfs/` (same upstream FatFs) + `components/fatfs/port/diskio_sd_spi_pico.c` |
| Board storage service | `src/board/solar_os_board_storage_sd.c` | `src/board/pico/solar_os_board_storage_sd_pico.c` |
| Storage contract | `src/board/solar_os_board_storage.h` | **unchanged** |
| Battery | `src/drivers/battery_adc.c/.h`<br>`src/board/solar_os_board_battery_adc.c` | `src/drivers/pico/battery_picocalc.c/.h`<br>`src/board/pico/solar_os_board_battery_picocalc.c` |
| Battery contract | `src/board/solar_os_board_battery.h` | **unchanged** |

### 5.2 Platform layer

| Concern | ESP32 side | RP2350 side |
| --- | --- | --- |
| RTOS | ESP-IDF's FreeRTOS fork | `external/FreeRTOS-Kernel` @ V11.3.1, `RP2350_ARM_NTZ` port |
| RTOS config | `sdkconfig.defaults*` | `targets/picocalc/config/FreeRTOSConfig.h` |
| Entry point | `app_main()` called by ESP-IDF | `src/port/pico/solar_os_pico_main.c` → `app_main()` |
| ESP-IDF API surface | native | `src/port/pico/compat/` (§4) |
| POSIX filesystem | ESP-IDF VFS | `src/port/pico/compat/solar_os_compat_pico_vfs.c` |
| Build system | root `CMakeLists.txt` + ESP-IDF `project.cmake` | `targets/picocalc/CMakeLists.txt` + pico-sdk |
| Board driver fragments | `boards/drivers/*_esp_idf.cmake`, `display_ili9341.cmake`, `storage_sdspi.cmake`, `battery_adc.cmake` | `boards/drivers/{gpio,i2c,spi,uart,adc,pwm}_pico.cmake`, `display_ili9488_picocalc.cmake`, `storage_sd_spi_pico.cmake`, `battery_picocalc.cmake` |
| Board manifest | `boards/manifests/t_lora_pager.toml` (closest analogue) | `boards/manifests/picocalc.toml` |
| Flavour | `flavors/core.toml` | `flavors/picocalc-core.toml` |

### 5.3 Two design decisions that deviate from the brief, with reasons

**1. The ILI9488 wire format is 18bpp, not RGB565.** The brief suggested
picking RGB565 (`COLMOD 0x55`) to match the surface pipeline. That would
compile and then produce a garbled display: **ILI9488's 4-wire SPI interface
does not implement the 16bpp memory-write format at all.** 18bpp (3 bytes per
pixel, RGB666) is the only usable serial pixel format, which is why
clockworkpi's own reference driver sets `COLMOD` to `0x66`.

The resolution keeps both halves of the intent: the surface pipeline stays
RGB565 (`foreground_rgb565`/`background_rgb565`, matching every other colour
panel in the tree) and conversion to RGB666 happens at the SPI write boundary.
The 5-bit red/blue channels widen to 6 bits by replicating the top bit into the
new low bit, so full white stays full white — a plain left-shift would map
`0x1F` to `0x3E` and make white read slightly grey.

**2. Display brightness goes over I2C, not a GPIO.** The PicoCalc's LCD
backlight is not wired to a Pico pin. It is driven by the STM32 co-processor and
set by writing register `0x05` (`REG_ID_BKL`) over the keyboard I2C link. So the
display driver's brightness op calls into the keyboard transport, and requires
the keyboard driver to have attached first. This also means there is no
`SOLAR_OS_BOARD_PIN_LCD_BL` in the manifest and no PWM or pulse-dimmer
backlight path.

---

## 6. Hardware sources used

Everything below came from **clockworkpi's own repository**,
`github.com/clockworkpi/PicoCalc` — the vendor's firmware and reference
drivers, not community reverse-engineering. Confidence is high on *values* and
zero on *timing*, since nothing was observed on hardware.

| Item | Value | Source file | Confidence |
| --- | --- | --- | --- |
| Keyboard I2C address | `0x1F` | `Code/picocalc_keyboard/conf_app.h` (`SLAVE_ADDRESS`) | High — vendor firmware |
| Keyboard bus / pins | `i2c1`, SDA 6, SCL 7 | `Code/picocalc_helloworld/i2ckbd/i2ckbd.h` | High — vendor reference driver |
| Keyboard bus speed | 10 kHz **ceiling** | same; header states "if dual i2c, then the speed of keyboard i2c should be 10khz" | High — stated as a requirement, not a default. The driver clamps rather than obeys a higher manifest value. |
| Register map | `REG_ID_VER` 0x01 … `REG_ID_OFF` 0x0e | `Code/picocalc_keyboard/reg.h` | High — vendor firmware enum |
| Wire format | write `{reg}` (bit 7 = write) then read 2 bytes; per-register meaning | `Code/picocalc_keyboard/picocalc_keyboard.ino` (`receiveEvent`/`requestEvent`) | High — vendor firmware |
| FIFO encoding | `REG_ID_FIF` 0x09 → `{state, keycode}`, 31-deep | same + `fifo.h` | High |
| Key state enum | idle 0, pressed 1, hold 2, released 3 | `Code/picocalc_keyboard/keyboard.h` | High |
| Key codes | arrows 0xB4–0xB7, ESC 0xB1, mods 0xA1–0xA5, F-keys, Home/End/PgUp/PgDn/Del | same | High |
| Co-processor turnaround | 16 ms between register write and read | `i2ckbd.c` (`sleep_ms(16)`); matches `KEY_POLL_TIME` | High as a value, **unverified** as sufficient |
| LCD controller | ILI9488, 320x320 | `Code/picocalc_helloworld/lcdspi/lcdspi.c` (`pico_lcd_init`, `#ifdef ILI9488`) | High |
| LCD bus / pins | `spi1`, SCK 10, MOSI 11, MISO 12, CS 13, DC 14, RST 15 | `lcdspi/lcdspi.h` | High |
| LCD clock | 25 MHz | `lcdspi.h` (`LCD_SPI_SPEED`) | High as vendor's choice, unverified here |
| LCD init sequence | gamma tables, power/VCOM/frame-rate registers, `MADCTL 0x48`, `COLMOD 0x66`, inversion on | `lcdspi.c` | High — transcribed verbatim; **panel-specific, not interchangeable with an ILI9341 sequence** |
| SD bus / pins | `spi0`, SCLK 18, MOSI 19, MISO 16, CS 17 | `Code/MP3Player/config.h` | High — also the Pico Plus 2 W's `PICO_DEFAULT_SPI_*` pins, which corroborates it |
| SD card detect | GPIO 22, active low, internal pull-up | `Code/pico_multi_booter/boot.c` (`SD_DET_PIN`) | Medium-high — one source in the vendor repo |
| Battery path | AXP2101 PMU on the STM32's own I2C (PB10/PB11), republished to the host at `REG_ID_BAT` 0x0b | `conf_app.h` (`CONFIG_PMU_SDA/SCL`), `picocalc_keyboard.ino` (`sync_bat`), `i2ckbd.c` (`read_battery`) | High — vendor firmware |
| Battery encoding | percentage 0–100, bit 7 set while charging, 0 when disconnected | `sync_bat()` | High |
| Audio pins | PWM L 26, R 27 | `Code/picocalc_helloworld/pwm_sound/pwm_sound.h` | High — recorded as reserved; audio is out of scope |
| RP2350B ADC pins | GPIO 40–47 (`ADC_BASE_PIN` 40 when `PICO_RP2350A == 0`) | pico-sdk `src/rp2350/hardware_regs/.../platform_defs.h` | High |

**The battery deserves a specific note**, because the brief said not to guess.
It was not guessed, and it is not stubbed. The PicoCalc genuinely has no
resistor divider to a Pico ADC pin — the fuel gauge lives behind the STM32 —
and the vendor firmware exposes it at a known register. So the driver is real.

The one mismatch is *units*: `solar_os_board_battery_sample_t` carries
`battery_mv`, and the co-processor reports a **percentage**. There is no
register exposing the AXP2101's raw millivolts; `getBattVoltage()` is called
only for the STM32's own serial debug output and never published. So the driver
reports the percentage exactly (it is the fuel gauge's own figure) and fills
`battery_mv` from a documented piecewise-linear single-cell Li-ion curve with
**`calibrated = false`** — which is the existing contract's way of saying "this
number is an estimate", and which every consumer already handles.

**Not found, and therefore not claimed:** an authoritative map of which Pico
GPIOs the PicoCalc mainboard breaks out to a user-accessible header. The
manifest declares only pins with a confirmed function as `fixed`, and the free
expansion pins it lists (GPIO 40–46) are the *Pico Plus 2 W module's* extra
castellations, not PicoCalc pins — the mainboard wires the standard 40-pin Pico
footprint, so those are reachable only if the module's extra headers are. That
is called out in the manifest as well as here so nobody mistakes them for
PicoCalc pins.

---

## 7. Build status

Verified by actually invoking `arm-none-eabi-gcc` 13.2 through the vendored
pico-sdk CMake build. **This is the strongest claim made anywhere in this
document.** It means "compiles" and, where stated, "links" — never "works".

### 7.1 What compiles today

| Target | State |
| --- | --- |
| `solar_os_compat` — the whole ESP-IDF shim layer + NVS + VFS | **compiles clean** |
| `solar_os_rtos` — `solar_os_task.c`, `solar_os_queue.c` | **compiles clean, zero source edits** |
| `solar_os_picocalc_drivers` — all four new drivers + board glue | **compiles clean** |
| `solar_os_fatfs` — FatFs + diskio glue | **compiles clean** |
| `solar_os_u8g2` — renderer + 29 font faces | **compiles clean** |
| `solar_os_cjson`, `solar_os_miniz` | **compiles clean** |
| `picocalc-core` — full firmware | **all 95 shared sources compile; does not link yet** |

The default build (`cmake --build build/picocalc`) builds everything except the
firmware executable, so a normal build proves the drivers and RTOS layer. The
firmware is behind `-DSOLAR_OS_PICOCALC_FULL_FIRMWARE=ON`.

### 7.2 The next blocking error

The firmware compiles completely and fails at **link** with **61 undefined
symbols**, in these groups — no unexplained residue:

| Group | Count | What is missing |
| --- | --- | --- |
| `uart_port_*` | 9 | RP2350 backend for the UART service (`drivers/pico/uart_port_pico.c`) |
| `i2c_bus_*` | 8 | RP2350 backend for `solar_os_buses` I2C (`drivers/pico/i2c_bus_pico.c`) |
| `flash_storage_*` | 9 | `drivers/flash_storage.c`, excluded by name — needs a flash/partition abstraction |
| `spi_bus_*`, `spi_device_polling_transmit` | 5 | RP2350 backend for `solar_os_buses` SPI |
| `gpio_port_*`, `gpio_install_isr_service`, `gpio_isr_handler_add` | 5 | RP2350 backend for the GPIO service, plus a GPIO ISR dispatcher |
| `pwm_port_*` | 4 | RP2350 backend for the PWM service (`driver/ledc.h` exists; this is the SolarOS-side wrapper) |
| `adc_port_*` | 3 | RP2350 backend for the ADC service |
| `solar_os_ramfs_*` | 7 | `services/solar_os_ramfs.c`, excluded by name — needs a VFS registration hook |
| `solar_os_nvs_backup_*`, `solar_os_cdc_init` | 3 | excluded by name |
| `portENTER_CRITICAL`, `portEXIT_CRITICAL`, `taskENTER_CRITICAL`, `taskEXIT_CRITICAL` | 4 | referenced as *functions* from a file where the shim's macro form is not visible; needs one include ordering fix |
| `xPortInIsrContext`, `xSemaphoreCreateMutexStatic`, `nvs_find_key`, `ESP_ERROR_CHECK` | 4 | small shim additions |

**To continue**, the single highest-value next step is the bus/port backend
tranche — `drivers/pico/{i2c_bus,spi_bus,gpio_port,uart_port,pwm_port,adc_port}_pico.c`
against the interfaces in `src/drivers/{i2c_bus,spi_bus,gpio_port,uart_port,pwm_port,adc_port}.h`.
That is 34 of the 61 symbols and would leave only the four
explicitly-excluded files plus ~11 small shim gaps. The exact command to see the
current list:

```sh
cmake -S targets/picocalc -B build/picocalc -DSOLAR_OS_PICOCALC_FULL_FIRMWARE=ON -G Ninja
cmake --build build/picocalc --target picocalc-core -- -k 0 2>&1 \
  | grep -oE "undefined reference to .[a-zA-Z_0-9]*'" | sort -u
```

`picocalc-core.uf2` is produced by `pico_add_extra_outputs()` and will appear in
`build/picocalc/` as soon as the link succeeds.

### 7.3 Numbers that are guesses

Flagged explicitly because they compile fine and can only be settled on a
bench:

- `configTOTAL_HEAP_SIZE` 256 KB of RP2350's 520 KB — a starting point, not a
  measured budget. Task admission decisions are made against this heap.
- `SOLAR_OS_MAIN_TASK_STACK` 8192 words for the boot task — never measured
  against a high-water mark.
- SD transfer clock 12.5 MHz — deliberately conservative (the vendor runs the
  *display* at 25 MHz on a different bus); the card slot's timing is
  uncharacterised.
- Display `preferred_stream_fps` 20 and `max_stream_pixels_per_second` — derived
  arithmetically from 25 MHz × 3 bytes/pixel, ignoring per-transfer overhead.
- SD init/busy/token timeouts — from the SD specification's nominal figures,
  widened; real cards vary.
- The battery percent→millivolt curve — a generic single-cell Li-ion
  approximation, flagged in-band by `calibrated = false`.

---

## Deferred / stubbed / not supported in this pass

### Whole packages

**`net` (Wi-Fi / lwIP / CYW43439) — not supported.** Nothing network-related is
linked. Where it would hook in:

- pico-sdk already carries `pico_cyw43_driver` and `pico_lwip`, but as *nested
  submodules of pico-sdk* that this port does not initialise (configure warns
  about exactly this). Step one is `git submodule update --init` inside
  `external/pico-sdk`.
- The board would then need `PICO_CYW43_SUPPORTED` wiring plus an lwIP
  configuration header, and a SolarOS-side `solar_os_wifi` backend replacing
  the `esp_wifi`/`esp_netif` calls in `src/services/solar_os_wifi.c`.
- Every flavour group gated by `wifi = true` is off as a consequence:
  `wifi`, `http_client`, `wireguard`, `ssh`, plus (in richer flavours)
  `web_browser`, `ftp`, `telnet`, `http_server`, `mqtt`, `ntp`, `slip`, `osc`,
  `chat*`, `gateway_sync`, `email`, `agent`, `espnow`, `documentation_sync`.
- `esp_netif_sntp.h` is shimmed to `ESP_ERR_NOT_SUPPORTED`, so
  `services/solar_os_time.c` takes its manual/RTC path.

**`ssh` / `scp` — not supported**, because they depend on `net`. The shell
commands are not linked and the `.ssh` directory convention is unused (though
the path layout is preserved, so a later pass inherits it).

**`audio` — not supported.** No audio backend was ported. The PicoCalc's PWM
audio pins (GPIO 26/27) are recorded in the manifest as reserved so nothing
else claims them. `audio_support`, `audio_commands`, `player`, `recorder`,
`synth`, `function_generator`, `webradio` and the codec/DAC expansion packages
are all off.

**Python and Lua runtimes — not supported.** `components/micropython_embed` and
`components/lua` are not built. These are also the main PSRAM consumers, which
is part of why §2 holds.

**All `ble`-prefixed shell commands and the BLE stack — not supported, and
deliberately not stubbed.** `solar_os_ble*.c`, the NimBLE backend, the HID
report map and the NVS pairing store are simply **absent from the source
list** — there is no dead code standing in for them. `esp_bt.h` exists only
because `services/solar_os_power.c` includes it to release the BT controller's
memory on ESP32; that call is a no-op here. `esp_chip_info()` does not report
BLE as a feature even though the CYW43439 is physically present.

### Within otherwise-supported areas

| Deferred | Consequence | Where it would go |
| --- | --- | --- |
| **NVS flash persistence** | Settings work within a boot, reset to defaults on reboot | A wear-levelled region carved out of the 16 MB flash below the firmware, via pico-sdk `hardware_flash`; replaces the RAM store in `solar_os_compat_pico.c` |
| **Partition table / OTA** | `esp_partition_*` finds nothing, OTA cannot start | Same flash-layout work; note pico-sdk's own update model is the UF2 bootrom, a different design |
| **Bus/port service backends** (i2c, spi, uart, gpio, pwm, adc) | The `i2c`/`spi`/`uart`/`gpio`/`pwm`/`adc` shell commands and arbitrary expansion devices have no backend. This is what blocks the link. | `src/drivers/pico/*_pico.c` against the existing `src/drivers/*.h` interfaces |
| **`drivers/flash_storage.c`** | No internal-flash filesystem | Excluded by name in the CMake `SOLAR_OS_DEFERRED_SRCS` list |
| **`services/solar_os_ramfs.c`** | No RAM filesystem | Needs a VFS mount-registration hook in the new VFS layer |
| **`services/solar_os_cdc.c`** | USB console goes through pico-sdk's `pico_stdio_usb` instead | pico-sdk's stdio model differs from ESP-IDF's USB-serial-JTAG driver |
| **`services/solar_os_nvs_backup.c`** | No NVS snapshot/restore | Depends on the partition work |
| **Deep sleep / power management** | `esp_sleep_*` and `esp_pm_*` return `ESP_ERR_NOT_SUPPORTED` | `hardware_powman` (dormant/sleep with a powman alarm or GPIO wake) |
| **GPIO interrupts** | `gpio_config`'s `intr_type` is accepted and ignored | `gpio_set_irq_enabled_with_callback()`; nothing in this flavour registers an ISR (the keyboard is polled, card-detect is read on demand) |
| **Open-drain GPIO** | Returns `ESP_ERR_NOT_SUPPORTED` | RP2350 has no open-drain driver; emulation must be per-transition at the call site |
| **Per-device I2C clock speeds** | All devices on a bus run at one rate; shim warns | RP2350 sets the rate per peripheral, not per device |
| **PSRAM** | 8 MB present, unused | `hardware_psram`; only worth it if a later pass needs the capacity |
| **Multi-partition SD mounts** | Only partition 1 is mountable; others enumerate but report `mountable = false` | `FF_MULTI_PARTITION` in `ffconf.h` plus volume-to-partition mapping |
| **File timestamps** | Files are stamped 1980-01-01 | `solar_os_board_storage_fattime()` needs wiring to the time service |
| **SD erase-block alignment** | `f_mkfs` gets `GET_BLOCK_SIZE = 1` (unknown), giving a slightly less optimal format | Read `AU_SIZE` from the SD status register in `sd_spi_pico.c` |

### The requested shell command surface

The brief named `help, pkg, apps, port list/status, log, mem, top, sd,
ls/cat/cd/etc.`. **All of them are in the flavour and all of their source files
compile.** None had to be dropped:

| Command | Source file | Compiles |
| --- | --- | --- |
| `help` | `src/shell/solar_os_shell_manual.c` | yes |
| `pkg`, `mem`, `top`, `sd` | `src/shell/solar_os_shell_system.c` | yes |
| `apps`, `port`, `log` | `src/shell/solar_os_shell_commands.c` | yes |
| `ls`, `cat`, `cd` and the rest of the fs set | `src/shell/solar_os_shell_fs.c` | yes |

Caveats that are about *behaviour*, not compilation:

- `ls`/`cat`/`cd` depend on the new VFS layer and on the SD driver. Both
  compile; neither has read a real card.
- `sd` reports through `solar_os_board_storage_get_status()`, which is
  implemented.
- `mem`/`top` read the heap and task statistics the shim supplies; the figures
  are real heap_4 numbers, but `heap_caps_get_largest_free_block()` returns
  total free rather than the true largest block (heap_4 does not expose it),
  making admission control slightly optimistic. That is recorded in the shim.
- `port` and `log` do not depend on the missing bus backends.
- None of the above can run at all until the link completes (§7.2).

---

## Hardware-validation gap

**Nothing in this port has ever been run on a PicoCalc.** There was no
PicoCalc, no Pico Plus 2 W, and no SD card available while it was written.
Every claim in this document is one of:

- **Compiles / links** — verified by actually invoking `arm-none-eabi-gcc`
  through the vendored pico-sdk CMake build. This is the strongest claim made
  anywhere here, and §7 states precisely how far it goes.
- **Sourced** — a register number, pin assignment or init sequence taken from a
  named primary source, cited in §6 with its confidence level.
- **Untested logic** — believed correct by inspection only. This covers the SD
  protocol state machine, the ILI9488 pixel conversion, the keyboard codec's
  modifier handling, the FAT timestamp arithmetic and the MBR parser.

No statement here should be read as "works on hardware". The specific classes
of thing most likely to compile perfectly and then fail on a bench:

- **Timing.** The 10 kHz I2C ceiling and 16 ms co-processor turnaround, the
  25 MHz display clock, the 12.5 MHz SD clock, and every SD initialisation and
  busy-wait window. None has been observed.
- **Display geometry.** `MADCTL 0x48` with zero row/column offsets is what the
  vendor's driver uses, but if the image comes out shifted, mirrored or wrapped,
  the offsets and MADCTL are the first things to try.
- **Keyboard encoding.** The register map is from vendor firmware, but the
  vendor's *host* reference driver contains a legacy path keying on `0x7e02`/
  `0x7e03` for Ctrl that does not match `keyboard.h`'s `KEY_MOD_CTRL` (0xA5).
  This port implements the firmware constants. If Ctrl behaves oddly on a unit
  with older keyboard firmware, that discrepancy is the place to look.
- **SD card variability.** Cards differ widely in how long they take to leave
  busy and how they respond to marginal clocking. The retry and timeout
  behaviour is from the specification, not from observation.
- **Heap sizing.** 256 KB is a guess, and task admission is measured against it.

Two smaller correctness risks worth naming: `heap_caps_realloc()` cannot know
the old block's size under heap_4 and so copies the new size (safe when
growing, which is all the current callers do), and `rename()` is not atomic
because it unlinks an existing destination first — FAT offers no atomic
replace.
