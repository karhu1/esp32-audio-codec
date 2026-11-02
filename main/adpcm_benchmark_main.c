/*
 * ESP32 IMA ADPCM encoder benchmark
 * ----------------------------------
 * Encodes the same deterministic, speech-like test signal used by
 * opus_benchmark_main.c, but with a hand-written IMA/DVI4 ADPCM encoder
 * (the classic 1992 Interactive Multimedia Association algorithm; it's the
 * same math used by .wav IMA-ADPCM, DVI4, and most textbook ADPCM examples)
 * instead of libopus. Reports CPU cycles per 20 ms frame, memory use, and a
 * hash of the encoded output, in the same format as the Opus benchmark so
 * the two codecs can be compared directly.
 *
 * Unlike Opus (variable bitrate, big adaptive predictor, ~15 KB state),
 * ADPCM here is fixed-rate: every 16-bit sample always becomes exactly 4
 * bits, so compression is always exactly 4x and encoder state is a couple
 * of bytes. The interesting comparison is cycles/frame and bytes/frame
 * against the Opus baseline, not just raw "does it compress" correctness.
 *
 * The output hash is your correctness check: after you optimize anything,
 * the hash must stay the same as the baseline (the encoder is deterministic,
 * so any change means your optimization altered the bitstream).
 *
 * Runs once per reset. Press EN to run it again.
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"        // esp_cpu_get_cycle_count()
#include "esp_rom_sys.h"    // esp_rom_get_cpu_ticks_per_us()
#include "esp_heap_caps.h"  // heap_caps_get_free_size()

// ---- Test parameters -------------------------------------------------------
#define SAMPLE_RATE_HZ        16000
#define FRAME_MS              20
#define FRAME_SAMPLES         (SAMPLE_RATE_HZ * FRAME_MS / 1000)   // 320
#define FRAME_BYTES           (FRAME_SAMPLES / 2)                  // 160 (4 bits/sample)
#define NUM_FRAMES            50      // frames per pass (1 second of audio)
#define NUM_PASSES            5       // timed passes (after 1 untimed warm-up pass)

// ADPCM needs only a couple of bytes of state, so a small task stack is
// plenty. Raised anyway to match the Opus benchmark's margin of safety.
#define BENCH_TASK_STACK_BYTES  (8 * 1024)

#define NUM_HARMONICS         30
#define TWO_PI_F              6.28318530718f

// ---- Buffers (static, so they live in internal RAM, not on a stack) --------
static int16_t g_pcm[NUM_FRAMES * FRAME_SAMPLES];
static uint8_t g_pkt[FRAME_BYTES];

// ---- Test signal (identical to opus_benchmark_main.c, for a fair compare) --
// A "voiced speech" stand-in: a wobbling-pitch harmonic series with a slow
// amplitude envelope plus a little noise. A plain sine would be unrealistically
// easy for either encoder to compress.
static void generate_test_signal(int16_t *buf, int n)
{
    float phase[NUM_HARMONICS] = {0};
    uint32_t rng = 12345u;

    for (int i = 0; i < n; i++) {
        float t   = (float)i / (float)SAMPLE_RATE_HZ;
        float f0  = 110.0f + 25.0f * sinf(TWO_PI_F * 1.5f * t);        // pitch wobble
        float env = 0.5f + 0.5f * sinf(TWO_PI_F * 3.0f * t);           // syllable-rate envelope

        float s = 0.0f;
        for (int h = 0; h < NUM_HARMONICS; h++) {
            phase[h] += TWO_PI_F * f0 * (float)(h + 1) / (float)SAMPLE_RATE_HZ;
            if (phase[h] > TWO_PI_F) phase[h] -= TWO_PI_F;
            s += sinf(phase[h]) / (float)(h + 1);
        }

        rng = rng * 1664525u + 1013904223u;                             // tiny deterministic PRNG
        float noise = ((float)((int32_t)(rng >> 16) - 32768)) / 32768.0f;

        buf[i] = (int16_t)(5000.0f * s * env + 800.0f * noise);        // peak stays under 32767
    }
}

static uint32_t fnv1a(uint32_t h, const uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) {
        h ^= d[i];
        h *= 16777619u;
    }
    return h;
}

// ---- IMA/DVI4 ADPCM encoder -------------------------------------------------
// Standard IMA Digital Audio Focus / Interactive Multimedia Association
// tables (1992). This is the reference algorithm behind .wav IMA-ADPCM,
// DVI4, and most textbook ADPCM implementations.
static const int16_t k_step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t k_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

typedef struct {
    int16_t prev_sample;   // predictor
    int8_t  step_index;    // index into k_step_table
} adpcm_state_t;

static void adpcm_reset(adpcm_state_t *st)
{
    st->prev_sample = 0;
    st->step_index = 0;
}

static inline uint8_t adpcm_encode_sample(adpcm_state_t *st, int16_t sample)
{
    int diff = (int)sample - (int)st->prev_sample;
    int sign = 0;
    if (diff < 0) { sign = 8; diff = -diff; }

    int step = k_step_table[st->step_index];
    int delta = 0;
    int vpdiff = step >> 3;

    if (diff >= step) { delta = 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { delta |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { delta |= 1; vpdiff += step; }

    int predicted = (int)st->prev_sample + (sign ? -vpdiff : vpdiff);
    if (predicted > 32767)       predicted = 32767;
    else if (predicted < -32768) predicted = -32768;
    st->prev_sample = (int16_t)predicted;

    int code = delta | sign;
    st->step_index += k_index_table[code];
    if (st->step_index < 0)       st->step_index = 0;
    else if (st->step_index > 88) st->step_index = 88;

    return (uint8_t)code;
}

// Encodes FRAME_SAMPLES 16-bit samples into FRAME_SAMPLES/2 bytes (two
// 4-bit codes packed per byte, low nibble first). FRAME_SAMPLES must be even.
static void adpcm_encode_frame(adpcm_state_t *st, const int16_t *in, int n, uint8_t *out)
{
    for (int i = 0; i < n; i += 2) {
        uint8_t lo = adpcm_encode_sample(st, in[i]);
        uint8_t hi = adpcm_encode_sample(st, in[i + 1]);
        out[i / 2] = (uint8_t)(lo | (hi << 4));
    }
}

// ---- Benchmark task --------------------------------------------------------
static void bench_task(void *arg)
{
    (void)arg;

    const uint32_t cpu_mhz = esp_rom_get_cpu_ticks_per_us();   // real clock, read at run time

    uint32_t min_cycles = UINT32_MAX, max_cycles = 0;
    uint64_t sum_cycles = 0;
    uint32_t timed_frames = 0, worst_frame = 0;
    uint64_t total_bytes = 0;
    uint32_t ref_hash = 0;
    bool deterministic = true;

    printf("\nADPCM (IMA/DVI4) benchmark starting...\n");
    printf("Generating test signal...\n");
    generate_test_signal(g_pcm, NUM_FRAMES * FRAME_SAMPLES);

    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    adpcm_state_t state;   // no heap allocation needed at all
    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);

    printf("Encoding: pass -1 is an untimed warm-up, then %d timed passes of %d frames...\n",
           NUM_PASSES, NUM_FRAMES);

    for (int pass = -1; pass < NUM_PASSES; pass++) {
        adpcm_reset(&state);   // every pass starts from the same state
        uint32_t hash = 2166136261u;

        for (int f = 0; f < NUM_FRAMES; f++) {
            const int16_t *in = &g_pcm[f * FRAME_SAMPLES];

            uint32_t t0 = esp_cpu_get_cycle_count();
            adpcm_encode_frame(&state, in, FRAME_SAMPLES, g_pkt);
            uint32_t t1 = esp_cpu_get_cycle_count();

            hash = fnv1a(hash, g_pkt, FRAME_BYTES);

            if (pass >= 0) {
                uint32_t cycles = t1 - t0;          // unsigned math handles counter wraparound
                if (cycles < min_cycles) min_cycles = cycles;
                if (cycles > max_cycles) { max_cycles = cycles; worst_frame = (uint32_t)f; }
                sum_cycles += cycles;
                timed_frames++;
                total_bytes += FRAME_BYTES;
            }

            // Feed the watchdog and let other tasks run, outside the timed region.
            if ((f % 10) == 9) vTaskDelay(1);
        }

        if (pass == -1) {
            ref_hash = hash;
        } else if (hash != ref_hash) {
            deterministic = false;
        }
    }

    if (timed_frames > 0) {
        uint32_t avg_cycles = (uint32_t)(sum_cycles / timed_frames);
        float avg_us    = (float)avg_cycles / (float)cpu_mhz;
        float max_us    = (float)max_cycles / (float)cpu_mhz;
        float budget_us = (float)(FRAME_MS * 1000);
        float bitrate   = (float)FRAME_BYTES * 8.0f / (FRAME_MS / 1000.0f);   // fixed, not content-dependent

        printf("\n===== ADPCM (IMA/DVI4) encode benchmark =====\n");
        printf("CPU clock           : %u MHz\n", (unsigned)cpu_mhz);
        printf("Format              : %d Hz mono, %d ms frames (%d samples)\n",
               SAMPLE_RATE_HZ, FRAME_MS, FRAME_SAMPLES);
        printf("Settings            : 4 bits/sample, fixed-rate (no VBR, no complexity knob)\n");
        printf("Frames timed        : %u (%d passes x %d frames)\n",
               (unsigned)timed_frames, NUM_PASSES, NUM_FRAMES);
        printf("Cycles/frame        : min %" PRIu32 "  avg %" PRIu32 "  max %" PRIu32 " (worst = frame %u)\n",
               min_cycles, avg_cycles, max_cycles, (unsigned)worst_frame);
        printf("Time/frame          : avg %.1f us  max %.1f us  (budget %.0f us)\n",
               avg_us, max_us, budget_us);
        printf("CPU load (1 core)   : avg %.1f %%  worst-case %.1f %%\n",
               100.0f * avg_us / budget_us, 100.0f * max_us / budget_us);
        printf("Real-time (average) : %s\n", avg_us < budget_us ? "YES" : "NO");
        printf("Real-time (worst)   : %s\n", max_us < budget_us ? "YES" : "NO");
        printf("Output              : %d bytes/frame (fixed), %.0f bps, 4x compression vs 16-bit PCM\n",
               FRAME_BYTES, bitrate);
        printf("Output hash (FNV-1a): 0x%08" PRIX32 "   (must not change after optimizing)\n", ref_hash);
        printf("Deterministic       : %s\n", deterministic ? "YES (all passes identical)" : "NO - investigate!");
        printf("Encoder state size  : %d bytes (sizeof adpcm_state_t)\n", (int)sizeof(adpcm_state_t));
        printf("Heap used by create : %u bytes (no dynamic allocation needed)\n",
               (unsigned)(heap_before - heap_after));
        printf("Heap free / min     : %u / %u bytes\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
        printf("Task stack          : %u bytes allocated, %u bytes never used\n",
               (unsigned)BENCH_TASK_STACK_BYTES, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }

    printf("\nDone. Press EN to run again.\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Pin to core 1 so system and Wi-Fi housekeeping on core 0 disturbs the timing less.
    xTaskCreatePinnedToCore(bench_task, "adpcm_bench", BENCH_TASK_STACK_BYTES,
                            NULL, 5, NULL, 1);
}
