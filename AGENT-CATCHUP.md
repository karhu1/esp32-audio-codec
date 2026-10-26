# Agent Catch-Up: ESP32 Opus Benchmarking Project

> Read this first. It summarizes everything done so far, what is verified vs. assumed,
> and what to do next. Last updated: 2026-10-25.
> This file is machine-agnostic: it is meant for every contributor, on any OS.

## 1. Goal

A **computer architecture class group project**: first run a naive Opus encoder on an ESP32,
measure it, then **optimize Opus for the ESP32's Xtensa LX6 architecture by editing Opus
internals** (not just API parameters). Primary metric: **CPU cycles per 20 ms audio frame**.
Contributors may be new to C/ESP32/embedded work, so explain things plainly, step by step,
and avoid assuming prior knowledge.

## 2. Environment

| Item | Value |
|---|---|
| Board | ESP-WROOM-32 ESP32 dev board (3-pack, Amazon B08D5ZD528; likely HiLetgo-style, unconfirmed). Micro USB. |
| Chip | ESP32 dual-core Xtensa LX6, rev v3.1, 4 MB flash, **no PSRAM**, no microphone/codec |
| USB-serial chip | **CP210x** (Silicon Labs). Install the CP210x driver if no serial port appears. Port names by OS: `COMx` (Windows), `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART` (macOS), `/dev/ttyUSB*` (Linux). |
| ESP-IDF | **v6.1**, esptool v5.6.0 |
| Flash method | UART (JTAG/DFU not applicable: no probe, original ESP32 has no native USB) |
| Automated agents | Typically run in a cloud sandbox: they **cannot build/flash ESP-IDF or reach a contributor's hardware or files**. A human builds, flashes and pastes serial output. Code written by an agent was only syntax-checked against stub headers. |

### Settings in use (set via menuconfig)
- CPU frequency **240 MHz** (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240`). A stale build once kept printing 160; a rebuild/reflash fixed it.
- Flash size set to 4 MB (the boot log shows `SPI Flash Size : 4MB`).
- Compiler optimization: **Performance (-O2)** (was Debug `-Og`).

## 3. Flashing / operating quirks

- **Flash fails with `Wrong boot mode detected (0x13)`**: hold **BOOT**, tap **EN**, release BOOT once `Chip is ESP32...` appears. Needed on this board.
- Only one program can hold the serial port at a time. Close the serial monitor (**Ctrl+]**) before flashing.
- Program lives in flash and runs on power-up. **EN** = reset/rerun. A crash reboots in a loop by default; hold EN or unplug to stop (or set the panic handler to "Print registers and halt" in menuconfig).
- Avoid long project paths and spaces in them (matters most on Windows).
- Harmless warnings: esptool deprecation notices; `Detected size(4096k) larger than the size in the binary image header(2048k)` (before the flash size fix).

## 4. Files (in the project's `main/`)

| File | Purpose |
|---|---|
| `base_benchmark_main.c` | Stand-in harness: 440 Hz tone, int16 autocorrelation kernel (80 lags x 320 samples = 25,600 MACs), cycle timing with `esp_cpu_get_cycle_count()`. Original version printed CPU MHz from a compile-time macro (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`); suggested switching to `esp_rom_get_cpu_ticks_per_us()`. Superseded by the Opus benchmark. |
| `opus_benchmark_main.c` | **Current main.** Runs `opus_encode()` on a synthetic speech-like signal (30-harmonic wobbling-pitch tone + envelope + deterministic noise). 16 kHz mono, 20 ms frames (320 samples), 50 frames per pass, 1 untimed warm-up pass + 5 timed passes, `OPUS_APPLICATION_VOIP`, `OPUS_SIGNAL_VOICE`, 20 kbps, VBR on, complexity 0. Runs in its own task pinned to core 1 (stack currently 16 KB). Prints min/avg/max cycles, time, CPU load, mode counts (SILK/hybrid/CELT from the TOC byte), bytes/frame, **FNV-1a hash of all output bytes**, determinism check, encoder size, heap and stack stats. Runs once per reset. |

`main/CMakeLists.txt` must list the active source (`idf_component_register(SRCS "opus_benchmark_main.c" INCLUDE_DIRS ".")`).

## 5. Opus component and the IRAM bug (important)

