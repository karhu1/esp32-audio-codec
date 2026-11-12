# Project facts and agent handoff

This file is the source of truth for shared project facts, working constraints, and the
current handoff. See [README.md](README.md) for a short project overview and
[historical-benchmarks.md](historical-benchmarks.md) for recorded run output.

## Project and active objective

This computer-architecture project measures and optimizes Opus encoding on an
ESP-WROOM-32 using the original ESP32's dual-core Xtensa LX6. The connected board is an
ESP32-D0WD-V3 revision 3.1, configured for 240 MHz, 4 MB flash, and no PSRAM. The project
uses ESP-IDF 6.1 and CMake/Ninja.

Complexity 0 is the sole codec optimization target. Complexities 2, 5, and 10 remain in
the sweep as comparison data; do not optimize against them or treat them as acceptance
criteria. Keep clock, compiler options, input signal, and codec settings fixed when
comparing changes. Record any intentional changes with the run.

The working goal: minimize cycles per frame at complexity 0 with a bit-identical output
hash, then find the lowest CPU clock that still meets the 20 ms frame deadline with margin.

Default benchmark settings are 240 MHz, `-O2`, fixed-point Opus, 16 kHz mono, 20 ms
frames (320 samples), VOIP application, forced voice signal, and 20 kbps VBR. The current
input is a deterministic synthetic speech-like signal. The benchmark measures encode
latency; it does not yet establish decode performance, energy/current, or perceptual
quality.

## Current handoff

Current baseline (complexity 0, 240 MHz, `-O2`, stage-profile hooks compiled in, SILK
encode path placed in IRAM): median of 10 repeated runs is 1,046,666 cycles per frame
(4,361 us), run-average range 1,046,656 to 1,046,677 cycles, slowest frame 1,208,071 cycles
(5,034 us, about 25 % of the 20 ms budget), hash 0x317FC6D7. The previous flash-resident
baseline was 2,032,014 cycles. Full tables are in
[historical-benchmarks.md](historical-benchmarks.md).

Two findings drive the plan:

1. Measurement is highly repeatable within a boot (spread under 0.03 %), but cycle counts
   differ by far more between builds. The cause is code placement and flash-cache behavior,
   not run-to-run noise.
2. Placing the SILK fixed-point encode path in IRAM (`components/opus/linker.lf`, about
   29 KB of code) cut complexity-0 cycles by 48.5 % with an unchanged bitstream. The most likely
   cause is instruction-fetch stalls in the flash cache (indirect evidence; no cache counters
   were measured). Other complexities improved less, in proportion to how much of their code
   was not moved.

The benchmark now includes `run_baseline_repeats()`, which repeats the complexity-0
measurement 10 times per boot and prints the median, range, standard deviation, worst frame,
and hash check. The marked JSON block and plotter are unchanged.

The active benchmark does not run two Opus encoders concurrently. A dual-encoder sweep was
attempted, but encoder creation failed from memory pressure after each core had retained its
60 KB non-thread-safe pseudostack allocation; the test also needs two encoder states and two
worker stacks at once. The latest failure and successful single-encoder handoff are recorded
in [historical-benchmarks.md](historical-benchmarks.md). Do not re-enable that sweep without
changing its memory plan. The handoff test only stages PCM on core 0 and runs the whole encode
on core 1; it does not split Opus stages across cores.

The planned real-input path is to transfer a WAV file from the PC over USB serial into device
memory, with Core 0 receiving PCM and Core 1 encoding it. This is deferred until the
optimization work below produces results. It will replace the synthetic signal, whose
per-frame cost follows its own amplitude envelope and may not match real speech.

Set up the plotter's Python environment from the project root. In Git Bash on Windows:

```bash
python -m venv .venv
source .venv/Scripts/activate
python -m pip install matplotlib
```

