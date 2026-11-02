# Agent Catch-Up: ESP32 Opus Benchmark

> Current project state and next work. Updated 2026-11-01.

## Goal and setup

This is a computer architecture project measuring and optimizing Opus encoding on an
ESP32 by changing Opus internals. The headline metric is CPU cycles per 20 ms frame.

- Board: ESP-WROOM-32 / original ESP32, dual-core Xtensa LX6, 4 MB flash, no PSRAM.
- ESP-IDF: v6.1. The project builds with the Espressif IDF extension or `idf.py` from
  an ESP-IDF terminal.
- Benchmark settings: 240 MHz, `-O2`, fixed-point Opus, 16 kHz mono, 320 samples/frame,
  VOIP application, voice signal, 20 kbps VBR. A sweep runs complexities 0, 2, 5, 10;
  each level uses one warm-up pass and five timed passes of 50 frames.
- Keep the clock, compiler optimization, and benchmark settings fixed when comparing
  experiments. Record any intentional setting changes with the results.

## Repository layout and current integration

- `components/opus/` contains the vendored Xiph Opus source from upstream commit
  `677070ed` (2026-10-31). Its nested Git metadata is removed; this project tracks the
  source files directly.
- `components/opus/CMakeLists.txt` builds Opus encoder/decoder sources directly as an
  ESP-IDF component. It uses fixed-point math and a 60 KB non-thread-safe scratch stack.
  The scratch stack is suitable for the benchmark's single Opus-calling task; do not call
  Opus concurrently without changing the scratch allocation mode.
- The component CMake file reads source groups from upstream `opus_sources.mk`,
  `celt_sources.mk`, and `silk_sources.mk`. Its local parser is needed because ESP-IDF
  evaluates component CMake files from the build directory during dependency scanning.
- `main/CMakeLists.txt` privately requires `opus`; `main/opus_benchmark_main.c` is the
  active app and uses the public Opus API. `main/idf_component.yml` no longer has external
  codec dependencies.
- `sdkconfig.defaults` sets 4 MB flash, 240 MHz, and performance optimization. It does
  not contain component-specific Opus Kconfig settings.
- `dependencies.lock` contains only the ESP-IDF dependency.

### Component integration gotchas

- The component parses upstream's continued source lists from the three `*_sources.mk`
  files. Keep the local parser in `components/opus/CMakeLists.txt`; Opus's generic CMake
  source-list helper assumes a source-tree working directory and did not provide valid
  sources in ESP-IDF's dependency scan.
- ESP-IDF evaluates a component once during dependency scanning, when `COMPONENT_LIB` is
  an interface target. Keep the private `target_compile_definitions()` inside the
  `if(NOT CMAKE_BUILD_EARLY_EXPANSION)` guard, or CMake rejects the private properties.
- The Xtensa toolchain declares `lrint()` and `lrintf()` in `math.h`, but upstream's
  configure checks are not run in this ESP-IDF build. The component therefore defines
  `HAVE_LRINT` and `HAVE_LRINTF` to avoid Opus's warning fallback, which ESP-IDF promotes
  to an error.
- Do not restore old Opus Kconfig entries to `sdkconfig.defaults`; the local component
  has no Opus Kconfig menu. Its fixed-point and scratch-stack settings are compile
  definitions in the component build file.

## Verified local-component benchmark

Built with ESP-IDF 6.1, flashed, and run on the ESP32. Output was deterministic. All
frames used SILK (TOC config 9). The current component build reports
`libopus unknown-fixed`: fixed-point is enabled; the version string is unknown because
the vendored tree has no Git history for upstream's version-generation script.

| Complexity | Min cycles | Avg cycles | Max cycles | Avg time | Max time | Real-time worst frame | Bytes/frame | Hash |
|---:|---:|---:|---:|---:|---:|---|---:|---|
| 0 | 1,626,475 | 2,086,956 | 2,364,289 | 8,696 us | 9,851 us | Yes | 45.8 | `0x317FC6D7` |
| 2 | 2,182,330 | 2,661,713 | 2,962,903 | 11,090 us | 12,345 us | Yes | 44.6 | `0xE56B6272` |
| 5 | 2,976,527 | 3,548,216 | 3,843,391 | 14,784 us | 16,014 us | Yes | 39.8 | `0x2550DE12` |
| 10 | 5,939,168 | 6,620,667 | 6,898,777 | 27,586 us | 28,745 us | **No** | 42.4 | `0x8949A3CB` |

The real-time budget is 20,000 us/frame. Encoder state size is 29,236 bytes; creation
used 29,700 heap bytes. After the run, free 8-bit heap was 187,900 bytes (minimum
158,092). The benchmark task has a 24 KiB stack, with 20,860 bytes unused.

## Current optimization finding

The component compile command does not define `OPUS_XTENSA_LX7`. The vendored SILK
source has Xtensa fixed-point implementations behind this macro in
`silk/xtensa/macros_lx7.h` and `silk/xtensa/SigProc_FIX_lx7.h`; they are currently
disabled. This is a plausible contributor to complexity 10 exceeding the real-time
budget, but it has not been confirmed by a controlled local A/B run. Check that those
instructions are supported on the target ESP32 LX6 before enabling them.

For each optimization, change one thing at a time, rebuild, flash, and compare the
output hashes and cycles. A changed hash requires investigation; record a new correctness
reference only when a bitstream change is intentional.

## Next work

1. Make a clean commit of the imported local component and benchmark baseline before
   optimization experiments.
2. Run an A/B build with the appropriate Xtensa SILK optimization enabled, after
   confirming target instruction support. Compare every complexity's cycles and hashes.
3. Add stage-level cycle profiling in SILK (pitch analysis, noise-shaping analysis,
   predictor coefficients, and NSQ) to identify the dominant high-complexity costs.
4. Optimize one measured hotspot at a time and record cycles, real-time status, and hash.

## Hardware / flashing notes

- UART flashing is used; the original ESP32 has no native USB. This board uses a CP210x
  USB-serial bridge. Port names are `COMx` on Windows, `/dev/cu.usbserial-*` on macOS,
  and `/dev/ttyUSB*` on Linux.
- If flashing reports `Wrong boot mode detected (0x13)`, hold BOOT, tap EN, then release
  BOOT after the chip is detected. Close the serial monitor before flashing.
- Press EN to rerun the flashed benchmark. A panic may reboot repeatedly; hold EN or
  unplug to stop it.
- Only one program can own the serial port at a time.

## Contributor notes

- Treat pasted build logs and serial output as evidence; label assumptions clearly.
- `sdkconfig` and `.vscode` device settings are machine-specific. Shared build defaults
  belong in tracked `sdkconfig.defaults`.
- The build uses CMake/Ninja, not Makefiles. Avoid spaces and very long paths for
  compatibility with ESP-IDF tools on Windows.
