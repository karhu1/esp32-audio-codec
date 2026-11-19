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
regression input is a deterministic synthetic speech-like signal. The benchmark measures encode
latency; it does not yet establish decode performance, energy/current, or perceptual
quality.

## Current handoff

Current baseline (complexity 0, 240 MHz, `-O2`, stage-profile hooks compiled in, SILK
encode path placed in IRAM): median of 10 repeated runs is 1,046,666 cycles per frame
(4,361 us), run-average range 1,046,656 to 1,046,677 cycles, slowest frame 1,208,071 cycles
(5,034 us, about 25 % of the 20 ms budget), hash 0x317FC6D7. The previous flash-resident
baseline was 2,032,014 cycles. Full tables are in
[historical-benchmarks.md](historical-benchmarks.md).

Three findings drive the plan:

1. Measurement is highly repeatable within a boot (spread under 0.03 %), but cycle counts
   differ more between builds. The large 2026-11-11 difference came from code placement and
   flash-cache behavior, not run-to-run noise. Smaller shifts also occur: across three stream
   firmware revisions, average cycles on the same input dropped 1.4 % although the encoder code
   did not change (see "Measurement rules").
2. Placing the SILK fixed-point encode path in IRAM (`components/opus/linker.lf`, about
   29 KB of code) cut complexity-0 cycles by 48.5 % with an unchanged bitstream. The most likely
   cause is instruction-fetch stalls in the flash cache (indirect evidence; no cache counters
   were measured). Other complexities improved less, in proportion to how much of their code
   was not moved.
3. Real audio streamed through the stream build (2026-11-19, recorded in
   [historical-benchmarks.md](historical-benchmarks.md)) averages within about 3 % to 5 % of the
   synthetic baseline, but its slowest frames are 11 % to 13 % higher: 1,362,819 cycles (5.68 ms,
   28.4 % of the 20 ms budget) on a 185 s conversation, against 1,208,071 on the synthetic
   signal. Use the real-input worst case when judging deadline margin and lower clock speeds.

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

The real-input path is a separate firmware mode that streams an MP3 (decoded on the PC) over
USB serial, with core 0 handling the UART and core 1 encoding, and writes a playable Ogg Opus
file on the PC. It works and its results are recorded; the script's default link speed is now
460800 baud because 921600 fails intermittently (cause not identified). See "USB streaming
mode" below for the design, status, and open issues. The streamed numbers are real-input data,
not a regression baseline; the synthetic benchmark remains the baseline for comparing changes.

## USB streaming mode (stream build): working at 460800 baud; 921600 intermittent

Status (2026-11-19): built, flashed, and run on three inputs. Results, firmware revisions, and
failed runs are in the 2026-11-19 entry of [historical-benchmarks.md](historical-benchmarks.md).
Summary:

- Every run recorded as a pass matched the board's frame count, byte count, and FNV-1a hash on
  the PC. For the 185 s conversation file the output hash (0xD18954DF) and byte count (384,499)
  were identical across all ten passes, both baud rates, and all three firmware revisions.
- Throughput: about 1.28x real time at 460800 baud and 1.91x at 921600 (a 185 s file takes about
  145 s and 97 s).
- Reliability: at 921600 the conversation file failed in all 3 runs on the original firmware and
  in 2 of 7 runs on the later revisions; at 460800 it passed 5 of 5. The default is now 460800.
  The cause is open; see "Known limitations and risks".
- Cycle statistics (core 1, 240 MHz): song average 1,099,033, slowest 1,345,941; conversation
  average 1,066,973 to 1,082,988, slowest 1,342,766 to 1,362,819; 5 s clip average 1,016,967,
  slowest 1,249,578. Fastest frames are about 820,000 to 836,000 on all inputs.
- Not yet recorded: memory use (heap, stack high-water), sequence wraparound, listening results
  for the conversation and song outputs. The 5 s clip output was reported to play.

Purpose: feed real speech/music to the encoder and return the Opus packets to the PC, using both
cores in a producer-consumer arrangement, and record per-frame encode cycles on real audio.

