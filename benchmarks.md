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

### Micro-Opus, Compiler Options: Performance 240MHz

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

### Micro-Opus v2, Compiler Options: Performance 240MHz

```
Opus benchmark v2 starting (libopus libopus unknown-fixed)...
Generating test signal...
Sweeping complexity levels; each runs 1 warm-up pass + 5 timed passes of 50 frames.
  complexity 0 ...

--- Per-frame detail, complexity 0 (cycles averaged over 5 passes) ---
frame, avg_cycles, avg_us, bytes, toc_config
0, 1781131, 7421.4, 38, 9
1, 2234314, 9309.6, 47, 9
2, 2321705, 9673.8, 43, 9
3, 2342407, 9760.0, 48, 9
4, 2317151, 9654.8, 41, 9
5, 2292630, 9552.6, 40, 9
6, 2323896, 9682.9, 43, 9
7, 2292591, 9552.5, 45, 9
8, 2308649, 9619.4, 49, 9
9, 2310160, 9625.7, 49, 9
10, 2324619, 9685.9, 52, 9
11, 1940626, 8085.9, 55, 9
12, 1869458, 7789.4, 52, 9
13, 1822447, 7593.5, 51, 9
14, 1848175, 7700.7, 56, 9
15, 1844536, 7685.6, 45, 9
16, 2255599, 9398.3, 56, 9
17, 2278850, 9495.2, 46, 9
18, 2318479, 9660.3, 51, 9
19, 2326743, 9694.8, 50, 9
20, 2362564, 9844.0, 51, 9
21, 2305451, 9606.0, 46, 9
22, 2346959, 9779.0, 57, 9
23, 2311149, 9629.8, 42, 9
24, 2319603, 9665.0, 49, 9
25, 1964721, 8186.3, 36, 9
26, 1715138, 7146.4, 32, 9
27, 1660290, 6917.9, 27, 9
28, 1838496, 7660.4, 40, 9
29, 1667387, 6947.4, 50, 9
30, 1668200, 6950.8, 47, 9
31, 1826658, 7611.1, 56, 9
32, 1865032, 7771.0, 53, 9
33, 2252073, 9383.6, 49, 9
34, 2315065, 9646.1, 53, 9
35, 2295454, 9564.4, 47, 9
36, 2328474, 9702.0, 50, 9
37, 2326866, 9695.3, 44, 9
38, 2314573, 9644.1, 48, 9
39, 2291598, 9548.3, 46, 9
40, 2333336, 9722.2, 43, 9
41, 2319075, 9662.8, 32, 9
42, 2321170, 9671.5, 43, 9
43, 1747446, 7281.0, 32, 9
44, 1666506, 6943.8, 43, 9
45, 1647488, 6864.5, 38, 9
46, 1626476, 6777.0, 44, 9
47, 1643188, 6846.6, 49, 9
48, 1852543, 7718.9, 42, 9
49, 1860660, 7752.8, 42, 9
  complexity 2 ...
  complexity 5 ...
  complexity 10 ...

===== Opus encode benchmark: complexity sweep =====
CPU clock           : 240 MHz
Compiler opt level  : -O2 (performance)
Format              : 16000 Hz mono, 20 ms frames (320 samples)
Settings            : target 20000 bps, VBR on, VOIP/voice
Frames per level    : 250 timed (5 passes x 50 frames)
Real-time budget    : 20000 us per frame

cplx | min cycles |  avg cycles | max cycles | avg us | max us | load avg/worst % | RT | B/frame |  bps  | modes S/H/C | hash       | det
   0 |    1626475 |     2086956 |    2364289 |   8696 |   9851 |   43.5 / 49.3   | OK |    45.8 | 18304 | 250/0/0 | 0x317FC6D7 | yes
   2 |    2182330 |     2661713 |    2962903 |  11090 |  12345 |   55.5 / 61.7   | OK |    44.6 | 17856 | 250/0/0 | 0xE56B6272 | yes
   5 |    2976527 |     3548216 |    3843391 |  14784 |  16014 |   73.9 / 80.1   | OK |    39.8 | 15936 | 250/0/0 | 0x2550DE12 | yes
  10 |    5939168 |     6620667 |    6898777 |  27586 |  28745 |  137.9 / 143.7  | NO |    42.4 | 16960 | 250/0/0 | 0x8949A3CB | yes
(RT: OK = even the slowest frame fits the budget; avg = only the average fits; NO = too slow)

Baseline check (complexity 0): hash 0x317FC6D7 vs v1 baseline 0x317FC6D7 -> MATCH

Encoder state size  : 29236 bytes (opus_encoder_get_size)
Heap used by create : 29700 bytes
Heap free / min     : 187900 / 158092 bytes
Task stack          : 24576 bytes allocated, 20860 bytes never used
```

