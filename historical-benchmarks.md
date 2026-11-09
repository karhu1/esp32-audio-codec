# Historical Benchmark Records

This archive preserves captured benchmark and profiling output from multiple builds and
experiments. Settings and metrics differ between entries, so compare runs only when their
recorded conditions match. Current project scope, measurement rules, and workflow are in
[AGENTS.md](AGENTS.md).

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

### Opus, Baseline + Xtensa LX7

```
Opus benchmark v2 starting (libopus libopus unknown-fixed)...
Generating test signal...
Sweeping complexity levels; each runs 1 warm-up pass + 5 timed passes of 50 frames.
  complexity 0 ...

--- Per-frame detail, complexity 0 (cycles averaged over 5 passes) ---
frame, avg_cycles, avg_us, bytes, toc_config
0, 1596058, 6650.2, 38, 9
1, 2028655, 8452.7, 47, 9
2, 2091088, 8712.9, 43, 9
3, 2113398, 8805.8, 48, 9
4, 2076205, 8650.9, 41, 9
5, 2066082, 8608.7, 40, 9
6, 2089788, 8707.5, 43, 9
7, 2064756, 8603.2, 45, 9
8, 2071021, 8629.3, 49, 9
9, 2089664, 8706.9, 49, 9
10, 2093133, 8721.4, 52, 9
11, 1746081, 7275.3, 55, 9
12, 1687285, 7030.4, 52, 9
13, 1654174, 6892.4, 51, 9
14, 1671054, 6962.7, 56, 9
15, 1677380, 6989.1, 45, 9
16, 2042760, 8511.5, 56, 9
17, 2056412, 8568.4, 46, 9
18, 2083398, 8680.8, 51, 9
19, 2093658, 8723.6, 50, 9
20, 2118226, 8825.9, 51, 9
21, 2067970, 8616.5, 46, 9
22, 2106312, 8776.3, 57, 9
23, 2079646, 8665.2, 42, 9
24, 2083338, 8680.6, 49, 9
25, 1751843, 7299.3, 36, 9
26, 1519270, 6330.3, 32, 9
27, 1502841, 6261.8, 27, 9
28, 1691532, 7048.0, 40, 9
29, 1486943, 6195.6, 50, 9
30, 1496658, 6236.1, 47, 9
31, 1672173, 6967.4, 56, 9
32, 1681428, 7006.0, 53, 9
33, 2047342, 8530.6, 49, 9
34, 2081532, 8673.0, 53, 9
35, 2070172, 8625.7, 47, 9
36, 2086247, 8692.7, 50, 9
37, 2090324, 8709.7, 44, 9
38, 2069387, 8622.4, 48, 9
39, 2067918, 8616.3, 46, 9
40, 2096247, 8734.4, 43, 9
41, 2086596, 8694.2, 32, 9
42, 2078289, 8659.5, 43, 9
43, 1544335, 6434.7, 32, 9
44, 1495690, 6232.0, 43, 9
45, 1492786, 6219.9, 38, 9
46, 1481092, 6171.2, 44, 9
47, 1488112, 6200.5, 49, 9
48, 1695106, 7062.9, 42, 9
49, 1681644, 7006.9, 42, 9
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
   0 |    1480515 |     1880061 |    2118621 |   7834 |   8828 |   39.2 / 44.1   | OK |    45.8 | 18304 | 250/0/0 | 0x317FC6D7 | yes
   2 |    1849143 |     2264442 |    2525775 |   9435 |  10524 |   47.2 / 52.6   | OK |    44.6 | 17856 | 250/0/0 | 0xE56B6272 | yes
   5 |    2610025 |     3081739 |    3347961 |  12841 |  13950 |   64.2 / 69.7   | OK |    39.8 | 15936 | 250/0/0 | 0x2550DE12 | yes
  10 |    5306782 |     5906535 |    6143496 |  24611 |  25598 |  123.1 / 128.0  | NO |    42.4 | 16960 | 250/0/0 | 0x8949A3CB | yes
(RT: OK = even the slowest frame fits the budget; avg = only the average fits; NO = too slow)

Baseline check (complexity 0): hash 0x317FC6D7 vs v1 baseline 0x317FC6D7 -> MATCH

Encoder state size  : 29236 bytes (opus_encoder_get_size)
Heap used by create : 29700 bytes
Heap free / min     : 187900 / 158092 bytes
Task stack          : 24576 bytes allocated, 20892 bytes never used

Done. Press EN to run again.
```

