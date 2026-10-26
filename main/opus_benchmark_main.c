/*
 * ESP32 Opus encoder benchmark
 * ----------------------------
 * Encodes a deterministic, speech-like test signal with libopus and reports
 * CPU cycles per 20 ms frame, memory use, which Opus mode was chosen, and a
 * hash of the encoded output.
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
#include "opus.h"

// ---- Test parameters -------------------------------------------------------
#define SAMPLE_RATE_HZ        16000
#define FRAME_MS              20
#define FRAME_SAMPLES         (SAMPLE_RATE_HZ * FRAME_MS / 1000)   // 320
#define NUM_FRAMES            50      // frames per pass (1 second of audio)
#define NUM_PASSES            5       // timed passes (after 1 untimed warm-up pass)

#define BENCH_BITRATE         20000   // bits per second
#define BENCH_COMPLEXITY      0       // 0 (fastest) to 10 (best quality)

// Opus can need a lot of stack when it is not built with its pseudostack
// option, so the encoder runs in its own task with a big stack instead of
// the small default main task. If you see "Stack canary watchpoint triggered
// (opus_bench)", raise this.
#define BENCH_TASK_STACK_BYTES  (16 * 1024)

#define NUM_HARMONICS         30
#define TWO_PI_F              6.28318530718f

// ---- Buffers (static, so they live in internal RAM, not on a stack) --------
static int16_t g_pcm[NUM_FRAMES * FRAME_SAMPLES];
static uint8_t g_pkt[1276];   // 1276 bytes is the largest possible Opus packet

// ---- Test signal -----------------------------------------------------------
// A "voiced speech" stand-in: a wobbling-pitch harmonic series with a slow
// amplitude envelope plus a little noise. A plain sine would be unrealistically
// easy for the encoder to compress.
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

// ---- Benchmark task --------------------------------------------------------
static void bench_task(void *arg)
{
    (void)arg;

    const uint32_t cpu_mhz = esp_rom_get_cpu_ticks_per_us();   // real clock, read at run time

    uint32_t min_cycles = UINT32_MAX, max_cycles = 0;
    uint64_t sum_cycles = 0;
    uint32_t timed_frames = 0, worst_frame = 0;
    uint32_t total_bytes = 0;
    uint32_t silk_frames = 0, hybrid_frames = 0, celt_frames = 0;
    uint32_t ref_hash = 0;
    bool deterministic = true;
    bool ok = true;

    printf("\nOpus benchmark starting (libopus %s)...\n", opus_get_version_string());
    printf("Generating test signal...\n");
    generate_test_signal(g_pcm, NUM_FRAMES * FRAME_SAMPLES);

    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(SAMPLE_RATE_HZ, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || enc == NULL) {
        printf("opus_encoder_create failed: %s\n", opus_strerror(err));
        vTaskDelete(NULL);
        return;
    }
    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(BENCH_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(BENCH_COMPLEXITY));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    printf("Encoding: pass -1 is an untimed warm-up, then %d timed passes of %d frames...\n",
           NUM_PASSES, NUM_FRAMES);

    for (int pass = -1; pass < NUM_PASSES && ok; pass++) {
        opus_encoder_ctl(enc, OPUS_RESET_STATE);   // every pass starts from the same state
        uint32_t hash = 2166136261u;

        for (int f = 0; f < NUM_FRAMES; f++) {
            const int16_t *in = &g_pcm[f * FRAME_SAMPLES];

            uint32_t t0 = esp_cpu_get_cycle_count();
            int n = opus_encode(enc, in, FRAME_SAMPLES, g_pkt, (opus_int32)sizeof(g_pkt));
            uint32_t t1 = esp_cpu_get_cycle_count();

            if (n < 0) {
                printf("opus_encode failed: %s\n", opus_strerror(n));
                ok = false;
                break;
            }

            hash = fnv1a(hash, g_pkt, n);

            if (pass >= 0) {
                uint32_t cycles = t1 - t0;          // unsigned math handles counter wraparound
                if (cycles < min_cycles) min_cycles = cycles;
                if (cycles > max_cycles) { max_cycles = cycles; worst_frame = (uint32_t)f; }
                sum_cycles += cycles;
                timed_frames++;
                total_bytes += (uint32_t)n;

                int config = g_pkt[0] >> 3;         // Opus TOC byte: 0-11 SILK, 12-15 hybrid, 16-31 CELT
                if (config < 12)      silk_frames++;
                else if (config < 16) hybrid_frames++;
                else                  celt_frames++;
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

    if (ok && timed_frames > 0) {
        uint32_t avg_cycles = (uint32_t)(sum_cycles / timed_frames);
        float avg_us    = (float)avg_cycles / (float)cpu_mhz;
        float max_us    = (float)max_cycles / (float)cpu_mhz;
        float budget_us = (float)(FRAME_MS * 1000);
        float bitrate   = (float)total_bytes * 8.0f / ((float)timed_frames * (FRAME_MS / 1000.0f));

        printf("\n===== Opus encode benchmark =====\n");
        printf("CPU clock           : %u MHz\n", (unsigned)cpu_mhz);
        printf("Format              : %d Hz mono, %d ms frames (%d samples)\n",
               SAMPLE_RATE_HZ, FRAME_MS, FRAME_SAMPLES);
        printf("Settings            : target %d bps, complexity %d, VBR on, VOIP/voice\n",
               BENCH_BITRATE, BENCH_COMPLEXITY);
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
        printf("Opus modes used     : SILK %u, hybrid %u, CELT %u frames\n",
               (unsigned)silk_frames, (unsigned)hybrid_frames, (unsigned)celt_frames);
        printf("Output              : %.1f bytes/frame avg, effective %.0f bps\n",
               (float)total_bytes / (float)timed_frames, bitrate);
        printf("Output hash (FNV-1a): 0x%08" PRIX32 "   (must not change after optimizing)\n", ref_hash);
        printf("Deterministic       : %s\n", deterministic ? "YES (all passes identical)" : "NO - investigate!");
        printf("Encoder state size  : %d bytes (opus_encoder_get_size)\n", opus_encoder_get_size(1));
        printf("Heap used by create : %u bytes\n", (unsigned)(heap_before - heap_after));
        printf("Heap free / min     : %u / %u bytes\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
        printf("Task stack          : %u bytes allocated, %u bytes never used\n",
               (unsigned)BENCH_TASK_STACK_BYTES, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }

    opus_encoder_destroy(enc);
    printf("\nDone. Press EN to run again.\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Pin to core 1 so system and Wi-Fi housekeeping on core 0 disturbs the timing less.
    xTaskCreatePinnedToCore(bench_task, "opus_bench", BENCH_TASK_STACK_BYTES,
                            NULL, 5, NULL, 1);
}