### Opus, Baseline

```
Opus benchmark v2 starting (libopus libopus 1.4-fixed)...
Generating test signal...
Sweeping complexity levels; each runs 1 warm-up pass + 5 timed passes of 50 frames.
  complexity 0 ...

--- Per-frame detail, complexity 0 (cycles averaged over 5 passes) ---
frame, avg_cycles, avg_us, bytes, toc_config
0, 1539204, 6413.4, 38, 9
1, 2010289, 8376.2, 47, 9
2, 2062246, 8592.7, 43, 9
3, 2062450, 8593.5, 48, 9
4, 2054272, 8559.5, 41, 9
5, 2022687, 8427.9, 40, 9
6, 2060301, 8584.6, 43, 9
7, 2027739, 8448.9, 45, 9
8, 2054076, 8558.7, 49, 9
9, 2040758, 8503.2, 49, 9
10, 2064954, 8604.0, 52, 9
11, 1712624, 7135.9, 55, 9
12, 1669647, 6956.9, 52, 9
13, 1625898, 6774.6, 51, 9
14, 1654871, 6895.3, 56, 9
15, 1640936, 6837.2, 45, 9
16, 2006885, 8362.0, 56, 9
17, 2025508, 8439.6, 46, 9
18, 2049378, 8539.1, 51, 9
19, 2055421, 8564.3, 50, 9
20, 2077613, 8656.7, 51, 9
21, 2039362, 8497.3, 46, 9
22, 2069843, 8624.3, 57, 9
23, 2040902, 8503.8, 42, 9
24, 2052740, 8553.1, 49, 9
25, 1731111, 7213.0, 36, 9
26, 1490145, 6208.9, 32, 9
27, 1465770, 6107.4, 27, 9
28, 1651197, 6880.0, 40, 9
29, 1470954, 6129.0, 50, 9
30, 1470737, 6128.1, 47, 9
31, 1646539, 6860.6, 56, 9
32, 1660351, 6918.1, 53, 9
33, 2004243, 8351.0, 49, 9
34, 2060147, 8583.9, 53, 9
35, 2026725, 8444.7, 47, 9
36, 2059895, 8582.9, 50, 9
37, 2044529, 8518.9, 44, 9
38, 2052672, 8552.8, 48, 9
39, 2024093, 8433.7, 46, 9
40, 2059673, 8582.0, 43, 9
41, 2036906, 8487.1, 32, 9
42, 2057747, 8573.9, 43, 9
43, 1500677, 6252.8, 32, 9
44, 1468689, 6119.5, 43, 9
45, 1466147, 6108.9, 38, 9
46, 1459479, 6081.2, 44, 9
47, 1461952, 6091.5, 49, 9
48, 1670781, 6961.6, 42, 9
49, 1657238, 6905.2, 42, 9
  complexity 2 ...
  complexity 5 ...
  complexity 10 ...

===== Opus encode benchmark: complexity sweep =====
CPU clock           : 240 MHz
Compiler opt level  : -O2 (performance)
Format              : 16000 Hz mono, 20 ms frames (320 samples)
Settings            : target 20000 bps, VBR on, VOIP/voice
Frames per level    : 250 timed (5 passes x 50 frames)
Real-time budget    : 20000 us per frame

cplx | min cycles |  avg cycles | max cycles | avg us | max us | load avg/worst % | RT | B/frame |  bps  | modes S/H/C | hash       | det
   0 |    1459262 |     1848380 |    2079356 |   7702 |   8664 |   38.5 / 43.3   | OK |    45.8 | 18304 | 250/0/0 | 0x317FC6D7 | yes
   2 |    1881965 |     2332174 |    2597552 |   9717 |  10823 |   48.6 / 54.1   | OK |    44.6 | 17856 | 250/0/0 | 0xE56B6272 | yes
   5 |    2606922 |     3149866 |    3417314 |  13124 |  14239 |   65.6 / 71.2   | OK |    39.8 | 15936 | 250/0/0 | 0x2550DE12 | yes
  10 |    3609375 |     4163580 |    4421975 |  17348 |  18425 |   86.7 / 92.1   | OK |    42.4 | 16960 | 250/0/0 | 0x8949A3CB | yes
(RT: OK = even the slowest frame fits the budget; avg = only the average fits; NO = too slow)

Baseline check (complexity 0): hash 0x317FC6D7 vs v1 baseline 0x317FC6D7 -> MATCH

Encoder state size  : 15228 bytes (opus_encoder_get_size)
Heap used by create : 15364 bytes
Heap free / min     : 187852 / 172428 bytes
Task stack          : 24576 bytes allocated, 20940 bytes never used
```