### Opus, Baseline + Xtensa LX7, -03 Compiler

Notes: Ended up worse with the -03, so removing.

```
Opus benchmark v2 starting (libopus libopus unknown-fixed)...
Generating test signal...
Sweeping complexity levels; each runs 1 warm-up pass + 5 timed passes of 50 frames.
  complexity 0 ...

--- Per-frame detail, complexity 0 (cycles averaged over 5 passes) ---
frame, avg_cycles, avg_us, bytes, toc_config
0, 1817732, 7573.9, 38, 9
1, 2246727, 9361.4, 47, 9
2, 2296469, 9568.6, 43, 9
3, 2294347, 9559.8, 48, 9
4, 2260571, 9419.0, 41, 9
5, 2223941, 9266.4, 40, 9
6, 2290167, 9542.4, 43, 9
7, 2229841, 9291.0, 45, 9
8, 2253748, 9390.6, 49, 9
9, 2269247, 9455.2, 49, 9
10, 2296663, 9569.4, 52, 9
11, 1922542, 8010.6, 55, 9
12, 1891141, 7879.8, 52, 9
13, 1846464, 7693.6, 51, 9
14, 1882464, 7843.6, 56, 9
15, 1895513, 7898.0, 45, 9
16, 2268928, 9453.9, 56, 9
17, 2231660, 9298.6, 46, 9
18, 2281658, 9506.9, 51, 9
19, 2286197, 9525.8, 50, 9
20, 2319790, 9665.8, 51, 9
21, 2241350, 9339.0, 46, 9
22, 2309254, 9621.9, 57, 9
23, 2269726, 9457.2, 42, 9
24, 2290472, 9543.6, 49, 9
25, 1953064, 8137.8, 36, 9
26, 1728955, 7204.0, 32, 9
27, 1710282, 7126.2, 27, 9
28, 1909692, 7957.0, 40, 9
29, 1678958, 6995.7, 50, 9
30, 1676720, 6986.3, 47, 9
31, 1873420, 7805.9, 56, 9
32, 1908988, 7954.1, 53, 9
33, 2263618, 9431.7, 49, 9
34, 2291040, 9546.0, 53, 9
35, 2238361, 9326.5, 47, 9
36, 2295219, 9563.4, 50, 9
37, 2276860, 9486.9, 44, 9
38, 2260150, 9417.3, 48, 9
39, 2227676, 9282.0, 46, 9
40, 2294186, 9559.1, 43, 9
41, 2267885, 9449.5, 32, 9
42, 2292017, 9550.1, 43, 9
43, 1715620, 7148.4, 32, 9
44, 1680828, 7003.5, 43, 9
45, 1691194, 7046.6, 38, 9
46, 1676396, 6985.0, 44, 9
47, 1692133, 7050.6, 49, 9
48, 1919092, 7996.2, 42, 9
49, 1918394, 7993.3, 42, 9
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
   0 |    1675795 |     2077147 |    2320180 |   8655 |   9667 |   43.3 / 48.3   | OK |    45.8 | 18304 | 250/0/0 | 0x317FC6D7 | yes
   2 |    2158900 |     2570000 |    2823934 |  10708 |  11766 |   53.5 / 58.8   | OK |    44.6 | 17856 | 250/0/0 | 0xE56B6272 | yes
   5 |    2902676 |     3363666 |    3606814 |  14015 |  15028 |   70.1 / 75.1   | OK |    39.8 | 15936 | 250/0/0 | 0x2550DE12 | yes
  10 |    5841875 |     6495293 |    6739441 |  27064 |  28081 |  135.3 / 140.4  | NO |    42.4 | 16960 | 250/0/0 | 0x8949A3CB | yes
(RT: OK = even the slowest frame fits the budget; avg = only the average fits; NO = too slow)

Baseline check (complexity 0): hash 0x317FC6D7 vs v1 baseline 0x317FC6D7 -> MATCH

Encoder state size  : 29236 bytes (opus_encoder_get_size)
Heap used by create : 29700 bytes
Heap free / min     : 187900 / 158092 bytes
Task stack          : 24576 bytes allocated, 20892 bytes never used

Done. Press EN to run again.
```

