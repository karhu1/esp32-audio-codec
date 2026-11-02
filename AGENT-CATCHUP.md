# Agent Catch-Up: ESP32 Opus Benchmark

> Current project state and next work. Updated 2026-11-01.

## Goal and setup

This is a computer architecture project measuring and optimizing Opus encoding on an
ESP32 by changing Opus internals. The goal is to make complexity-0 encoding as fast,
memory-efficient, and low-power as practical on this hardware.

- Board: ESP-WROOM-32 / original ESP32, dual-core Xtensa LX6, 4 MB flash, no PSRAM.
- ESP-IDF: v6.1. The project builds with the Espressif IDF extension or `idf.py` from
  an ESP-IDF terminal.
- Benchmark settings: 240 MHz, `-O2`, fixed-point Opus, 16 kHz mono, 320 samples/frame,
  VOIP application, voice signal, 20 kbps VBR. The historical sweep includes complexities
  0, 2, 5, and 10; the active optimization target is complexity 0 only.
- Keep the clock, compiler optimization, and benchmark settings fixed when comparing
  experiments. Record any intentional setting changes with the results.

## Project measurement direction

The broader project goal is to characterize and optimize audio compression for this
ESP32, then use measured bottlenecks to motivate software and hardware co-design ideas.
**Scope decision: complexity 0 is the sole codec-complexity optimization target.**
Complexities 2, 5, and 10 remain comparison measurements only. Do not optimize against
their timing or treat them as acceptance criteria. Keep them in benchmark output for
context, with complexity 0 as the target for speed, memory, and power work.

Use this experiment order so each result answers a clear question:

1. Establish a repeatable codec-only baseline. Compare the current Opus component with
   the available naive/general-purpose implementation and Micro-Opus baseline where
   feasible. Keep the input audio, codec settings, clock, compiler, and build options
   fixed; document any unavoidable differences. Measure encode and decode time per frame,
   memory use, energy per frame, and output behavior. Use complexity 0 as the fixed codec
   setting for optimization comparisons. Other settings are comparison data only.
2. Report memory by region (IRAM and DRAM; PSRAM only if present), CPU use per core, and
   energy/current under a documented measurement setup. Cycle counts and CPU load are not
   substitutes for power measurements.
   Check which performance/cache counters ESP-IDF exposes for this exact ESP32 target
   before promising cache hit/miss data. Counter support and meanings vary by chip. If
   direct cache counts are unavailable, record that limitation and use carefully labeled
   indirect evidence, such as timing variation and controlled code-placement experiments.
3. Compare single-core and multicore system designs. The dual-core ESP32 does not make
   one codec call faster by itself; evaluate a pipeline that assigns capture, encoding,
   and transmission tasks to cores. Keep the codec at complexity 0 and report both
   per-frame encode latency and end-to-end latency, plus per-core load.
4. Compare wired and wireless transmission with the same codec workload. Measure
   transmission and end-to-end latency, packet loss, and any buffering needed, so the
   bandwidth/reliability benefit can be weighed against CPU and latency costs.
5. Optimize only after profiling the chosen real-use configuration. Change one thing at
   a time, measure the same metrics, and verify output behavior. A matching bitstream
   hash is a regression check, not proof of identical perceptual quality; if output
   changes, evaluate audio quality and document the change.
6. Use measured bottlenecks to propose hardware/interface improvements (for example,
   more internal instruction memory, a faster core, or an accelerator). Label projected
   gains as estimates unless measured on that hardware.

For the current ESP32, IRAM placement is a candidate experiment only for profiled hot
code: most application code executes from flash through cache, while IRAM is limited and
its use trades against other internal memory. Likewise, do not assume cache counters are
available just because other ESP32-family chips expose them. The original ESP32 board has
no PSRAM, so report PSRAM as not applicable for current-board measurements.

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
- GCC 15 at `-O3` warns about possible `stringop-overflow` in `silk/decode_indices.c`.
  The affected loops use `nb_subfr`, which the decoder sets to 2 or 4 before decoding.
  The Opus target uses `-Wno-error=stringop-overflow` for this optimization experiment,
  so the diagnostic remains visible without stopping the build.
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

### Xtensa SILK optimization experiment

`OPUS_XTENSA_LX7` was enabled for the Opus component and verified in the ESP-IDF compile
database. The benchmark hashes remained unchanged. Compared with the baseline above,
worst-frame cycles fell about 10-15% across the sweep. The complexity-5 maximum was
13,950 us. The complexity-10 timing is retained only as historical data; complexity 10
is out of scope.

| Complexity | Avg cycles with Xtensa path | Max cycles with Xtensa path | Max time | Hash |
|---:|---:|---:|---:|---|
| 0 | 1,880,061 | 2,118,621 | 8,828 us | `0x317FC6D7` |
| 2 | 2,264,442 | 2,525,775 | 10,524 us | `0xE56B6272` |
| 5 | 3,081,739 | 3,347,961 | 13,950 us | `0x2550DE12` |
| 10 | 5,906,535 | 6,143,496 | 25,598 us | `0x8949A3CB` |

An Opus-only `-O3` experiment was also run after the Xtensa path was enabled. It made
performance worse, including at the in-scope complexity 0, so `-O3` and its warning
override have been removed; keep the component at the project-wide `-O2` while isolating
source-level optimizations.

The subsequent `-O3` run (not retained) had average/max cycles of 2,077,147/2,320,180
(complexity 0), 2,570,000/2,823,934 (2), 3,363,666/3,606,814 (5), and
6,495,293/6,739,441 (10), with unchanged hashes. This confirms `-O3` is not a useful
next step for this build.

## Historical optimization and profiling notes

