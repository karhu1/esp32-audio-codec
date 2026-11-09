# ESP32 Opus Benchmark

This ESP-IDF project benchmarks and optimizes Opus encoding on an original ESP32
(ESP-WROOM-32, Xtensa LX6). Complexity 0 is the active optimization target; higher
complexities remain comparison measurements.

## Documentation

- [AGENTS.md](AGENTS.md) is the source of truth for project settings, integration notes,
  measurement rules, current status, and handoff.
- [historical-benchmarks.md](historical-benchmarks.md) preserves benchmark runs and
  profiling output for historical comparison.

The Python plotting workflow and environment setup are described in AGENTS.md. The
plotter is `tools/plot_opus_benchmark.py`.