On macOS or Linux, use:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install matplotlib
```

The current monitor and plotting workflow is:

1. Activate the Python virtual environment with Matplotlib in an ESP-IDF terminal.
2. Run `python tools/plot_opus_benchmark.py`.
3. Press EN to run the flashed benchmark, then exit the monitor with Ctrl+] to save a
   dated plot under `out/`.

The script reads `idf.port` from the local `.vscode/settings.json` first, then falls back
to `ESPPORT`. It captures monitor output to `out/opus-monitor.log`; each generated PNG
has a date and time in its filename. Use `python tools/plot_opus_benchmark.py --plot-only`
to regenerate a plot from that saved log. Only one program can own the serial port at a
time; close other serial monitors before running it.

## Optimization decisions

- IRAM placement of the SILK encode path is retained. `components/opus/linker.lf` maps the
  fixed-point SILK encoder objects to IRAM with `noflash_text` (loaded through `LDFRAGMENTS`
  in `components/opus/CMakeLists.txt`). It cost about 29 KB of IRAM (free IRAM heap 87 KiB to
  59 KiB) and reduced the complexity-0 median from 2,032,014 to 1,046,666 cycles with the hash
  unchanged. The exact object list and per-stage results are in
  [historical-benchmarks.md](historical-benchmarks.md). The minimal necessary set of objects has
  not been determined yet.
- `OPUS_XTENSA_LX7` remains enabled in the Opus component. It selects an upstream Xtensa
  fixed-point path and was measured on this LX6 board; the macro name does not describe
  the board's core. The earlier recorded complexity-0 run (1,880,061 average / 2,118,621
  maximum cycles, baseline hash unchanged) was taken with the encode path running from flash.
  Re-test it against the IRAM baseline before treating its effect as established.
- `analysis.c` uses `sqrtf`, `logf`, and `log10f` in feature calculations. The recorded
  change was retained after hashes stayed the same and the complexity-5 comparison
  improved. Its recorded results are in [historical-benchmarks.md](historical-benchmarks.md).
- An Opus-only `-O3` experiment was slower than `-O2` at complexity 0 and was reverted.
  Keep the project at `-O2` while isolating source changes. That comparison was made while code
  ran from flash, so layout and cache effects may have contributed; repeat it on the IRAM build
  before drawing conclusions. The comparison is recorded in
  [historical-benchmarks.md](historical-benchmarks.md).
- Stage profiles for both the flash-resident and IRAM builds at complexity 0 are now recorded
  in [historical-benchmarks.md](historical-benchmarks.md). Older detailed profiles used
  complexity 10 and are historical diagnostics only. Profiling hooks can affect timing; they are
  still compiled in, so compare runs with equivalent instrumentation.

Full captured outputs and measurement tables are in
[historical-benchmarks.md](historical-benchmarks.md).

## Next work

Ordered by value and cost. Change one thing at a time and compare against the current
baseline median.

1. Find the minimal IRAM set. Rebuild with one object group removed from `linker.lf` at a time
   (pitch and noise-shape; the predictor chain; NSQ plus entropy and remaining objects) and
   record the cycle change and the IRAM saved. This shows which code is cache-hot and
   how much of the 59 KiB of remaining IRAM can be kept free.
2. Sweep the CPU clock (80, 160, 240 MHz) on the IRAM build at complexity 0. Record cycles per
   frame, time per frame, and worst-case margin. Projection to verify, not a result: at 1,208,071
   worst-case cycles, 80 MHz would take about 15.1 ms per frame (about 75 % of the budget) if
   cycles per frame stay constant. Remaining flash-resident constants may make cycles clock
   dependent. Power must still be measured separately.
3. Re-test `OPUS_XTENSA_LX7` (with and without) and `-O3` on the IRAM build, one at a time.
4. Profile-guided kernel work, using the current complexity-0 shares: predictor search about
   31 % (NLSF quantization, LPC/Burg, LTP), NSQ about 19 %, pitch analysis about 17 %. NSQ
   gained least from placement, so its remaining cost is likely real compute. Only the
   `SMULWB`/`SMLAWB`/`SMULWT`/`SMLAWT`, `CLZ32`, and `SAT16` operations have Xtensa-specific
   code so far; inner-loop kernels do not.
5. Try moving flash-resident constant tables to DRAM (`noflash_data`) as a separate experiment
   from code placement.
6. Memory work: re-add `DISABLE_FLOAT_API` and compare. It should remove the unused
   `TonalityAnalysisState` (14,004 bytes on the host build) and bring encoder state back near
   15 KB from 29,236 bytes, but it also removes the complexity-10 analysis path, so note the
   effect on the comparison data. Measure the pseudostack's real peak use and the benchmark task
   stack's real peak (about 3.7 KB of 24 KB used) before choosing sizes for a dual-encoder plan.
7. Check cache-counter availability for this exact original ESP32 target before claiming
   hit/miss counts. Until then, label placement results as indirect evidence.
8. Replace the synthetic input with a WAV file transferred over USB serial (see handoff).
9. Later: compare available codec implementations (including a floating-point build, since the
   ESP32 has a hardware single-precision FPU) and decode performance at complexity 0, and
   measure energy/current with a documented setup. If the system comparison proceeds,
   measure wired and wireless transmission latency and packet loss.

## Measurement rules

- The cycle counter brackets `opus_encode()`. Interrupts and system activity during that
  interval affect the result; logging and the output block occur outside the timed encode.
- Use the `run_baseline_repeats()` median as the comparison figure and the run-average range as
  the within-boot noise estimate. Because cycle counts also shift between builds, record
  the `linker.lf` contents, `idf.py size` output, and free IRAM with each run, and treat
  differences of a few percent between builds cautiously until a layout-sensitivity control
  (a no-op change that shifts code layout) has been measured.
- Bitstream hashes are regression checks, not proof of equal perceptual quality. If an
  optimization changes output, investigate it and evaluate audio quality before updating
  a reference hash. Code placement changes must leave all hashes unchanged.
- Heap capability pools can overlap. Do not add byte-addressable, executable, or other
  capability totals as if they were disjoint physical regions. Use `idf.py size` and its
  link-map data for static IRAM/DRAM use; runtime snapshots describe available heaps.
- Do not infer lower power from fewer cycles. Measure energy/current using a documented,
  repeatable setup.
- Verify cache-counter support for this exact original ESP32/ESP-IDF version before
  claiming hit/miss counts. If unavailable, label timing variation or code-placement
  comparisons as indirect evidence.
- Change one thing at a time and retain reverted experiment results so they are not
  repeated.

## Repository layout and integration constraints

- `components/opus/` is the vendored Xiph Opus source from upstream commit `677070ed`
  (2026-10-31), tracked directly without nested Git metadata. Its own README files are
  upstream documentation; keep them with the vendored source.
- `components/opus/CMakeLists.txt` builds Opus encoder/decoder sources directly for
  ESP-IDF, uses fixed-point math, and configures a 60 KB non-thread-safe scratch stack.
  The scratch setup is for this benchmark's single Opus-calling task; do not run Opus
  calls concurrently without changing the scratch allocation mode.
- `components/opus/linker.lf` places the SILK encode-path objects in IRAM and is loaded via
  `LDFRAGMENTS` in the component's `idf_component_register()` call. Entries use the archive
  name `libopus.a`. If an edit makes the link fail with an `iram0_0_seg` overflow, remove
  objects from the list.
- The component reads source groups from upstream `opus_sources.mk`, `celt_sources.mk`,
  and `silk_sources.mk`. Keep its local parser: Opus's generic CMake source-list helper
  assumes a source-tree working directory and fails during ESP-IDF dependency scanning.
- ESP-IDF evaluates a component during dependency scanning with `COMPONENT_LIB` as an
  interface target. Keep private `target_compile_definitions()` inside the
  `if(NOT CMAKE_BUILD_EARLY_EXPANSION)` guard.
- The Xtensa toolchain declares `lrint()` and `lrintf()` in `math.h`, but this ESP-IDF
  build does not run upstream configure checks. The component defines `HAVE_LRINT` and
  `HAVE_LRINTF` to avoid Opus's warning fallback, which ESP-IDF promotes to an error.
- `OPUS_BENCH_STAGE_PROFILE` is defined for the Opus component, and some calls to the
  profiling hooks in `opus_encoder.c` do not appear to be guarded by it. Removing the define may
  break the build; verify before turning profiling off.
- `sdkconfig.defaults` holds shared defaults such as 4 MB flash, 240 MHz, and performance
  optimization. Do not add removed Opus Kconfig settings; this local component has no
  Opus Kconfig menu. Fixed-point and scratch settings are compile definitions.
- GCC 15 at `-O3` warns about possible `stringop-overflow` in SILK decode loops. The
  temporary warning override used for that experiment was removed with `-O3`.

## Hardware and contributor notes

- UART flashing uses the board's CP210x USB-serial bridge. Typical names are `COMx` on
  Windows, `/dev/cu.usbserial-*` on macOS, and `/dev/ttyUSB*` on Linux. The current local
  VS Code setting is COM6; device settings are machine-specific and ignored by Git.
- If flashing reports `Wrong boot mode detected (0x13)`, hold BOOT, tap EN, then release
  BOOT after the chip is detected. Close the serial monitor before flashing.
- Press EN to rerun the flashed benchmark. A panic may reboot repeatedly; hold EN or
  unplug to stop it.
- Treat build logs and serial output as evidence and label assumptions. Put shared build
  defaults in tracked `sdkconfig.defaults`, not machine-specific `sdkconfig` or VS Code
  settings.
- The project uses CMake/Ninja, not Makefiles. Avoid spaces and very long paths for
  compatibility with ESP-IDF tools on Windows.