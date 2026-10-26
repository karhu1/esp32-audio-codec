/*
 * ESP32 cycle-count benchmark harness
 * -----------------------------------
 * Generates a 16 kHz test tone, runs a stand-in "codec kernel" on 20 ms
 * frames, and reports cycles / microseconds per frame plus a real-time check.
 *
 * Later, replace kernel_under_test() with an Opus call (e.g. opus_encode())
 * and nothing else in this file needs to change.
 *
 * The stand-in kernel is a fixed-point autocorrelation, which has the same
 * shape as Opus's pitch cross-correlation: int16 multiply-accumulate in a
 * nested loop.
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"   // esp_cpu_get_cycle_count()

// ---- Test parameters -------------------------------------------------------
#define SAMPLE_RATE_HZ   16000
#define FRAME_MS         20
#define FRAME_SAMPLES    (SAMPLE_RATE_HZ * FRAME_MS / 1000)   // 320 samples
#define TONE_HZ          440.0f
#define TONE_AMPLITUDE   12000.0f                             // int16 range is +/-32767

#define NUM_WARMUP       10     // untimed runs to warm the instruction cache
#define NUM_ITERATIONS   200    // timed runs

#define MAX_LAG          80     // correlation lags for the stand-in kernel

// CPU clock in MHz, as set in menuconfig (used to convert cycles to time)
#define CPU_MHZ          CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ

// ---- Buffers (static, so they live in internal DRAM, not on the stack) -----
static int16_t g_frame[FRAME_SAMPLES + MAX_LAG];   // padded so lag reads stay in range
static int32_t g_result[MAX_LAG];

// Written after every run so the compiler can't optimize the work away.
static volatile int32_t g_sink;

// ---- Test signal -----------------------------------------------------------
static void generate_tone(int16_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        buf[i] = (int16_t)(TONE_AMPLITUDE *
                           sinf(2.0f * (float)M_PI * TONE_HZ * (float)i / SAMPLE_RATE_HZ));
    }
}

// ---- The code being measured ----------------------------------------------
// Swap this body for an Opus call once the harness is trusted.
// __attribute__((noinline)) keeps it a real function call so timing is clean.
__attribute__((noinline))
static void kernel_under_test(const int16_t *x, int32_t *out)
{
    for (int lag = 0; lag < MAX_LAG; lag++) {
        int32_t sum = 0;
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            sum += (int32_t)x[i] * (int32_t)x[i + lag] >> 8;   // scaled to avoid overflow
        }
        out[lag] = sum;
    }
}

// ---- Benchmark driver ------------------------------------------------------
static void run_benchmark(void)
{
    uint32_t min_cycles = UINT32_MAX;
    uint32_t max_cycles = 0;
    uint64_t total_cycles = 0;

    // Warm-up: first runs are slower because code is fetched from flash into cache.
    for (int i = 0; i < NUM_WARMUP; i++) {
        kernel_under_test(g_frame, g_result);
        g_sink = g_result[0];
    }

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        uint32_t start = esp_cpu_get_cycle_count();
        kernel_under_test(g_frame, g_result);
        uint32_t end = esp_cpu_get_cycle_count();

        g_sink = g_result[0];

        uint32_t cycles = end - start;   // unsigned subtraction handles counter wraparound
        if (cycles < min_cycles) min_cycles = cycles;
        if (cycles > max_cycles) max_cycles = cycles;
        total_cycles += cycles;
    }

    uint32_t avg_cycles = (uint32_t)(total_cycles / NUM_ITERATIONS);

    // Convert to time. At N MHz, one microsecond is N cycles.
    float avg_us    = (float)avg_cycles / (float)CPU_MHZ;
    float budget_us = (float)(FRAME_MS * 1000);       // time available per frame
    float cpu_load  = 100.0f * avg_us / budget_us;    // % of one core used

    printf("\n===== Benchmark results =====\n");
    printf("CPU clock          : %d MHz\n", CPU_MHZ);
    printf("Frame              : %d samples (%d ms @ %d Hz)\n",
           FRAME_SAMPLES, FRAME_MS, SAMPLE_RATE_HZ);
    printf("Iterations         : %d (after %d warm-up)\n", NUM_ITERATIONS, NUM_WARMUP);
    printf("Cycles/frame       : min %" PRIu32 "  avg %" PRIu32 "  max %" PRIu32 "\n",
           min_cycles, avg_cycles, max_cycles);
    printf("Time/frame (avg)   : %.1f us  (budget %.0f us)\n", avg_us, budget_us);
    printf("Cycles per sample  : %.1f\n", (float)avg_cycles / FRAME_SAMPLES);
    printf("CPU load (1 core)  : %.1f %%\n", cpu_load);
    printf("Real-time capable  : %s\n", avg_us < budget_us ? "YES" : "NO");
    printf("(checksum %" PRId32 ")\n", (int32_t)g_sink);
}

void app_main(void)
{
    printf("\nESP32 benchmark harness starting...\n");

    generate_tone(g_frame, FRAME_SAMPLES + MAX_LAG);

    run_benchmark();

    // Run again every 10 s so you can compare runs without reflashing.
    // for (size_t i = 0; i < 3; i++) {
    //   vTaskDelay(pdMS_TO_TICKS(10000));
    //   run_benchmark();
    // }
}