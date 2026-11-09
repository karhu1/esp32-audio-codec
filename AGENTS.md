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

Default benchmark settings are 240 MHz, `-O2`, fixed-point Opus, 16 kHz mono, 20 ms
frames (320 samples), VOIP application, forced voice signal, and 20 kbps VBR. The current
input is a deterministic synthetic speech-like signal. The benchmark measures encode
latency; it does not yet establish decode performance, energy/current, or perceptual
quality.

## Current handoff

The current source adds complexity-0 per-frame preservation and runtime memory snapshots
to the marked JSON output. Memory snapshots cover before encoder creation, after creation,
after encoding, and after destruction. They record byte-addressable internal heap, the
executable internal heap, largest free blocks, and task stack high-water usage. The plot
shows latency, complexity-0 per-frame latency, those heap snapshots, and stack use.

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

- `OPUS_XTENSA_LX7` remains enabled in the Opus component. It selects an upstream Xtensa
  fixed-point path and was measured on this LX6 board; the macro name does not describe
  the board's core. The recorded complexity-0 run was 1,880,061 average / 2,118,621
  maximum cycles, with the baseline hash unchanged.
- `analysis.c` uses `sqrtf`, `logf`, and `log10f` in feature calculations. The recorded
  change was retained after hashes stayed the same and the complexity-5 comparison
  improved. Its recorded results are in [historical-benchmarks.md](historical-benchmarks.md).
- An Opus-only `-O3` experiment was slower than `-O2` at complexity 0 and was reverted.
  Keep the project at `-O2` while isolating source changes. The comparison is recorded in
  [historical-benchmarks.md](historical-benchmarks.md).
- Historical detailed stage profiles primarily used complexity 10. Treat them as
  historical diagnostics, not as evidence for complexity-0 optimization priorities.
  Profiling hooks can affect timing; compare runs with equivalent instrumentation.

Full captured outputs and measurement tables are in
[historical-benchmarks.md](historical-benchmarks.md).

## Next work

1. Build and flash the current source, capture a fresh run, and confirm the complexity-0
   JSON, memory snapshots, and dated plot agree.
2. Repeat the complexity-0 run under fixed conditions to establish measurement variation.
   Use `idf.py size` alongside runtime heap snapshots to account for static IRAM/DRAM use.
3. Profile complexity 0 before choosing a memory/code-placement experiment. Consider IRAM
   placement only for a measured hot path, then compare latency and memory headroom.
4. Check cache-counter availability for this exact original ESP32 target before promising
   cache hit/miss measurements.
5. Later, compare available codec implementations and decode performance at complexity 0,
   and measure energy/current with a documented setup. If the system comparison proceeds,
   measure wired and wireless transmission latency and packet loss.

## Measurement rules

- The cycle counter brackets `opus_encode()`. Interrupts and system activity during that
  interval affect the result; logging and the output block occur outside the timed encode.
- Bitstream hashes are regression checks, not proof of equal perceptual quality. If an
  optimization changes output, investigate it and evaluate audio quality before updating
  a reference hash.
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
- The component reads source groups from upstream `opus_sources.mk`, `celt_sources.mk`,
  and `silk_sources.mk`. Keep its local parser: Opus's generic CMake source-list helper
  assumes a source-tree working directory and fails during ESP-IDF dependency scanning.
- ESP-IDF evaluates a component during dependency scanning with `COMPONENT_LIB` as an
  interface target. Keep private `target_compile_definitions()` inside the
  `if(NOT CMAKE_BUILD_EARLY_EXPANSION)` guard.
- The Xtensa toolchain declares `lrint()` and `lrintf()` in `math.h`, but this ESP-IDF
  build does not run upstream configure checks. The component defines `HAVE_LRINT` and
  `HAVE_LRINTF` to avoid Opus's warning fallback, which ESP-IDF promotes to an error.
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