### Opus, Baseline + Xtensa LX7 (SILK measurements)

```
Opus benchmark v2 starting (libopus libopus unknown-fixed)...
Generating test signal...
Sweeping complexity levels; each runs 1 warm-up pass + 5 timed passes of 50 frames.
  complexity 0 ...

--- Per-frame detail, complexity 0 (cycles averaged over 5 passes) ---
frame, avg_cycles, avg_us, bytes, toc_config
0, 1613757, 6724.0, 38, 9
1, 2029732, 8457.2, 47, 9
2, 2077193, 8655.0, 43, 9
3, 2098822, 8745.1, 48, 9
4, 2063469, 8597.8, 41, 9
5, 2049653, 8540.2, 40, 9
6, 2080267, 8667.8, 43, 9
7, 2052700, 8552.9, 45, 9
8, 2068310, 8618.0, 49, 9
9, 2070195, 8625.8, 49, 9
10, 2089378, 8705.7, 52, 9
11, 1763395, 7347.5, 55, 9
12, 1708305, 7117.9, 52, 9
13, 1663434, 6931.0, 51, 9
14, 1685430, 7022.6, 56, 9
15, 1687692, 7032.0, 45, 9
16, 2041394, 8505.8, 56, 9
17, 2034883, 8478.7, 46, 9
18, 2073638, 8640.2, 51, 9
19, 2081147, 8671.4, 50, 9
20, 2111153, 8796.5, 51, 9
21, 2060098, 8583.7, 46, 9
22, 2093386, 8722.4, 57, 9
23, 2059304, 8580.4, 42, 9
24, 2077171, 8654.9, 49, 9
25, 1774298, 7392.9, 36, 9
26, 1540852, 6420.2, 32, 9
27, 1519683, 6332.0, 27, 9
28, 1706974, 7112.4, 40, 9
29, 1510474, 6293.6, 50, 9
30, 1500737, 6253.1, 47, 9
31, 1680796, 7003.3, 56, 9
32, 1699429, 7081.0, 53, 9
33, 2042325, 8509.7, 49, 9
34, 2076368, 8651.5, 53, 9
35, 2050732, 8544.7, 47, 9
36, 2083652, 8681.9, 50, 9
37, 2079412, 8664.2, 44, 9
38, 2064233, 8601.0, 48, 9
39, 2053065, 8554.4, 46, 9
40, 2097429, 8739.3, 43, 9
41, 2079865, 8666.1, 32, 9
42, 2071788, 8632.5, 43, 9
43, 1562149, 6509.0, 32, 9
44, 1518280, 6326.2, 43, 9
45, 1495324, 6230.5, 38, 9
46, 1493348, 6222.3, 44, 9
47, 1487158, 6196.5, 49, 9
48, 1712580, 7135.8, 42, 9
49, 1711875, 7132.8, 42, 9
  complexity 2 ...
  complexity 5 ...
  complexity 10 ...

--- SILK stage profile, complexity 10 (250 timed frames) ---
stage, total_cycles, avg_cycles/call, calls
pitch analysis, 113130384, 452521, 250
noise-shape analysis, 199420198, 797680, 250
predictor search, 259269307, 1037077, 250
gain processing, 9676107, 38704, 250
NSQ retries, 339247606, 1356990, 250
entropy encoding, 32577888, 130311, 250
LBRR total, 585930, 2343, 250
(LBRR total includes its own quantization and entropy work; it overlaps those counters.)

===== Opus encode benchmark: complexity sweep =====
CPU clock           : 240 MHz
Compiler opt level  : -O2 (performance)
Format              : 16000 Hz mono, 20 ms frames (320 samples)
Settings            : target 20000 bps, VBR on, VOIP/voice
Frames per level    : 250 timed (5 passes x 50 frames)
Real-time budget    : 20000 us per frame

cplx | min cycles |  avg cycles | max cycles | avg us | max us | load avg/worst % | RT | B/frame |  bps  | modes S/H/C | hash       | det
   0 |    1487149 |     1880935 |    2112286 |   7837 |   8801 |   39.2 / 44.0   | OK |    45.8 | 18304 | 250/0/0 | 0x317FC6D7 | yes
   2 |    1886221 |     2313626 |    2580217 |   9640 |  10751 |   48.2 / 53.8   | OK |    44.6 | 17856 | 250/0/0 | 0xE56B6272 | yes
   5 |    2632436 |     3124665 |    3384058 |  13019 |  14100 |   65.1 / 70.5   | OK |    39.8 | 15936 | 250/0/0 | 0x2550DE12 | yes
  10 |    5347697 |     5943069 |    6179410 |  24763 |  25748 |  123.8 / 128.7  | NO |    42.4 | 16960 | 250/0/0 | 0x8949A3CB | yes
(RT: OK = even the slowest frame fits the budget; avg = only the average fits; NO = too slow)

Baseline check (complexity 0): hash 0x317FC6D7 vs v1 baseline 0x317FC6D7 -> MATCH

Encoder state size  : 29236 bytes (opus_encoder_get_size)
Heap used by create : 29700 bytes
Heap free / min     : 187716 / 157908 bytes
Task stack          : 24576 bytes allocated, 20884 bytes never used

Done. Press EN to run again.
```