The Xtensa SILK path improved cycles by 10-15% with unchanged hashes; complexity 5 had a
13.950 ms worst frame in that run. The following complexity-10 profiles are historical
diagnostics only. Do not use their apparent bottlenecks to set optimization priorities;
profile complexity 5 if additional stage data is needed.

### Historical complexity-10 stage profile (out of scope)

Temporary cycle profiling was added to `silk_encode_frame_FIX()` and the benchmark. The
latest hardware run (250 timed frames, hashes unchanged) measured 5,943,069 average total
cycles/frame at complexity 10. Within the instrumented frame, the largest stages were
NSQ (1,356,990 cycles/call), predictor search (1,037,077), noise-shape analysis (797,680),
and pitch analysis (452,521). The measured child stages add to about 3,813,286 cycles/frame,
leaving roughly 2,129,783 cycles/frame (36%) outside those stage timers. LBRR was negligible
(2,343 cycles/call). The reported “NSQ retries” label was corrected to “NSQ quantizer”;
the prior run recorded one NSQ call per frame, not an internal retry count.

The subsequent profile added inclusive totals around the complete fixed-point SILK frame and the
`silk_Encode()` API call, plus substage timers inside predictor analysis (LTP, LPC, NLSF
quantization, and residual energy). Parent and child counters overlap by design. Use the
new output to identify whether the unmeasured cycles are in frame-level control, SILK API
setup, or Opus work around SILK before choosing a source optimization.

The latest expanded hardware run kept the expected hashes. At complexity 10, the average
was 5,925,278 cycles/frame; `silk_Encode()` accounted for 4,081,712 cycles and the SILK
frame for 3,852,824 cycles. Thus roughly 1,843,566 cycles/frame (31%) remain outside the
timed SILK API call. Opus source inspection shows `opus_encode_native()` runs tonality
analysis for fixed-point builds at complexity 10 when the sample rate is 16-48 kHz and
the application is not restricted SILK; this benchmark meets those conditions. This is a
strong candidate for the unexplained cost, but it was not timed in that run. The current
instrumentation now separately times Opus tonality analysis and `opus_encode_frame_native()`
to verify it on the next hardware run.

The following run measured tonality analysis at 1,560,745 cycles/call (250 calls), about
26% of the 5,930,918-cycle average frame cost. `opus_encode_frame_native()` measured
4,239,780 cycles/call and contains the 4,080,036-cycle SILK API call. The combined timed
sections leave only about 130,393 cycles/frame outside them. The analysis is not safe to
remove without evaluating behavior: while forced `OPUS_SIGNAL_VOICE` makes its music
probability unused for voice estimation, the analysis also provides detected bandwidth
and activity probability used elsewhere in encoding. New temporary timers split each
`tonality_analysis()` frame from `tonality_get_info()` result aggregation; use those before
choosing a behavior-preserving optimization.

The latest run measured the per-frame tonality feature work at 1,534,699 cycles and result
aggregation at 21,707 cycles (250 calls each). Nearly all analysis time is therefore inside
`tonality_analysis()`, rather than `tonality_get_info()`. Its spectral-bin loop calls
`fast_atan2f()` twice for each of 239 bins, making that loop a plausible hot spot; profile
it separately from `opus_fft()` and the remaining feature/model work before changing math
that could affect analysis decisions or encoded output.

The latest subprofile run measured the FFT at 357,648 cycles, the phase/atan2 loop at
220,141, the neural model at 267,527, input preparation at 134,381, and post-phase features
at 547,118. These account for nearly all 1,537,563 feature-analysis cycles. The largest
section is post-phase feature work (about 36%); new counters split it into band statistics,
leakage, spectral variability, bandwidth detection, and DCT/feature preparation. No encoder
settings or codec decisions have been changed by the profiling edits.

The finer historical profile measured band statistics at 396,354 cycles/frame; leakage at 19,981,
spectral variability at 25,799, bandwidth detection at 29,511, and DCT/features at 76,710.
Complexity 10 was 5,932,888 average and 6,184,789 maximum cycles/frame with the expected
hash `0x8949A3CB`. A subsequent source change replaced double-precision `sqrt`/`log`/
`log10` calls in analysis feature calculations with `sqrtf`/`logf`/`log10f`; it was kept
after the benchmark hashes remained unchanged and complexity-5 performance improved.

For each optimization, change one thing at a time, rebuild, flash, and compare the
output hashes and cycles. A changed hash requires investigation; record a new correctness
reference only when a bitstream change is intentional.

## Next work

1. Use complexity 0 as the single codec optimization target and compare implementations
   at that setting. Keep complexities 2, 5, and 10 in the sweep as comparison measurements.
2. Make complexity-0 codec-only measurements repeatable and capture latency, memory by
   region, per-core use, energy/current, and output behavior. Verify ESP32 cache-counter
   availability before implementing or claiming cache metrics.
3. Add system-level single-core/multicore and wired/wireless comparisons after the
   codec-only baseline is stable.
4. Then optimize the measured bottlenecks one at a time, checking output behavior and
   audio quality whenever an optimization changes encoded output.
5. Use measured results to estimate and clearly label any proposed hardware improvements.

## Hardware / flashing notes

- UART flashing is used; the original ESP32 has no native USB. This board uses a CP210x
  USB-serial bridge. Port names are `COMx` on Windows, `/dev/cu.usbserial-*` on macOS,
  and `/dev/ttyUSB*` on Linux.
- Connected device: COM6; ESP32-D0WD-V3, revision v3.1; Wi-Fi, Bluetooth, dual core plus
  LP core; 240 MHz; 40 MHz crystal; eFuse Vref calibration; coding scheme None; MAC
  `4c:c3:82:ed:01:58`.
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