- Added with `idf.py add-dependency "esphome/micro-opus^0.4.1"`. Reports `libopus 1.4-fixed`, so **fixed-point is enabled**. Alternative considered: `78/esp-opus` (thin upstream wrapper; fixed-point enabling not verified).
- micro-opus is **not stock Opus**: it adds Xtensa DSP instructions (MULSH, CLAMPS, NSAU, MULA.DD.*, ROUND.S; e.g. `silk_SAT16_lx7` appears in backtraces) and a thread-local "pseudostack" (default 120 KB, min 60 KB, Kconfig `OPUS_PSEUDOSTACK_SIZE`). Menu: `Component config -> Opus Audio Codec`. The exact pseudostack size/placement values in use were not confirmed.
- **Bug in v0.4.1**: `patches/thread_local_stack.c` allocated the pseudostack with `MALLOC_CAP_INTERNAL` without `MALLOC_CAP_8BIT`. With no PSRAM and tight DRAM, the allocation can land in the **IRAM heap**, which only allows aligned 32-bit access, causing `LoadStoreError` (EXCCAUSE 3, EXCVADDR `0x4008A570`, in `silk_biquad_alt_stride1`) on the first `opus_encode()`. Fixed upstream in commit `4eef306` (#72, "Cleanup pseudostack allocation paths"), not in any tagged release as of 0.4.1.
- **Local workaround** in `managed_components/esphome__micro-opus/patches/thread_local_stack.c`: replace the `#if defined(CONFIG_OPUS_PSEUDOSTACK_...)` block with
  `#define OPUS_ALLOC_SCRATCH(size) heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`.
  If the old block is kept, wrap it in `#if 0 ... #endif` rather than `//` comments: `//` lines ending in backslashes trigger a harmless `-Wcomment` "multi-line comment" warning.
- **Fragile**: `managed_components/` is gitignored and can be overwritten by the component manager, so this edit does not travel between machines. After editing component sources, run **ESP-IDF: Full Clean**, because sources are staged into `build/esp-idf/esphome__micro-opus/opus-staged/`.
- Plan: copy the component into the project's `components/` folder (with the `lib/opus` submodule, i.e. clone `https://github.com/esphome-libs/micro-opus` recursively) so edits are tracked in git and include the upstream fix.

## 6. Results so far

Measured on one board; reproduce on yours before comparing across boards.

### Stand-in kernel (toy autocorrelation, 25,600 MACs/frame)
| Config | Avg cycles/frame | Cycles/MAC | Time/frame |
|---|---|---|---|
| -Og, 160 MHz | 539,016 | 21.0 | 3368.9 us |
| -Og, 240 MHz | 538,875 | 21.0 | 2245.3 us |
| -O2, 240 MHz | 359,331 (min 359,144, max 360,406) | 14.0 | 1497.2 us |

Takeaways: cycle count is independent of clock (compute-bound; time scales 160/240 exactly); -O2 gave 1.5x; run-to-run spread about 0.3%.

### Opus encoder baseline (-O2, 240 MHz, complexity 0, 20 kbps, VOIP, 16 kHz mono)
| Metric | Value |
|---|---|
| Cycles/frame | min 1,460,186 / avg 1,851,940 / max 2,087,297 (worst frame index 20) |
| Time/frame | avg 7716 us, max 8697 us (budget 20,000 us) |
| CPU load (1 core) | avg 38.6%, worst 43.5%; real-time YES (avg and worst) |
| Cycles/sample | about 5,790 |
| Modes | SILK 250, hybrid 0, CELT 0 |
| Output | 45.8 bytes/frame, about 18.3 kbps (about 14x smaller than 256 kbps raw) |
| **Output hash** | **`0x317FC6D7`** (correctness reference; deterministic across passes) |
| Encoder state | 15,228 bytes; heap used by create 15,364 B; heap free about 181 KB |
| Task stack | 16,384 allocated, 12,940 never used (about 3.4 KB used; 8 KB would suffice) |

Notes:
- Min-to-max spread (about 43%) is far larger than the toy kernel's and the output is deterministic, so it is **content-dependent work, not noise** (hypothesis, not yet verified; per-frame logging would confirm).
- The hash is valid **only for this exact config** (bitrate, complexity, signal, sample rate, fixed-point build). Re-baseline when any of those change. Compiler flags/optimizations should NOT change it (integer math); a changed hash after an optimization means the bitstream changed (bug).
- Baseline already includes micro-opus's Xtensa DSP optimizations.

## 7. Hardware facts relevant to optimization

- Original ESP32 (LX6): ~520 KB SRAM total; DRAM free at boot ~300 KB (heap regions: 146 KiB + 111 KiB + 14 KiB + 6 KiB); IRAM heap ~87 KiB is **32-bit-access only** (never use for byte/halfword data).
- Single-precision hardware FPU; **double precision is software (very slow)**. **No SIMD** (that is ESP32-S3). Has MAC16 multiply-accumulate unit and zero-overhead loops.
- Flash code runs through a cache; `IRAM_ATTR` keeps hot code out of it. Memory does not speed up with core clock, so cycle count rising at higher MHz indicates memory-bound code.
- Default app partition is 1 MB (`factory`, 0x100000).

## 8. Suggested next steps (not yet done)

1. **Complexity sweep** (0, 2, 5, 10) and record cycles + bytes/frame; find where real-time breaks.
2. **Per-frame cycle log** to explain the 43% min-to-max spread.
3. **Copy micro-opus into `components/`** with the IRAM fix; remove reliance on `managed_components/` edits.
4. **Profile by stage**: add cycle accumulators around SILK stages (pitch analysis `silk_find_pitch_lags`, noise shaping analysis, `find_pred_coefs`, NSQ quantizer, etc. in `silk_encode_frame_FIX`) to find the top hotspots before optimizing.
5. **Build a stock upstream baseline** (no Xtensa DSP asm) so the team can separate its own gains from the component's.
6. **Optimize hotspots**, one change at a time, with a table row each: `-O3`, loop unrolling, `IRAM_ATTR`, MAC16/`MULA.DD` intrinsics or inline asm, hot-table placement, using both cores. Candidate files: `celt/pitch.c` (`celt_pitch_xcorr`), `celt/kiss_fft.c`, `celt/mdct.c`, SILK filters/NSQ, macros in `celt/arch.h`, `celt/fixed_generic.h`, `silk/xtensa/SigProc_FIX_lx7.h`.
7. **Correctness gate after every change**: output hash must match baseline; also validate against upstream test vectors / `opus_compare` when practical.
8. **Real audio input**: convert a clip with `ffmpeg -i in.wav -ar 16000 -ac 1 -f s16le -acodec pcm_s16le test.raw`, embed via `EMBED_FILES` or `xxd -i`, feed 320-sample frames; optionally add an encode->decode round trip and print decoded PCM over serial so it can be played back on a computer.
9. Optional: unroll / `-O3` experiments on the toy kernel (cheap, good for the report); JTAG probe only if a real debugger is needed (probably not).

## 9. Working agreements and gotchas for agents

- Contributors paste serial output; confirm facts against it, and say plainly when something is unverified (menuconfig names, ESP-IDF 6.1 compatibility beyond what has built so far).
- Amazon blocks automated fetches; product details came from a pasted listing title.
- The project has been developed with VS Code + the Espressif IDF extension (Build/Flash/Monitor buttons; ESP-IDF terminal via Ctrl+Shift+P). Equivalent `idf.py` commands work from any ESP-IDF terminal. Build system is CMake/Ninja, not Makefiles.
- Cycle count is the headline metric; time = cycles / MHz. Keep 240 MHz and -O2 fixed unless an experiment varies them on purpose.

## 10. Multi-machine and team notes

- Contributors may use Windows, macOS or Linux. Setup on any OS: install ESP-IDF v6.1 (VS Code + Espressif IDF extension, or the command-line installer), select target `esp32`, and select the serial port for your OS (see section 2). On macOS, install the Xcode command line tools first (`xcode-select --install`).
- **What does not sync through git** (per `.gitignore`):
  - `sdkconfig` (generated, machine- and version-specific). Recommended: keep shared settings in a tracked `sdkconfig.defaults` (create it with `idf.py save-defconfig` after menuconfig changes). Defaults only apply when no local `sdkconfig` exists, so delete the local `sdkconfig` after pulling changes to the defaults. Per-experiment overlays: `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.<experiment>" reconfigure build` (untested on ESP-IDF 6.1). Adoption status not confirmed.
  - `managed_components/` (downloaded components; local edits are lost, hence the IRAM patch caveat in section 5).
  - `.vscode/settings.json`, `c_cpp_properties.json`, `launch.json` (per-machine paths and serial port; regenerated by the extension).
- **Do commit** `dependencies.lock` and `main/idf_component.yml` so everyone resolves the same component versions.
- Line endings: recommended `core.autocrlf=false` plus a `.gitattributes` containing `* text=auto eol=lf` (then `git add --renormalize .` once) to avoid CRLF warnings and mixed endings across OSes.
- Record the key settings (clock, optimization level, complexity) next to every measurement in the results tables so numbers stay interpretable across machines and configs.