Files:

- `main/stream_codec.c`, `main/stream_codec.h`: board side. The header documents the wire
  protocol; keep it and `tools/stream_mp3.py` in sync. **The header has not yet been updated for
  the BYE frame, the post-DONE grace period, and the END resend described below; do that next.**
- `main/stream_main.c`: `app_main()` for the stream build; calls `stream_codec_start()`.
- `main/CMakeLists.txt`: selects the firmware by CMake variable `APP_MODE`.
  `idf.py -DAPP_MODE=stream build flash` builds stream mode (`stream_main.c` +
  `stream_codec.c`); `idf.py -DAPP_MODE=benchmark build flash` (the default) builds the
  unchanged `opus_benchmark_main.c`. CMake caches `APP_MODE`, so pass it explicitly when
  switching back. The benchmark source is not modified by stream mode.
- `tools/stream_mp3.py`: PC side. Needs `pip install pyserial miniaudio`. Accepts only `.mp3`;
  decodes and resamples to 16 kHz mono 16-bit with `miniaudio`, pads the end with silence to whole
  20 ms frames, streams, and verifies integrity. Port comes from `--port`, then `idf.port` in
  `.vscode/settings.json`, then `ESPPORT`. Link speed comes from `--baud` (default 460800; pass
  `--baud 921600` for the faster, less reliable setting). Close any serial monitor first.

Design (core assignment follows the existing benchmark: encoder on core 1):

- `rx_task` (core 0, priority 6): reads UART0, parses frames, validates CRC and sequence, queues
  PCM jobs, and handles the post-DONE grace period. `tx_task` (core 0, priority 5): sends
  READY/PACKET/DONE/ERROR frames. `enc_task` (core 1, priority 5, 16 KB stack): the only task
  that calls `opus_*()`.
- Hand-off uses two FreeRTOS message buffers, each with exactly one writer and one reader
  (`g_in`: rx to enc; `g_out`: enc to tx). RX-side errors are sent directly through the
  mutex-protected `send_frame()` and never through `g_out`, to keep a single writer. Do not add a
  second writer to either buffer without adding locking.
- Wire format: `0xA5 0x5A | type | seq u16 | len u16 | payload | crc16` (little-endian,
  CRC-16/CCITT-FALSE over type..payload). Types: HELLO 0x01, PCM 0x02, END 0x03, BYE 0x04 (PC to
  board); READY 0x81, PACKET 0x82, DONE 0x83, ERROR 0x8F (board to PC). PACKET carries the core-1
  encode cycle count before the Opus bytes. DONE carries frames, total bytes, FNV-1a hash of all
  packet bytes, and min/avg/max encode cycles. BYE has an empty payload and means "DONE
  received". Layouts for the original types are in `stream_codec.h`.
- Link: handshake at 115200 (HELLO, READY), then both sides switch to the baud rate carried in
  HELLO (the board accepts 115200 to 1,000,000; the CP2102 limit is 1,000,000). Wire time for one
  649-byte PCM frame is about 14.1 ms at 460800 and 7.0 ms at 921600. Measured wall time per frame
  is about 15.6 ms at 460800 (about 1.5 ms above wire time, link-limited) and about 10.4 ms at
  921600, about 3.4 ms above wire time; see "Known limitations and risks". The board returns to 115200
  after BYE, after a 6 s grace period following DONE, or after any error, so the normal monitor
  works again.
- End of stream: after sending all PCM frames the PC sends END. If all packets have arrived but
  DONE has not after 1 s, the PC resends END, at most 4 times, then fails. After DONE the board
  stays at the high baud rate for `DONE_GRACE_US` (6 s), answers a repeated END with DONE again,
  and leaves the grace period on BYE or on timeout. The PC sends BYE twice after receiving DONE.
  The resend path has not been exercised by any recorded run (no pass printed an `END resends`
  line). Keep the board's grace period longer than the PC's resend window (about 5 s).