# Opus Latest

```
Opus benchmark v2 starting (libopus libopus unknown-fixed)...
Generating test signal...
Sweeping complexity levels; each runs 1 warm-up pass + 5 timed passes of 50 frames.
  complexity 0 ...

--- Per-frame detail, complexity 0 (cycles averaged over 5 passes) ---
frame, avg_cycles, avg_us, bytes, toc_config
0, 1625125, 6771.4, 38, 9
1, 2081455, 8672.7, 47, 9
2, 2142165, 8925.7, 43, 9
3, 2164641, 9019.3, 48, 9
4, 2123073, 8846.1, 41, 9
5, 2106175, 8775.7, 40, 9
6, 2133140, 8888.1, 43, 9
7, 2115264, 8813.6, 45, 9
8, 2115303, 8813.8, 49, 9
9, 2123735, 8848.9, 49, 9
10, 2156846, 8986.9, 52, 9
11, 1797980, 7491.6, 55, 9
12, 1726709, 7194.6, 52, 9
13, 1696081, 7067.0, 51, 9
14, 1707631, 7115.1, 56, 9
15, 1712583, 7135.8, 45, 9
16, 2079657, 8665.2, 56, 9
17, 2108622, 8785.9, 46, 9
18, 2118369, 8826.5, 51, 9
19, 2153201, 8971.7, 50, 9
20, 2173923, 9058.0, 51, 9
21, 2130323, 8876.3, 46, 9
22, 2142109, 8925.5, 57, 9
23, 2130622, 8877.6, 42, 9
24, 2130723, 8878.0, 49, 9
25, 1813575, 7556.6, 36, 9
26, 1528467, 6368.6, 32, 9
27, 1504124, 6267.2, 27, 9
28, 1716041, 7150.2, 40, 9
29, 1500032, 6250.1, 50, 9
30, 1526879, 6362.0, 47, 9
31, 1712944, 7137.3, 56, 9
32, 1715733, 7148.9, 53, 9
33, 2088803, 8703.3, 49, 9
34, 2136888, 8903.7, 53, 9
35, 2111239, 8796.8, 47, 9
36, 2128881, 8870.3, 50, 9
37, 2141298, 8922.1, 44, 9
38, 2122901, 8845.4, 48, 9
39, 2103094, 8762.9, 46, 9
40, 2149618, 8956.7, 43, 9
41, 2135345, 8897.3, 32, 9
42, 2130080, 8875.3, 43, 9
43, 1568060, 6533.6, 32, 9
44, 1510025, 6291.8, 43, 9
45, 1505346, 6272.3, 38, 9
46, 1498388, 6243.3, 44, 9
47, 1500627, 6252.6, 49, 9
48, 1730721, 7211.3, 42, 9
49, 1716144, 7150.6, 42, 9
  complexity 2 ...
  complexity 5 ...
  complexity 10 ...

--- SILK stage profile, complexity 10 (250 timed frames) ---
stage, total_cycles, avg_cycles/call, calls
pitch analysis, 113203731, 452814, 250
noise-shape analysis, 201054467, 804217, 250
predictor search, 258645568, 1034582, 250
gain processing, 9756654, 39026, 250
NSQ quantizer, 336981386, 1347925, 250
entropy encoding, 32525646, 130102, 250
LBRR total, 726000, 2904, 250
predictor: LTP, 27288402, 188195, 145
predictor: LPC, 127600284, 510401, 250
predictor: NLSF quantization, 82081847, 328327, 250
predictor: residual energy, 14926705, 59706, 250
SILK frame total, 965300392, 3861201, 250
SILK API call total, 1022132620, 4088530, 250
Opus tonality analysis, 324536496, 1298145, 250
Opus frame processing, 1062567716, 4250270, 250
tonality: feature analysis, 315581101, 1262324, 250
tonality: result aggregation, 6841564, 27366, 250
tonality: FFT, 89360391, 357441, 250
tonality: phase/atan2 loop, 54690476, 218761, 250
tonality: neural model, 66564842, 266259, 250
tonality: input preparation, 34296233, 137184, 250
tonality: post-phase features, 67179317, 268717, 250
tonality: band statistics, 32476178, 129904, 250
tonality: leakage, 4902675, 19610, 250
tonality: spectral variability, 6240250, 24961, 250
tonality: bandwidth, 7177232, 28708, 250
tonality: DCT/features, 16036282, 64145, 250
(Parent and child totals overlap: predictor, SILK frame, SILK API, and Opus frame totals are inclusive. LBRR overlaps its own quantization and entropy work.)

===== Opus encode benchmark: complexity sweep =====
CPU clock           : 240 MHz
Compiler opt level  : -O2 (performance)
Format              : 16000 Hz mono, 20 ms frames (320 samples)
Settings            : target 20000 bps, VBR on, VOIP/voice
Frames per level    : 250 timed (5 passes x 50 frames)
Real-time budget    : 20000 us per frame

cplx | min cycles |  avg cycles | max cycles | avg us | max us | load avg/worst % | RT | B/frame |  bps  | modes S/H/C | hash       | det
   0 |    1498388 |     1919814 |    2174510 |   7999 |   9060 |   40.0 / 45.3   | OK |    45.8 | 18304 | 250/0/0 | 0x317FC6D7 | yes
   2 |    1906188 |     2328465 |    2583855 |   9702 |  10766 |   48.5 / 53.8   | OK |    44.6 | 17856 | 250/0/0 | 0xE56B6272 | yes
   5 |    2665873 |     3146941 |    3394563 |  13112 |  14144 |   65.6 / 70.7   | OK |    39.8 | 15936 | 250/0/0 | 0x2550DE12 | yes
  10 |    5174155 |     5672104 |    5900994 |  23634 |  24587 |  118.2 / 122.9  | NO |    42.4 | 16960 | 250/0/0 | 0x8949A3CB | yes
(RT: OK = even the slowest frame fits the budget; avg = only the average fits; NO = too slow)

Baseline check (complexity 0): hash 0x317FC6D7 vs v1 baseline 0x317FC6D7 -> MATCH

Encoder state size  : 29236 bytes (opus_encoder_get_size)
Heap used by create : 29700 bytes
Heap free / min     : 187236 / 157428 bytes
Task stack          : 24576 bytes allocated, 20852 bytes never used

Done. Press EN to run again.
```
