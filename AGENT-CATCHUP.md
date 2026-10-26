# Agent Catch-Up: ESP32 Opus Benchmarking Project

> Read this first. It summarizes everything done so far, what is verified vs. assumed,
> and what to do next. Last updated: 2026-10-25.

## 1. Goal

The user (a student) is doing a **computer architecture class project**: run a naive Opus
encoder on an ESP32, measure it, then **optimize Opus for the ESP32's Xtensa LX6 architecture
by editing Opus internals** (not just API parameters). Primary metric: **CPU cycles per
20 ms audio frame**. The user is new to C/ESP32/embedded work, so explain things plainly,
step by step, and avoid assuming prior knowledge.

## 2. Environment

| Item | Value |
|---|---|
| Board | ESP-WROOM-32 ESP32 dev board (3-pack, Amazon B08D5ZD528; likely HiLetgo-style, unconfirmed). Micro USB. |
| Chip | ESP32 dual-core Xtensa LX6, rev v3.1, 4 MB flash, **no PSRAM**, no microphone/codec |
| USB-serial chip | Unconfirmed (CP2102 or CH340); driver works, board appears as **COM6** |
| Host | Windows PC, MINGW64 shell, VS Code + Espressif IDF extension |
| ESP-IDF | **v6.1**, esptool v5.6.0 |
| Project | `C:\Users\karhu\repos\esp-idf\sample_project` (project name `sample_project`) |
| Flash method | UART (JTAG/DFU not applicable: no probe, original ESP32 has no native USB) |
| Agent sandbox | Cloud Linux container. **Cannot build/flash ESP-IDF and cannot reach the user's files.** The user pastes serial output and edits files themselves. Code written by the agent was only syntax-checked against stub headers. |

### Settings the user has applied (via menuconfig / sdkconfig)
- CPU frequency **240 MHz** (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240`). A stale build once kept printing 160; a rebuild/reflash fixed it.
- Flash size set to 4 MB (the boot log now shows `SPI Flash Size : 4MB`).
- Compiler optimization: **Performance (-O2)** (was Debug `-Og`).
- Recommended but **not confirmed**: panic handler = "Print registers and halt" (so crashes stop looping).

## 3. Flashing / operating quirks

- **Flash fails with `Wrong boot mode detected (0x13)`**: hold **BOOT**, tap **EN**, release BOOT once `Chip is ESP32...` appears. Needed on this board.
- Only one program can hold COM6 at a time. Close the serial monitor (**Ctrl+]**) before flashing.
- Program lives in flash and runs on power-up. **EN** = reset/rerun. A crash reboots in a loop by default; hold EN or unplug to stop.
- Keep project paths short, no spaces.
- Harmless warnings: esptool deprecation notices; `Detected size(4096k) larger than the size in the binary image header(2048k)` (before the flash size fix).

## 4. Files (in the project's `main/`)

| File | Purpose |
|---|---|
| `base_benchmark_main.c` | Stand-in harness: 440 Hz tone, int16 autocorrelation kernel (80 lags x 320 samples = 25,600 MACs), cycle timing with `esp_cpu_get_cycle_count()`. Original version printed CPU MHz from a compile-time macro (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`); suggested switching to `esp_rom_get_cpu_ticks_per_us()`. Superseded by the Opus benchmark. |
| `opus_benchmark_main.c` | **Current main.** Runs `opus_encode()` on a synthetic speech-like signal (30-harmonic wobbling-pitch tone + envelope + deterministic noise). 16 kHz mono, 20 ms frames (320 samples), 50 frames per pass, 1 untimed warm-up pass + 5 timed passes, `OPUS_APPLICATION_VOIP`, `OPUS_SIGNAL_VOICE`, 20 kbps, VBR on, complexity 0. Runs in its own task pinned to core 1 (stack currently 16 KB). Prints min/avg/max cycles, time, CPU load, mode counts (SILK/hybrid/CELT from the TOC byte), bytes/frame, **FNV-1a hash of all output bytes**, determinism check, encoder size, heap and stack stats. Runs once per reset. |

`main/CMakeLists.txt` must list the active source (`idf_component_register(SRCS "opus_benchmark_main.c" INCLUDE_DIRS ".")`).