- Flow control: software credit window of 8 frames (`STREAM_WINDOW`). The PC may have at most 8
  PCM frames sent but not answered; each PACKET returns one credit. The board has no RTS/CTS
  wired. Buffers are sized for window plus 2 messages, so they should never overflow; overflow,
  bad CRC, wrong sequence, wrong PCM length, a frame cut short for over 100 ms during a session,
  and a 5 s receive timeout all abort the session with an ERROR frame. For a CRC error the detail
  field holds the expected PCM sequence number; for a truncated frame it is `0x40000000` ORed with
  that number. The UART driver's RX ring is 8,192 bytes, larger than the 8-frame window
  (about 5,200 bytes). A lost PCM or PACKET mid-stream still aborts the run; only a lost DONE or
  END is recovered.
- The PC sends as fast as credits allow (no real-time pacing), because the goal is speed. The PC
  gives up after 12 s without any frame (`STALL_TIMEOUT_S`), longer than the board's 5 s session
  timeout so the board's own error arrives first, and prints frames sent, packets received, and
  whether END was sent when it fails.
- Encoder settings match the benchmark by default: 16 kHz mono, VOIP, forced voice, VBR,
  20 kbps, complexity 0. HELLO can override bitrate (6,000 to 510,000) and complexity (0 to 10).
  Complexity 10 does not meet real time.
- While a session is active, UART0 (also the console) carries binary data. Nothing in the session
  path may call `printf` or log; log level is set to none from READY until the session ends.
  The "stream done" summary prints after the board returns to 115200.
- Core 1 never blocks while frames are queued, so the idle task on core 1 would starve and trip
  the task watchdog. `enc_task` calls `vTaskDelay(1)` every `YIELD_EVERY_FRAMES` (200) frames.
  Lower it if "Task watchdog got triggered" appears. Each yield costs up to one tick (10 ms at the
  default 100 Hz tick), about 1 % of throughput at the current value.
- Output on the PC (`out/`): `<name>.opus` (playable Ogg Opus, mono; play with VLC or
  `ffplay out/<name>.opus`), `<name>.opkt` (12-byte header `OPKT`, version 1, sample rate, frame
  samples, reserved; then repeated u16 packet length and packet bytes) and
  `<name>.cycles.csv` (frame, bytes, encode cycles).
- Ogg Opus details: OpusHead pre-skip is 312 (encoder lookahead of 104 samples at 16 kHz, from
  `OPUS_GET_LOOKAHEAD` in `opus_encoder.c`, expressed at 48 kHz); granule positions count 48 kHz
  samples (960 per 20 ms frame); the final granule trims the silence padded onto the last frame.
  The encoder is not flushed, so up to about 6.5 ms at the end may be missing. Players report
  48 kHz; the 16 kHz input rate is stored in the header. Ogg CRC is poly 0x04C11DB7, init 0, no
  reflection, no final XOR.

Original checklist, with status:

1. Build: passed. The firmware builds and runs under ESP-IDF 6.1 with the console UART driver
   installed.
2. Handshake: passed at 921600 and 460800 on this CP2102 board.
3. Integrity: passed in every recorded pass (frames, bytes, and hash match between board and PC).
4. Watchdog: partly verified. Runs of 169.7 s and 185.3 s completed. The console is not visible
   during a session, so a non-fatal watchdog message would not have been seen.
5. Throughput and cycle statistics: recorded (see the status summary above).
6. Memory: not yet measured. Record `heap_caps_get_free_size` and stack high-water for the three
   stream tasks. The encoder task stack (16 KB) and the 60 KB pseudostack on core 1 are carried
   over from the benchmark.

Known limitations and risks:

