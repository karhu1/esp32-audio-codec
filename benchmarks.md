### Base, Compiler Options: Debug, 160MHz

```
ESP32 benchmark harness starting...

===== Benchmark results =====
CPU clock          : 160 MHz
Frame              : 320 samples (20 ms @ 16000 Hz)
Iterations         : 200 (after 10 warm-up)
Cycles/frame       : min 538583  avg 539016  max 539884
Time/frame (avg)   : 3368.9 us  (budget 20000 us)
Cycles per sample  : 1684.4
CPU load (1 core)  : 16.8 %
Real-time capable  : YES
```

### Base, Compiler Options: Debug, 240MHz

```
ESP32 benchmark harness starting...

===== Benchmark results =====
CPU clock          : 240 MHz
Frame              : 320 samples (20 ms @ 16000 Hz)
Iterations         : 200 (after 10 warm-up)
Cycles/frame       : min 538583  avg 538875  max 539890
Time/frame (avg)   : 2245.3 us  (budget 20000 us)
Cycles per sample  : 1684.0
CPU load (1 core)  : 11.2 %
Real-time capable  : YES
```

### Base, Compiler Options: Performance 240MHz

```
ESP32 benchmark harness starting...

===== Benchmark results =====
CPU clock          : 240 MHz
Frame              : 320 samples (20 ms @ 16000 Hz)
Iterations         : 200 (after 10 warm-up)
Cycles/frame       : min 359144  avg 359331  max 360406
Time/frame (avg)   : 1497.2 us  (budget 20000 us)
Cycles per sample  : 1122.9
CPU load (1 core)  : 7.5 %
Real-time capable  : YES
```

### Opus, Compiler Options: Performance 240MHz

```
Opus benchmark starting (libopus libopus 1.4-fixed)...
Generating test signal...
Encoding: pass -1 is an untimed warm-up, then 5 timed passes of 50 frames...

===== Opus encode benchmark =====
CPU clock           : 240 MHz
Format              : 16000 Hz mono, 20 ms frames (320 samples)
Settings            : target 20000 bps, complexity 0, VBR on, VOIP/voice
Frames timed        : 250 (5 passes x 50 frames)
Cycles/frame        : min 1460186  avg 1851940  max 2087297 (worst = frame 20)
Time/frame          : avg 7716.4 us  max 8697.1 us  (budget 20000 us)
CPU load (1 core)   : avg 38.6 %  worst-case 43.5 %
Real-time (average) : YES
Real-time (worst)   : YES
Opus modes used     : SILK 250, hybrid 0, CELT 0 frames
Output              : 45.8 bytes/frame avg, effective 18304 bps
Output hash (FNV-1a): 0x317FC6D7   (must not change after optimizing)
Deterministic       : YES (all passes identical)
Encoder state size  : 15228 bytes (opus_encoder_get_size)
Heap used by create : 15364 bytes
Heap free / min     : 181272 / 181212 bytes
Task stack          : 16384 bytes allocated, 12940 bytes never used

Done. Press EN to run again.
```