## 5. Opus component and the IRAM bug (important)

- Added with `idf.py add-dependency "esphome/micro-opus^0.4.1"`. Reports `libopus 1.4-fixed`, so **fixed-point is enabled**. Alternative considered: `78/esp-opus` (thin upstream wrapper; fixed-point enabling not verified).
- micro-opus is **not stock Opus**: it adds Xtensa DSP instructions (MULSH, CLAMPS, NSAU, MULA.DD.*, ROUND.S; e.g. `silk_SAT16_lx7` appears in backtraces) and a thread-local "pseudostack" (default 120 KB, min 60 KB, Kconfig `OPUS_PSEUDOSTACK_SIZE`). Menu: `Component config -> Opus Audio Codec`. Exact values the user chose for pseudostack size/placement were not confirmed.
- **Bug in v0.4.1**: `patches/thread_local_stack.c` allocated the pseudostack with `MALLOC_CAP_INTERNAL` without `MALLOC_CAP_8BIT`. With no PSRAM and tight DRAM, the allocation can land in the **IRAM heap**, which only allows aligned 32-bit access, causing `LoadStoreError` (EXCCAUSE 3, EXCVADDR `0x4008A570`, in `silk_biquad_alt_stride1`) on the first `opus_encode()`. Fixed upstream in commit `4eef306` (#72, "Cleanup pseudostack allocation paths"), not in any tagged release as of 0.4.1.
- **Workaround applied by the user** in `managed_components/esphome__micro-opus/patches/thread_local_stack.c`: replaced the `#if defined(CONFIG_OPUS_PSEUDOSTACK_...)` block with
  `#define OPUS_ALLOC_SCRATCH(size) heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`.
  The old block was commented out with `//` lines ending in backslashes, which triggers a harmless `-Wcomment` "multi-line comment" warning; recommended wrapping it in `#if 0 ... #endif` or deleting it.
- **Fragile**: `managed_components/` can be overwritten by the component manager. After editing component sources, run **ESP-IDF: Full Clean**, because sources are staged into `build/esp-idf/esphome__micro-opus/opus-staged/`.
- Plan: copy the component into the project's `components/` folder (with `lib/opus` submodule, i.e. clone `https://github.com/esphome-libs/micro-opus` recursively) so edits persist and include the upstream fix.

## 6. Results so far

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
5. **Build a stock upstream baseline** (no Xtensa DSP asm) so the user can separate their gains from the component's.
6. **Optimize hotspots**, one change at a time, with a table row each: `-O3`, loop unrolling, `IRAM_ATTR`, MAC16/`MULA.DD` intrinsics or inline asm, hot-table placement, using both cores. Candidate files: `celt/pitch.c` (`celt_pitch_xcorr`), `celt/kiss_fft.c`, `celt/mdct.c`, SILK filters/NSQ, macros in `celt/arch.h`, `celt/fixed_generic.h`, `silk/xtensa/SigProc_FIX_lx7.h`.
7. **Correctness gate after every change**: output hash must match baseline; also validate against upstream test vectors / `opus_compare` when practical.
8. **Real audio input**: convert a clip with `ffmpeg -i in.wav -ar 16000 -ac 1 -f s16le -acodec pcm_s16le test.raw`, embed via `EMBED_FILES` or `xxd -i`, feed 320-sample frames; optionally add encode->decode round trip and print decoded PCM over serial to listen on the PC.
9. Optional: unroll / `-O3` experiments on the toy kernel (cheap, good for the report); JTAG probe only if a real debugger is needed (probably not).

## 9. Working agreements and gotchas for agents

- The user pastes serial output; confirm facts against it, and say plainly when something is unverified (menuconfig names, ESP-IDF 6.1 compatibility beyond what has built so far).
- Amazon blocks automated fetches; product details came from the title the user pasted.
- Give instructions for VS Code + Espressif extension (Build/Flash/Monitor buttons; ESP-IDF terminal via Ctrl+Shift+P), not Makefiles. Build system is CMake/Ninja via `idf.py`.
- Cycle count is the headline metric; time = cycles / MHz. Keep 240 MHz and -O2 fixed unless an experiment varies them on purpose.