- **921600 baud is unreliable (open).** Five failures were recorded on the conversation file, all
  at 921600: a board-side CRC error near frame 7,100; three runs that lost DONE (one also lost
  the last packet); and one out-of-order packet (seq 7148 received, 7147 expected) in the
  same 7,100 to about 7,160 range as the CRC error. The board reported no error for the last of these,
  which suggests loss or corruption on the return path; this is an inference. The cause is not
  isolated between the CP2102, the cable, the host USB stack, and ESP32 UART interrupt latency.
  Ideas not yet tried: count UART events (FIFO overflow, buffer full, framing, parity) through the
  driver's event queue; lower the RX full threshold with `uart_set_rx_full_threshold`; enable
  `CONFIG_UART_ISR_IN_IRAM` (confirm these names and defaults in ESP-IDF 6.1 first); swap the
  cable and port; try 576000; have the PC count and report frames dropped by its parser; check
  whether frames 7,100 to about 7,160 of the conversation file are unusual. Until then, use 460800.
- **921600 throughput is lower than wire time predicts (open).** The 649-byte PCM frame takes
  7.0 ms on the wire and the encode takes about 4.5 ms, which should overlap through the 8-frame
  window, yet the measured time is 10.4 ms per frame (about 3.4 ms extra). Halving the wire time
  from 460800 also did not halve the measured time (15.6 ms became 10.4 ms, not 7.8 ms). The
  cause is not isolated. Candidates, none measured: CP2102 or Windows driver latency, the PC
  script's per-frame write, read, and CRC handling in Python, and credit-window idle time. The
  board is not the limit (encode is 4.5 ms of 10.4 ms). It may or may not be related to the
  921600 reliability problem above. To investigate: timestamp each PC write and each parsed
  PACKET, try a larger `STREAM_WINDOW` (buffers are sized from it), and check the CP2102 latency
  timer setting. Not needed for the cycle benchmark, since core 1 times only `opus_encode()`.
- Sequence numbers are 16 bit and wrap after 65,536 frames (about 21.8 minutes); the protocol is
  written to tolerate wrap, but this is unexercised (the longest run is 9,264 frames).
- A stale PCM job from an aborted session may be encoded by core 1 before the next START is
  processed; `tx_task` drops the result because the session is inactive. A new HELLO arriving
  within a few milliseconds of an abort could in principle race this; the script waits far longer.
- If both BYE frames are lost, the board stays at the high baud rate for up to 6 s and ignores a
  new HELLO; the script retries HELLO for 15 s, so it should recover.
- The MP3 is decoded and resampled by `miniaudio` on the PC, so any audible difference from other
  decoders is outside this project's measurements.
- The same encoder, IRAM placement (`linker.lf`), and 240 MHz setting apply as in the benchmark
  build; results are therefore comparable in code placement but not in input. `idf.py size` and
  free IRAM were not recorded for the stream builds.

Follow-ups:

- Update `stream_codec.h` for BYE, the grace period, and the END resend (see "Files").
- Listen to the conversation and song `.opus` outputs and note the quality; optionally add a
  PC-side decode check against the source.
- Measure stream-task memory (checklist item 6).
- Decide whether to chase the 921600 failures (ideas under "Known limitations and risks"). The
  link, not the encoder, limits throughput at both speeds, so 460800 at 1.28x real time is enough
  for the current goals.
- Find the extra 3.4 ms per frame at 921600 (see "Known limitations and risks"), starting with
  PC-side timestamps and a larger window, only if faster test runs are wanted.
- Compare more real-speech clips and their per-frame cycle profiles (`<name>.cycles.csv`) against
  the synthetic signal. The song is not speech, and only one conversation has been recorded.

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
   frame, time per frame, and worst-case margin. Projection to verify, not a result: at the
   1,362,819-cycle slowest frame seen on real conversation audio, 80 MHz would take about 17.0 ms
   per frame (about 85 % of the budget) and 160 MHz about 8.5 ms (43 %) if cycles per frame stay
   constant. (The synthetic worst case, 1,208,071 cycles, gives 15.1 ms at 80 MHz.) Remaining
   flash-resident constants may make cycles clock dependent. Power must still be measured
   separately.
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
   Also record heap and stack high-water for the stream tasks.
7. Check cache-counter availability for this exact original ESP32 target before claiming
   hit/miss counts. Until then, label placement results as indirect evidence.
