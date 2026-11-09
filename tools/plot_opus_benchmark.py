#!/usr/bin/env python3
"""Parse the marked JSON block in an ESP-IDF monitor log and plot benchmark data."""

import argparse
from datetime import datetime
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


BEGIN = "===OPUS_BENCH_JSON_V1_BEGIN==="
END = "===OPUS_BENCH_JSON_V1_END==="
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
PROJECT_DIR = Path(__file__).resolve().parents[1]
OUTPUT_DIR = PROJECT_DIR / "out"
DEFAULT_LOG = OUTPUT_DIR / "opus-monitor.log"


def find_monitor_command():
    """Build a monitor command using ESP-IDF's own Python environment when available."""
    idf_path = os.environ.get("IDF_PATH")
    idf_python = os.environ.get("IDF_PYTHON_ENV_PATH")
    if idf_path and idf_python:
        python_name = "python.exe" if os.name == "nt" else "python"
        python_path = Path(idf_python) / ("Scripts" if os.name == "nt" else "bin") / python_name
        idf_script = Path(idf_path) / "tools" / "idf.py"
        if python_path.is_file() and idf_script.is_file():
            return [str(python_path), str(idf_script)]

    idf_command = shutil.which("idf.py")
    if idf_command:
        return [idf_command]
    return None


def configured_port():
    """Prefer this workspace's VS Code ESP-IDF port, then ESPPORT if configured."""
    settings_path = PROJECT_DIR / ".vscode" / "settings.json"
    try:
        settings = json.loads(settings_path.read_text(encoding="utf-8"))
        port = settings.get("idf.port")
        if isinstance(port, str) and port.strip():
            return port.strip()
    except (OSError, json.JSONDecodeError):
        pass
    return os.environ.get("ESPPORT") or None


def latest_block(log_text):
    """Return records from the last complete marked block in a monitor log."""
    records = []
    inside = False
    completed = []
    for raw_line in log_text.splitlines():
        line = ANSI.sub("", raw_line).strip()
        if BEGIN in line:
            records = []
            inside = True
            continue
        if END in line and inside:
            completed.append(records)
            inside = False
            continue
        if inside and line.startswith("{"):
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f"Invalid JSON inside benchmark block: {exc}") from exc
    if not completed:
        raise ValueError("No complete OPUS_BENCH_JSON_V1 block found in the log")
    block = completed[-1]
    run = next((item for item in block if item.get("type") == "run"), None)
    if not run or run.get("schema") != 1:
        raise ValueError("Benchmark block is missing a supported run record (schema 1)")
    return run, block


def capture_monitor(command, log_path):
    """Run the interactive monitor, teeing its live output to a predictable log."""
    monitor_env = os.environ.copy()
    monitor_env.setdefault("PYTHONUTF8", "1")
    try:
        process = subprocess.Popen(
            command,
            cwd=PROJECT_DIR,
            stdin=None,  # inherit the terminal so monitor keyboard shortcuts keep working
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=monitor_env,
        )
    except OSError as exc:
        print(f"error: could not start ESP-IDF monitor: {exc}", file=sys.stderr)
        return 2

    with log_path.open("wb") as log_file:
        assert process.stdout is not None
        while True:
            chunk = process.stdout.read1(4096)
            if not chunk:
                break
            log_file.write(chunk)
            log_file.flush()
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()

    return process.wait()


