# Opus optimization log

This file records experiments on the vendored Opus component for the original ESP32
(ESP-WROOM-32, dual-core Xtensa LX6). Unless an entry says otherwise, results use 240 MHz,
fixed-point Opus, `-O2`, 16 kHz mono, 20 ms frames, 20 kbps VBR, VOIP/voice.

## Scope and objective

**Complexity 0 is the sole active optimization target.** The goal is to make the codec as
fast, memory-efficient, and low-power as practical on this ESP32 while preserving
acceptable audio quality. Complexities 2, 5, and 10 stay in the benchmark as comparison
measurements only; do not optimize against their results or treat them as acceptance
criteria.

The benchmark currently records cycles and frame latency, plus some heap/state figures.
It does not directly measure energy or power. Do not infer lower power from fewer cycles
alone: energy/current must be measured under a documented, repeatable board and workload
setup. Bitstream hashes are regression checks, not proof of equal perceptual audio quality.
Profiling builds add measurement overhead, so compare timing runs made with equivalent
instrumentation and settings.

## Applied optimization experiments

### Xtensa SILK implementation path

- **Change:** Enabled `OPUS_XTENSA_LX7` for the Opus component, selecting upstream Xtensa
  fixed-point routines. The macro name says LX7 although this board's CPU is LX6; the
  configuration was built and measured on this ESP32.
- **Complexity-0 result:** 1,880,061 average / 2,118,621 maximum cycles per frame,
  8,828 us maximum, hash `0x317FC6D7`.
- **Assessment:** The recorded comparison showed approximately 10-15% fewer worst-frame
  cycles across the sweep with hashes unchanged. The macro remains enabled in
  `components/opus/CMakeLists.txt`. Other complexity values are comparative data only.

### Single-precision analysis math

- **Change:** Replaced analysis-feature calls to double-precision `sqrt`, `log`, and
  `log10` with `sqrtf`, `logf`, and `log10f` in `components/opus/src/analysis.c`.
- **Why:** These values are used as floats; avoiding double-precision library routines is
  a targeted way to reduce work on this fixed-point ESP32 build.
- **Recorded result:** The latest complexity-0 run measured 1,919,814 average /
  2,174,510 maximum cycles (9,060 us maximum), with expected hash `0x317FC6D7`.
  Complexity-5 and complexity-10 before/after figures are preserved in `benchmarks.md`
  as comparison data, not optimization criteria.
- **Assessment:** Kept in the source. Hash checks are not audio quality evaluations.

## Experiments not retained

### Opus-only `-O3`

- **Change tested:** Compiled the Opus component with `-O3` while retaining the Xtensa
  path.
- **Complexity-0 result:** 2,077,147 average / 2,320,180 maximum cycles at `-O3`, compared
  with 1,880,061 / 2,118,621 at `-O2`; hashes were unchanged.
- **Decision:** Reverted `-O3` and its warning override because it was slower. Keep the
  project at `-O2` while evaluating source-level changes.

## Profiling work (measurement instrumentation, not codec optimizations)

Temporary cycle counters in the benchmark and Opus internals measured SILK pitch and
noise-shape analysis, predictor search and sub-stages, NSQ, entropy encoding, SILK API/frame
processing, Opus frame processing, and tonality-analysis sub-stages. The initial detailed
profiles used complexity 10 and are historical exploratory data only. They identified
expensive work in tonality analysis and SILK and motivated the single-precision math test.
For future bottleneck work, profile complexity 0, the in-scope setting.

Profiling hooks are compiled behind `OPUS_BENCH_STAGE_PROFILE` and the benchmark prints
their stage data. Some counters are inclusive; do not add parent and child totals together.
Account for instrumentation overhead when comparing with unprofiled timings. Disable or
remove these hooks for final production performance measurements unless profiling itself
is under study.

## Current in-scope reference (complexity 0)

The latest recorded `-O2` run with the Xtensa path and single-precision analysis math:

| Complexity | Average cycles | Maximum cycles | Maximum time | Hash |
|---:|---:|---:|---:|---|
| **0 (target)** | **1,919,814** | **2,174,510** | **9,060 us** | **`0x317FC6D7`** |

The run used 29,236 bytes of encoder state, with 29,700 bytes used by encoder creation.
The benchmark's heap readings are not a complete per-region memory profile. Power and
energy have not yet been measured. See `benchmarks.md` for full logs; complexities 2, 5,
and 10 are comparison data only.

## Experiment practice

Change one thing at a time. Keep complexity 0 and the benchmark input/settings fixed for
optimization comparisons. Record source/configuration changes, compiler settings, board,
latency, memory by region when available, power/energy measurement method and results,
per-core load for system tests, output behavior, and any audio-quality evaluation. Treat
changed hashes as a prompt to investigate, not automatically as a failure or proof of
quality loss. Keep reverted experiments here with their results so they are not repeated.