8. Real-audio input. Streaming works and its first results are recorded. Open decision: whether
   to use real speech as the optimization comparison input. The streamed numbers are not a
   controlled baseline (they shifted 1.4 % between firmware revisions with unchanged encoder code),
   so if real audio is wanted for comparing changes, add a fixed real clip to the repeated-baseline
   protocol and record its hash. Until that decision, keep the synthetic benchmark as the
   regression baseline. Remaining stream work is listed under "USB streaming mode".
9. Later: compare available codec implementations (including a floating-point build, since the
   ESP32 has a hardware single-precision FPU) and decode performance at complexity 0, and
   measure energy/current with a documented setup. If the system comparison proceeds,
   measure wired and wireless transmission latency and packet loss. The streaming work already
   showed byte loss on the UART link at 921600, so loss handling matters for any later link
   comparison.

## Measurement rules

- The cycle counter brackets `opus_encode()`. Interrupts and system activity during that
  interval affect the result; logging and the output block occur outside the timed encode.
- Use the `run_baseline_repeats()` median as the comparison figure and the run-average range as
  the within-boot noise estimate. Because cycle counts also shift between builds, record
  the `linker.lf` contents, `idf.py size` output, and free IRAM with each run, and treat
  differences of a few percent between builds cautiously until a layout-sensitivity control
  (a no-op change that shifts code layout) has been measured. One accidental data point exists:
  three stream firmware revisions that changed only core 0 code and buffer sizes moved the
  conversation file's average encode cycles from about 1,082,975 to 1,079,600 to 1,068,000
  (1.4 % total) with identical output hash, and the link speed had no visible effect (within
  0.1 %). This is not a controlled experiment; treat differences of 1 % to 2 % between builds as
  within layout sensitivity.
- For streamed runs, count only runs whose integrity line reports `OK`, and record failed runs
  separately with their error text and firmware revision. Record the baud rate and firmware
  revision with every streamed result.
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
- `main/CMakeLists.txt` builds one of two firmwares chosen by `APP_MODE` (`benchmark`, the
  default, or `stream`). Only one `app_main()` exists per build. Keep the benchmark path unchanged
  so baselines stay comparable; see "USB streaming mode".
- Opus calls in the stream build happen only on core 1 (`enc_task`), consistent with the
  non-thread-safe per-core pseudostack. Do not call Opus from the core 0 tasks.
- Stream protocol changes must be made together in `main/stream_codec.c`, `main/stream_codec.h`,
  and `tools/stream_mp3.py`. The timing constants must stay consistent: the board's
  `DONE_GRACE_US` (6 s) must exceed the script's END resend window (`END_RESEND_AFTER_S` x
  `END_MAX_RESENDS` plus one interval, about 5 s), and the script's `STALL_TIMEOUT_S` (12 s) must
  exceed the board's 5 s session timeout.
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
- The board is an AiTrip ESP-WROOM-32 development board (3-pack, Amazon ASIN B08D5ZD528). A
  reseller-page reviewer says it uses the CP2102 USB-to-serial chip; this is not an official spec,
  but it agrees with the CP210x bridge noted above. The original CP2102 supports up to 1 Mbaud.
  Streaming defaults to 460800 because 921600 failed intermittently on this board and cable; the
  cause is not identified (see "USB streaming mode"). The original ESP32 has no native USB; "USB"
  here means this UART bridge, which is shared with the console.
- If flashing reports `Wrong boot mode detected (0x13)`, hold BOOT, tap EN, then release
  BOOT after the chip is detected. Close the serial monitor before flashing.
- Press EN to rerun the flashed benchmark. A panic may reboot repeatedly; hold EN or
  unplug to stop it.
- Treat build logs and serial output as evidence and label assumptions. Put shared build
  defaults in tracked `sdkconfig.defaults`, not machine-specific `sdkconfig` or VS Code
  settings.
- The project uses CMake/Ninja, not Makefiles. Avoid spaces and very long paths for
  compatibility with ESP-IDF tools on Windows.