def monitor_connection_failed(log_path):
    """Detect serial-open failures even if idf.py exits with status zero."""
    try:
        output = log_path.read_text(encoding="utf-8", errors="replace").lower()
    except OSError:
        return False
    return "could not open port" in output or "connection to" in output and "failed" in output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plot-only", action="store_true",
                        help="plot the saved default log without starting the monitor")
    args = parser.parse_args()

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    if not args.plot_only:
        command = find_monitor_command()
        if not command:
            print("error: idf.py was not found; run this from an ESP-IDF terminal",
                  file=sys.stderr)
            return 2
        port = configured_port()
        monitor_command = [*command]
        if port:
            monitor_command.extend(["-p", port])
            print(f"Using serial port {port}")
        else:
            print("No port is set in .vscode/settings.json or ESPPORT; using ESP-IDF default port.")
        print(f"Starting ESP-IDF monitor; results will be captured in {DEFAULT_LOG}")
        print("Press EN on the board to run the benchmark. Exit the monitor with Ctrl+] to plot.")
        monitor_command.append("monitor")
        monitor_status = capture_monitor(monitor_command, DEFAULT_LOG)
        if monitor_connection_failed(DEFAULT_LOG):
            print(f"error: could not open {port or 'the configured serial port'}. "
                  "Close any other serial monitor or terminal using the board, then retry.",
                  file=sys.stderr)
            return 2
        if monitor_status != 0:
            print(f"error: ESP-IDF monitor exited with status {monitor_status}",
                  file=sys.stderr)
            return monitor_status

    try:
        # Keep Matplotlib's cache in the project output directory, which is writable.
        os.environ.setdefault("MPLCONFIGDIR", str(OUTPUT_DIR / "matplotlib"))
        run, records = latest_block(DEFAULT_LOG.read_text(encoding="utf-8", errors="replace"))
        import matplotlib.pyplot as plt
    except ImportError:
        print("Plotting needs matplotlib. Install it with: python -m pip install matplotlib",
              file=sys.stderr)
        return 2
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    summaries = [r for r in records if r.get("type") == "summary" and r.get("ok")]
    frames = sorted((r for r in records if r.get("type") == "frame"),
                    key=lambda r: r["frame"])
    memory = [r for r in records if r.get("type") == "memory" and r.get("complexity") == 0]
    if not summaries:
        print("error: benchmark block contains no successful summary records", file=sys.stderr)
        return 2
    c0_summary = next((r for r in summaries if r["complexity"] == 0), None)
    if c0_summary and frames:
        frame_mean = sum(r["avg_cycles"] for r in frames) / len(frames)
        summary_mean = c0_summary["avg_cycles"]
        if abs(frame_mean - summary_mean) > max(2, summary_mean * 0.01):
            print("error: complexity-0 per-frame timings do not match the summary; "
                  "rebuild and flash the updated benchmark firmware, then capture a new log",
                  file=sys.stderr)
            return 2

    cpu_mhz = run["cpu_mhz"]
    complexities = [r["complexity"] for r in summaries]
    avg_ms = [r["avg_cycles"] / cpu_mhz / 1000 for r in summaries]
    max_ms = [r["max_cycles"] / cpu_mhz / 1000 for r in summaries]
    fig, axes = plt.subplots(5, 1, figsize=(10, 16), constrained_layout=True,
                             gridspec_kw={"height_ratios": [1, 1, 0.8, 0.8, 0.7]})
    summary_ax, frame_ax, dram_ax, exec_ax, stack_ax = axes

    x = list(range(len(complexities)))
    width = 0.36
    summary_ax.bar([i - width / 2 for i in x], avg_ms, width, label="Average")
    summary_ax.bar([i + width / 2 for i in x], max_ms, width, label="Maximum")
    summary_ax.axhline(run["frame_ms"], color="red", linestyle="--", label="Frame budget")
    summary_ax.set_xticks(x, [str(c) for c in complexities])
    summary_ax.set_xlabel("Opus complexity")
    summary_ax.set_ylabel("Encode time (ms/frame)")
    summary_ax.set_title("Opus encode latency by complexity")
    summary_ax.legend()
    summary_ax.grid(axis="y", alpha=0.25)

    if frames:
        frame_ax.plot([r["frame"] for r in frames], [r["avg_us"] / 1000 for r in frames],
                      marker=".", linewidth=1, label="Complexity 0 average")
        frame_ax.axhline(run["frame_ms"], color="red", linestyle="--", label="Frame budget")
        frame_ax.set_xlabel("Input frame")
        frame_ax.set_ylabel("Encode time (ms)")
        frame_ax.set_title("Per-frame latency (complexity 0)")
        frame_ax.legend()
        frame_ax.grid(alpha=0.25)
    else:
        frame_ax.text(0.5, 0.5, "No per-frame records in this run", ha="center", va="center")
        frame_ax.set_axis_off()

    stage_order = ["before_create", "after_create", "after_encode", "after_destroy"]
    stage_labels = ["Before create", "After create", "After encode", "After destroy"]
    memory_by_stage = {r["stage"]: r for r in memory}
    available_stages = [stage for stage in stage_order if stage in memory_by_stage]
    if available_stages:
        labels = [stage_labels[stage_order.index(stage)] for stage in available_stages]
        x_memory = list(range(len(available_stages)))
        for ax, free_key, largest_key, title in (
            (dram_ax, "dram_8bit_internal_free_bytes",
             "dram_8bit_internal_largest_block_bytes", "Internal byte-addressable heap"),
            (exec_ax, "executable_internal_free_bytes",
             "executable_internal_largest_block_bytes", "Internal executable heap (IRAM)"),
        ):
            ax.plot(x_memory, [memory_by_stage[s][free_key] / 1024 for s in available_stages],
                    marker="o", label="Total free")
            ax.plot(x_memory, [memory_by_stage[s][largest_key] / 1024 for s in available_stages],
                    marker="s", linestyle="--", label="Largest free block")
            ax.set_xticks(x_memory, labels)
            ax.set_ylabel("Free memory (KiB)")
            ax.set_title(title)
            ax.legend()
            ax.grid(axis="y", alpha=0.25)
    else:
        for ax in (dram_ax, exec_ax):
            ax.text(0.5, 0.5, "No memory snapshots in this firmware log", ha="center", va="center")
            ax.set_axis_off()

    if available_stages and "benchmark_task_stack_bytes" in run:
        stack_size = run["benchmark_task_stack_bytes"]
        stack_used = [stack_size - memory_by_stage[s]["task_stack_unused_bytes"]
                      for s in available_stages]
        stack_ax.plot(range(len(available_stages)), stack_used, marker="o",
                      label="High-water stack used")
        stack_ax.set_xticks(range(len(available_stages)),
                            [stage_labels[stage_order.index(s)] for s in available_stages])
        stack_ax.set_ylabel("Stack used (bytes)")
        stack_ax.set_title(f"Benchmark task stack (allocated {stack_size} bytes)")
        stack_ax.legend()
        stack_ax.grid(axis="y", alpha=0.25)
    else:
        stack_ax.text(0.5, 0.5, "No task-stack snapshots in this firmware log",
                      ha="center", va="center")
        stack_ax.set_axis_off()

    fig.suptitle(f"ESP32 Opus benchmark - {run['opus_version']} - {run['optimization']}")
    plot_path = OUTPUT_DIR / f"opus-benchmark-{datetime.now():%Y%m%d-%H%M%S}.png"
    fig.savefig(plot_path, dpi=160)
    print(f"Saved {plot_path} using the latest complete benchmark block in {DEFAULT_LOG}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
