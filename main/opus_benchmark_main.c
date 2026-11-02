/*
 * ESP32 Opus encoder benchmark, v2: complexity sweep + per-frame log
 * ------------------------------------------------------------------
 * Encodes a deterministic, speech-like test signal with libopus at several
 * complexity levels and reports, for each one:
 *   - CPU cycles per 20 ms frame (min / avg / max) and the real-time check
 *   - bytes per frame and effective bitrate
 *   - which Opus modes were used (SILK / hybrid / CELT)
 *   - an FNV-1a hash of the encoded bytes (the correctness reference)
 *
 * It also prints, for one chosen complexity, the average cycles and the packet
 * size of every frame, so you can see WHY the cost varies from frame to frame.
 *
 * Complexity 0 uses exactly the same settings as the v1 baseline, so its hash
 * should still equal 0x317FC6D7 on an unmodified build. Runs once per reset
 * (press EN to run again).
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <math.h>

#include "sdkconfig.h"
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

// Complexity levels to sweep (0 = fastest, 10 = best quality).
static const int k_complexities[] = {0, 2, 5, 10};
#define NUM_COMPLEXITIES      ((int)(sizeof(k_complexities) / sizeof(k_complexities[0])))

// Print the per-frame table for this complexity (set to -1 to disable).
#define PER_FRAME_LOG_COMPLEXITY   0

// Hash produced by the v1 baseline at complexity 0 (unmodified micro-opus build).
#define BASELINE_HASH_C0      0x317FC6D7u

// Task stack. Higher complexity may use more stack than complexity 0 did, so this
// is larger than the 16 KB v1 used. The result block prints how much was never used.
#define BENCH_TASK_STACK_BYTES  (24 * 1024)

#define NUM_HARMONICS         30
#define TWO_PI_F              6.28318530718f

// ---- Buffers (static, so they live in internal RAM, not on a stack) --------
static int16_t  g_pcm[NUM_FRAMES * FRAME_SAMPLES];
static uint8_t  g_pkt[1276];                 // 1276 bytes is the largest possible Opus packet

// Per-frame statistics (reused for every complexity level).
static uint32_t g_frame_cycle_sum[NUM_FRAMES];   // summed over the timed passes
static uint16_t g_frame_bytes[NUM_FRAMES];       // packet size (identical every pass)
static uint8_t  g_frame_config[NUM_FRAMES];      // Opus TOC config number (mode/bandwidth)

typedef struct {
    int      complexity;
    bool     ok;
    uint32_t min_cycles, avg_cycles, max_cycles;
    uint32_t worst_frame;
    uint32_t timed_frames, total_bytes;
    uint32_t silk, hybrid, celt;
    uint32_t hash;
    bool     deterministic;
    int      enc_size;
    size_t   heap_used;
} result_t;

static result_t g_res[NUM_COMPLEXITIES];

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

// Which compiler optimization level this firmware was built with (recorded next
// to every measurement so results stay interpretable).
static const char *opt_level_name(void)
{
#if defined(CONFIG_COMPILER_OPTIMIZATION_DEBUG)
    return "-Og (debug)";
#elif defined(CONFIG_COMPILER_OPTIMIZATION_PERF)
    return "-O2 (performance)";
#elif defined(CONFIG_COMPILER_OPTIMIZATION_SIZE)
    return "-Os (size)";
#elif defined(CONFIG_COMPILER_OPTIMIZATION_NONE)
    return "-O0 (none)";
#else
    return "unknown (check menuconfig)";
#endif
}

// ---- One complexity level --------------------------------------------------
static void run_config(int complexity, result_t *r)
{
    *r = (result_t){ .complexity = complexity, .ok = true, .min_cycles = UINT32_MAX,
                     .deterministic = true };
    uint64_t sum_cycles = 0;
    uint32_t ref_hash = 0;

    for (int f = 0; f < NUM_FRAMES; f++) g_frame_cycle_sum[f] = 0;

    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(SAMPLE_RATE_HZ, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || enc == NULL) {
        printf("opus_encoder_create failed (complexity %d): %s\n", complexity, opus_strerror(err));
        r->ok = false;
        return;
    }
    r->heap_used = heap_before - heap_caps_get_free_size(MALLOC_CAP_8BIT);
    r->enc_size  = opus_encoder_get_size(1);

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(BENCH_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    for (int pass = -1; pass < NUM_PASSES && r->ok; pass++) {
        opus_encoder_ctl(enc, OPUS_RESET_STATE);   // every pass starts from the same state
        uint32_t hash = 2166136261u;

        for (int f = 0; f < NUM_FRAMES; f++) {
            const int16_t *in = &g_pcm[f * FRAME_SAMPLES];

            uint32_t t0 = esp_cpu_get_cycle_count();
            int n = opus_encode(enc, in, FRAME_SAMPLES, g_pkt, (opus_int32)sizeof(g_pkt));
            uint32_t t1 = esp_cpu_get_cycle_count();

            if (n < 0) {
                printf("opus_encode failed (complexity %d): %s\n", complexity, opus_strerror(n));
                r->ok = false;
                break;
            }

            hash = fnv1a(hash, g_pkt, n);

            if (pass >= 0) {
                uint32_t cycles = t1 - t0;          // unsigned math handles counter wraparound
                if (cycles < r->min_cycles) r->min_cycles = cycles;
                if (cycles > r->max_cycles) { r->max_cycles = cycles; r->worst_frame = (uint32_t)f; }
                sum_cycles += cycles;
                g_frame_cycle_sum[f] += cycles;
                r->timed_frames++;
                r->total_bytes += (uint32_t)n;

                int config = g_pkt[0] >> 3;         // Opus TOC byte: 0-11 SILK, 12-15 hybrid, 16-31 CELT
                if (config < 12)      r->silk++;
                else if (config < 16) r->hybrid++;
                else                  r->celt++;

                g_frame_bytes[f]  = (uint16_t)n;
                g_frame_config[f] = (uint8_t)config;
            }

            // Let other tasks (and the watchdog) run, outside the timed region.
            if ((f % 10) == 9) vTaskDelay(1);
        }

        if (pass == -1)          ref_hash = hash;
        else if (hash != ref_hash) r->deterministic = false;
    }

    r->hash = ref_hash;
    if (r->timed_frames > 0) r->avg_cycles = (uint32_t)(sum_cycles / r->timed_frames);

    opus_encoder_destroy(enc);
}

static void print_per_frame(int complexity, const uint32_t cpu_mhz)
{
    printf("\n--- Per-frame detail, complexity %d (cycles averaged over %d passes) ---\n",
           complexity, NUM_PASSES);
    printf("frame, avg_cycles, avg_us, bytes, toc_config\n");
    for (int f = 0; f < NUM_FRAMES; f++) {
        uint32_t avg = g_frame_cycle_sum[f] / NUM_PASSES;
        printf("%d, %" PRIu32 ", %.1f, %u, %u\n", f, avg,
               (float)avg / (float)cpu_mhz, (unsigned)g_frame_bytes[f], (unsigned)g_frame_config[f]);
    }
}

// ---- Benchmark task --------------------------------------------------------
static void bench_task(void *arg)
{
    (void)arg;
    const uint32_t cpu_mhz = esp_rom_get_cpu_ticks_per_us();   // real clock, read at run time
    const float budget_us  = (float)(FRAME_MS * 1000);

    printf("\nOpus benchmark v2 starting (libopus %s)...\n", opus_get_version_string());
    printf("Generating test signal...\n");
    generate_test_signal(g_pcm, NUM_FRAMES * FRAME_SAMPLES);

    printf("Sweeping complexity levels; each runs 1 warm-up pass + %d timed passes of %d frames.\n",
           NUM_PASSES, NUM_FRAMES);

    for (int i = 0; i < NUM_COMPLEXITIES; i++) {
        printf("  complexity %d ...\n", k_complexities[i]);
        run_config(k_complexities[i], &g_res[i]);
        if (!g_res[i].ok) break;
        if (k_complexities[i] == PER_FRAME_LOG_COMPLEXITY) print_per_frame(k_complexities[i], cpu_mhz);
    }

    printf("\n===== Opus encode benchmark: complexity sweep =====\n");
    printf("CPU clock           : %u MHz\n", (unsigned)cpu_mhz);
    printf("Compiler opt level  : %s\n", opt_level_name());
    printf("Format              : %d Hz mono, %d ms frames (%d samples)\n",
           SAMPLE_RATE_HZ, FRAME_MS, FRAME_SAMPLES);
    printf("Settings            : target %d bps, VBR on, VOIP/voice\n", BENCH_BITRATE);
    printf("Frames per level    : %d timed (%d passes x %d frames)\n",
           NUM_PASSES * NUM_FRAMES, NUM_PASSES, NUM_FRAMES);
    printf("Real-time budget    : %.0f us per frame\n\n", budget_us);

    printf("cplx | min cycles |  avg cycles | max cycles | avg us | max us | load avg/worst %% | RT | B/frame |  bps  | modes S/H/C | hash       | det\n");
    for (int i = 0; i < NUM_COMPLEXITIES; i++) {
        const result_t *r = &g_res[i];
        if (!r->ok || r->timed_frames == 0) {
            printf("%4d | (failed)\n", r->complexity);
            continue;
        }
        float avg_us = (float)r->avg_cycles / (float)cpu_mhz;
        float max_us = (float)r->max_cycles / (float)cpu_mhz;
        float bpf    = (float)r->total_bytes / (float)r->timed_frames;
        float bps    = bpf * 8.0f / (FRAME_MS / 1000.0f);
        printf("%4d | %10" PRIu32 " | %11" PRIu32 " | %10" PRIu32 " | %6.0f | %6.0f | %6.1f / %-6.1f | %-2s | %7.1f | %5.0f | %u/%u/%u | 0x%08" PRIX32 " | %s\n",
               r->complexity, r->min_cycles, r->avg_cycles, r->max_cycles,
               avg_us, max_us, 100.0f * avg_us / budget_us, 100.0f * max_us / budget_us,
               (max_us < budget_us) ? "OK" : (avg_us < budget_us ? "avg" : "NO"),
               bpf, bps, (unsigned)r->silk, (unsigned)r->hybrid, (unsigned)r->celt,
               r->hash, r->deterministic ? "yes" : "NO");
    }
    printf("(RT: OK = even the slowest frame fits the budget; avg = only the average fits; NO = too slow)\n");

    if (g_res[0].ok && g_res[0].complexity == 0) {
        printf("\nBaseline check (complexity 0): hash 0x%08" PRIX32 " vs v1 baseline 0x%08X -> %s\n",
               g_res[0].hash, (unsigned)BASELINE_HASH_C0,
               g_res[0].hash == BASELINE_HASH_C0 ? "MATCH" : "DIFFERENT (expected only if you changed the encoder, build or settings)");
    }

    printf("\nEncoder state size  : %d bytes (opus_encoder_get_size)\n", g_res[0].enc_size);
    printf("Heap used by create : %u bytes\n", (unsigned)g_res[0].heap_used);
    printf("Heap free / min     : %u / %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    printf("Task stack          : %u bytes allocated, %u bytes never used\n",
           (unsigned)BENCH_TASK_STACK_BYTES, (unsigned)uxTaskGetStackHighWaterMark(NULL));

    printf("\nDone. Press EN to run again.\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Pin to core 1 so system and Wi-Fi housekeeping on core 0 disturbs the timing less.
    xTaskCreatePinnedToCore(bench_task, "opus_bench", BENCH_TASK_STACK_BYTES,
                            NULL, 5, NULL, 1);
}