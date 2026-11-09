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
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_cpu.h"        // esp_cpu_get_cycle_count()
#include "esp_rom_sys.h"    // esp_rom_get_cpu_ticks_per_us()
#include "esp_heap_caps.h"  // heap_caps_get_free_size()
#include "opus.h"
#include "opus_bench_profile.h"

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

// Capture the SILK stage profile for complexity 0 (in-scope optimization target).
#define STAGE_PROFILE_COMPLEXITY   0

// Hash produced by the v1 baseline at complexity 0 (unmodified micro-opus build).
#define BASELINE_HASH_C0      0x317FC6D7u

// Task stack. Higher complexity may use more stack than complexity 0 did, so this
// is larger than the 16 KB v1 used. The result block prints how much was never used.
#define BENCH_TASK_STACK_BYTES  (24 * 1024)

#define NUM_HARMONICS         30
#define TWO_PI_F              6.28318530718f

// ---- Buffers (static, so they live in internal RAM, not on a stack) --------
static int16_t  g_pcm[NUM_FRAMES * FRAME_SAMPLES];
static uint8_t  g_pkt[1276];                 // Single-core packet buffer
static uint8_t  g_pkt_dual[2][1276];         // Per-core packet buffers for dual-core runs

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

typedef struct {
    int      complexity;
    bool     ok;
    result_t core[2];                // Individual metrics for Core 0 (PRO) and Core 1 (APP)
    uint32_t wall_avg_us_per_frame;  // Slower core's average encode time; both cores overlap, so this is the time per frame pair
    float    throughput_streams;     // Real-time streams supported, from the slower core's average
    float    throughput_streams_worst; // Same, from the slower core's worst frame
    float    contention_pct[2];      // Per-core cycle overhead vs. that same core encoding alone
    size_t   dual_heap_used;         // Includes the two worker task stacks, not just the encoders
    size_t   min_heap_free;
} dual_parallel_result_t;

typedef struct {
    int      complexity;
    bool     ok;
    uint32_t core0_avg_cycles;       // Core 0 (PRO): frame staging / pre-processing
    uint32_t core1_avg_cycles;       // Core 1 (APP): opus_encode()
    uint32_t core1_max_cycles;
    uint32_t wall_avg_us;            // Wall-clock time per pipelined frame
    uint32_t hash;
    bool     deterministic;
} dual_pipeline_result_t;

static result_t               g_res[NUM_COMPLEXITIES];        // single stream, encoding on core 1
static result_t               g_core0_res[NUM_COMPLEXITIES];  // single stream, encoding on core 0
static dual_parallel_result_t g_dual_par_res[NUM_COMPLEXITIES];
static dual_pipeline_result_t g_dual_pipe_res;
static opus_bench_profile_t   g_stage_profile;
static opus_bench_profile_t   g_dual_stage_profile[2];



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

        if (pass == -1) {
            ref_hash = hash;
            /* Exclude warm-up work from the reported stage profile. */
            opus_bench_profile_reset();
        }
        else if (hash != ref_hash) r->deterministic = false;
    }

    // Keep only the profile of the in-scope complexity; later levels reset and refill the counters.
    if (complexity == STAGE_PROFILE_COMPLEXITY) opus_bench_profile_read(&g_stage_profile);

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

static const char *const k_stage_names[OPUS_BENCH_STAGE_COUNT] = {
    "pitch analysis", "noise-shape analysis", "predictor search",
    "gain processing", "NSQ quantizer", "entropy encoding", "LBRR total",
    "predictor: LTP", "predictor: LPC", "predictor: NLSF quantization",
    "predictor: residual energy", "SILK frame total", "SILK API call total",
    "Opus tonality analysis", "Opus frame processing",
    "tonality: feature analysis", "tonality: result aggregation",
    "tonality: FFT", "tonality: phase/atan2 loop", "tonality: neural model",
    "tonality: input preparation", "tonality: post-phase features",
    "tonality: band statistics", "tonality: leakage", "tonality: spectral variability",
    "tonality: bandwidth", "tonality: DCT/features"
};

// Index into k_complexities of STAGE_PROFILE_COMPLEXITY, or -1 if it is not in the sweep.
static int stage_profile_index(void)
{
    for (int i = 0; i < NUM_COMPLEXITIES; i++) {
        if (k_complexities[i] == STAGE_PROFILE_COMPLEXITY) return i;
    }
    return -1;
}

static uint64_t stage_avg(const opus_bench_profile_t *p, int stage)
{
    return p->calls[stage] ? p->cycles[stage] / p->calls[stage] : 0;
}

static void print_stage_profile(void)
{
    int prof_idx = stage_profile_index();
    if (prof_idx < 0 || !g_res[prof_idx].ok) return;

    printf("\n--- SILK stage profile, complexity %d (%u timed frames, single stream) ---\n",
           STAGE_PROFILE_COMPLEXITY, (unsigned)g_res[prof_idx].timed_frames);
    printf("stage, total_cycles, avg_cycles/call, calls\n");
    for (int i = 0; i < OPUS_BENCH_STAGE_COUNT; i++) {
        printf("%s, %" PRIu64 ", %" PRIu64 ", %" PRIu32 "\n",
               k_stage_names[i], g_stage_profile.cycles[i], stage_avg(&g_stage_profile, i),
               g_stage_profile.calls[i]);
    }
    printf("(Parent and child totals overlap: predictor, SILK frame, SILK API, and Opus frame totals are inclusive.)\n");
}

// Per-stage cost with both cores encoding at once, next to the single-stream run on core 1,
// so the stages that slow down under shared flash/SRAM contention stand out.
static void print_dual_stage_profile(void)
{
    int prof_idx = stage_profile_index();
    if (prof_idx < 0 || !g_dual_par_res[prof_idx].ok || !g_res[prof_idx].ok) return;

    printf("\n--- SILK stage profile, complexity %d, both cores encoding at once (avg cycles/call) ---\n",
           STAGE_PROFILE_COMPLEXITY);
    printf("stage, core0 dual, core1 dual, core1 solo\n");
    for (int i = 0; i < OPUS_BENCH_STAGE_COUNT; i++) {
        if (g_dual_stage_profile[1].calls[i] == 0 && g_stage_profile.calls[i] == 0) continue;
        printf("%s, %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n", k_stage_names[i],
               stage_avg(&g_dual_stage_profile[0], i), stage_avg(&g_dual_stage_profile[1], i),
               stage_avg(&g_stage_profile, i));
    }
}

// ---- Benchmark task --------------------------------------------------------
// ---- Dual-Core Mode 1: Parallel Dual-Stream Workers (Core 0 + Core 1) ------
#define EVT_C0_READY    (1 << 0)
#define EVT_C1_READY    (1 << 1)
#define EVT_PASS_GO     (1 << 2)
#define EVT_C0_DONE     (1 << 3)
#define EVT_C1_DONE     (1 << 4)
#define EVT_BOTH_READY  (EVT_C0_READY | EVT_C1_READY)
#define EVT_BOTH_DONE   (EVT_C0_DONE  | EVT_C1_DONE)

typedef struct {
    int                   core_id;
    int                   complexity;
    EventGroupHandle_t    eg;
    EventBits_t           ready_all;   // READY bits of every worker taking part in this run
    result_t             *out;
    opus_bench_profile_t *prof;        // if non-NULL, receives this core's timed-pass stage profile
} dual_worker_arg_t;

// Each core's profile counters are written only by that core, so the profile is taken as an
// end-minus-warm-up difference. A reset from one core would wipe the other core's counters.
static void profile_diff(const opus_bench_profile_t *end, const opus_bench_profile_t *base,
                         opus_bench_profile_t *out)
{
    for (int s = 0; s < OPUS_BENCH_STAGE_COUNT; s++) {
        out->cycles[s] = end->cycles[s] - base->cycles[s];
        out->calls[s]  = end->calls[s]  - base->calls[s];
    }
}

static void dual_parallel_worker_task(void *arg)
{
    dual_worker_arg_t *w = (dual_worker_arg_t *)arg;
    result_t *r = w->out;
    const int cid = w->core_id;
    uint8_t *pkt_buf = g_pkt_dual[cid];

    *r = (result_t){ .complexity = w->complexity, .ok = true,
                     .min_cycles = UINT32_MAX, .deterministic = true };

    int err = 0;
    OpusEncoder *enc = opus_encoder_create(SAMPLE_RATE_HZ, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || enc == NULL) {
        r->ok = false;
        xEventGroupSetBits(w->eg, (cid == 0) ? EVT_C0_READY | EVT_C0_DONE : EVT_C1_READY | EVT_C1_DONE);
        vTaskDelete(NULL);
        return;
    }

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(BENCH_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(w->complexity));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    // Synchronize all participating cores before starting the pass sweep
    EventBits_t ready_bit = (cid == 0) ? EVT_C0_READY : EVT_C1_READY;
    xEventGroupSync(w->eg, ready_bit, w->ready_all | EVT_PASS_GO, portMAX_DELAY);

    uint64_t sum_cycles = 0;
    uint32_t ref_hash = 0;
    opus_bench_profile_t prof_base = {0};

    for (int pass = -1; pass < NUM_PASSES && r->ok; pass++) {
        opus_encoder_ctl(enc, OPUS_RESET_STATE);
        uint32_t hash = 2166136261u;

        for (int f = 0; f < NUM_FRAMES; f++) {
            const int16_t *in = &g_pcm[f * FRAME_SAMPLES];

            uint32_t t0 = esp_cpu_get_cycle_count();
            int n = opus_encode(enc, in, FRAME_SAMPLES, pkt_buf, (opus_int32)sizeof(g_pkt_dual[0]));
            uint32_t t1 = esp_cpu_get_cycle_count();

            if (n < 0) {
                r->ok = false;
                break;
            }

            hash = fnv1a(hash, pkt_buf, n);

            if (pass >= 0) {
                uint32_t cycles = t1 - t0;
                if (cycles < r->min_cycles) r->min_cycles = cycles;
                if (cycles > r->max_cycles) { r->max_cycles = cycles; r->worst_frame = (uint32_t)f; }
                sum_cycles += cycles;
                r->timed_frames++;
                r->total_bytes += (uint32_t)n;

                int config = pkt_buf[0] >> 3;
                if (config < 12)      r->silk++;
                else if (config < 16) r->hybrid++;
                else                  r->celt++;
            }

            // Yield outside timed region every 10 frames so IDLE0/IDLE1 watchdogs stay fed
            if ((f % 10) == 9) vTaskDelay(1);
        }

        if (pass == -1) {
            ref_hash = hash;
            if (w->prof) opus_bench_profile_read_core(cid, &prof_base);
        } else if (hash != ref_hash) {
            r->deterministic = false;
        }
    }

    if (w->prof) {
        opus_bench_profile_t prof_end;
        opus_bench_profile_read_core(cid, &prof_end);
        profile_diff(&prof_end, &prof_base, w->prof);
    }

    r->hash = ref_hash;
    if (r->timed_frames > 0) r->avg_cycles = (uint32_t)(sum_cycles / r->timed_frames);

    opus_encoder_destroy(enc);

    EventBits_t done_bit = (cid == 0) ? EVT_C0_DONE : EVT_C1_DONE;
    xEventGroupSetBits(w->eg, done_bit);
    vTaskDelete(NULL);
}

// Runs one encoder worker on each core in `core_mask` (bit 0 = core 0, bit 1 = core 1) at the
// same time. A single-bit mask gives the per-core solo baseline that the dual run is compared to.
static void run_parallel_workers(int complexity, int core_mask, dual_parallel_result_t *dr)
{
    *dr = (dual_parallel_result_t){ .complexity = complexity, .ok = true };
    EventGroupHandle_t eg = xEventGroupCreate();

    const EventBits_t ready_all = ((core_mask & 1) ? EVT_C0_READY : 0) | ((core_mask & 2) ? EVT_C1_READY : 0);
    const EventBits_t done_all  = ((core_mask & 1) ? EVT_C0_DONE  : 0) | ((core_mask & 2) ? EVT_C1_DONE  : 0);
    const bool both = (core_mask == 3);

    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);

    dual_worker_arg_t args[2];
    for (int c = 0; c < 2; c++) {
        if (!(core_mask & (1 << c))) continue;
        args[c] = (dual_worker_arg_t){
            .core_id = c, .complexity = complexity, .eg = eg, .ready_all = ready_all,
            .out  = &dr->core[c],
            .prof = (both && complexity == STAGE_PROFILE_COMPLEXITY) ? &g_dual_stage_profile[c] : NULL,
        };
        xTaskCreatePinnedToCore(dual_parallel_worker_task, c ? "opus_c1" : "opus_c0",
                                BENCH_TASK_STACK_BYTES, &args[c], 5, NULL, c);
    }

    // Wait until every encoder is allocated on heap and waiting at the barrier
    xEventGroupWaitBits(eg, ready_all, pdFALSE, pdTRUE, portMAX_DELAY);
    dr->dual_heap_used = heap_before - heap_caps_get_free_size(MALLOC_CAP_8BIT);
    dr->min_heap_free  = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    xEventGroupSetBits(eg, EVT_PASS_GO);
    xEventGroupWaitBits(eg, done_all, pdTRUE, pdTRUE, portMAX_DELAY);

    vEventGroupDelete(eg);

    for (int c = 0; c < 2; c++) {
        if ((core_mask & (1 << c)) && !dr->core[c].ok) dr->ok = false;
    }
}

// Both cores encode the same input in lockstep, which is the worst case for shared flash/SRAM
// contention; the identical inputs also let both hashes be checked against the single-stream hash.
static void run_dual_core_parallel(int idx, dual_parallel_result_t *dr)
{
    run_parallel_workers(k_complexities[idx], 3, dr);
    if (!dr->ok) return;

    const uint32_t cpu_mhz = esp_rom_get_cpu_ticks_per_us();
    const float budget_us = (float)(FRAME_MS * 1000);
    uint32_t slower_avg = (dr->core[0].avg_cycles > dr->core[1].avg_cycles)
                        ? dr->core[0].avg_cycles : dr->core[1].avg_cycles;
    uint32_t slower_max = (dr->core[0].max_cycles > dr->core[1].max_cycles)
                        ? dr->core[0].max_cycles : dr->core[1].max_cycles;

    // The cores overlap, so one frame pair completes per slower-core encode time.
    dr->wall_avg_us_per_frame      = slower_avg / cpu_mhz;
    dr->throughput_streams         = 2.0f * budget_us / ((float)slower_avg / (float)cpu_mhz);
    dr->throughput_streams_worst   = 2.0f * budget_us / ((float)slower_max / (float)cpu_mhz);

    // Compare each core against itself encoding alone (core 0 is not as quiet as core 1).
    const uint32_t solo_avg[2] = { g_core0_res[idx].avg_cycles, g_res[idx].avg_cycles };
    for (int c = 0; c < 2; c++) {
        if (solo_avg[c] > 0) {
            dr->contention_pct[c] = 100.0f * ((float)dr->core[c].avg_cycles - (float)solo_avg[c]) / (float)solo_avg[c];
        }
    }
}

// ---- Dual-Core Mode 2: Producer-Consumer Ping-Pong Handoff -----------------
// Core 0 stages each PCM frame into a ping-pong buffer while core 1 runs the whole
// opus_encode(). This measures the cross-core handoff only; no Opus work is split
// between the cores.
#define PIPE_GO         (1 << 0)
#define PIPE_PROD_DONE  (1 << 1)
#define PIPE_CONS_DONE  (1 << 2)

typedef struct {
    int16_t                ping_pong_pcm[2][FRAME_SAMPLES];
    volatile int           slot_ready;
    volatile bool          abort;        // set by the consumer when it cannot continue
    int                    complexity;
    EventGroupHandle_t     eg;
    TaskHandle_t           prod_handle;
    TaskHandle_t           cons_handle;
    dual_pipeline_result_t *res;
} pipe_ctx_t;

static void dual_pipe_consumer_core1(void *arg)
{
    pipe_ctx_t *ctx = (pipe_ctx_t *)arg;
    dual_pipeline_result_t *r = ctx->res;

    // Task handles are written by the creator after this task exists; wait until they are valid.
    xEventGroupWaitBits(ctx->eg, PIPE_GO, pdFALSE, pdTRUE, portMAX_DELAY);

    int err = 0;
    OpusEncoder *enc = opus_encoder_create(SAMPLE_RATE_HZ, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || enc == NULL) {
        r->ok = false;
        ctx->abort = true;
        xTaskNotifyGive(ctx->prod_handle);
        xEventGroupSetBits(ctx->eg, PIPE_CONS_DONE);
        vTaskDelete(NULL);
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(BENCH_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(ctx->complexity));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    uint64_t sum_cycles = 0;
    uint32_t max_cycles = 0;
    uint32_t ref_hash = 0;

    for (int pass = -1; pass < NUM_PASSES && r->ok; pass++) {
        opus_encoder_ctl(enc, OPUS_RESET_STATE);
        uint32_t hash = 2166136261u;

        for (int f = 0; f < NUM_FRAMES; f++) {
            // Wait for core 0 to stage frame f in ping_pong_pcm[slot]
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            int slot = ctx->slot_ready;
            // Immediately release core 0 to stage frame f+1 in the other slot
            xTaskNotifyGive(ctx->prod_handle);

            uint32_t t0 = esp_cpu_get_cycle_count();
            int n = opus_encode(enc, ctx->ping_pong_pcm[slot], FRAME_SAMPLES,
                                g_pkt_dual[1], (opus_int32)sizeof(g_pkt_dual[1]));
            uint32_t t1 = esp_cpu_get_cycle_count();

            if (n < 0) {
                r->ok = false;
                ctx->abort = true;
                xTaskNotifyGive(ctx->prod_handle);   // unblock the producer so it can exit
                break;
            }

            hash = fnv1a(hash, g_pkt_dual[1], n);
            if (pass >= 0) {
                uint32_t c = t1 - t0;
                sum_cycles += c;
                if (c > max_cycles) max_cycles = c;
            }
        }
        if (pass == -1) ref_hash = hash;
        else if (hash != ref_hash) r->deterministic = false;
    }

    r->core1_avg_cycles = (uint32_t)(sum_cycles / (NUM_PASSES * NUM_FRAMES));
    r->core1_max_cycles = max_cycles;
    r->hash = ref_hash;

    opus_encoder_destroy(enc);
    xEventGroupSetBits(ctx->eg, PIPE_CONS_DONE);
    vTaskDelete(NULL);
}

static void dual_pipe_producer_core0(void *arg)
{
    pipe_ctx_t *ctx = (pipe_ctx_t *)arg;
    xEventGroupWaitBits(ctx->eg, PIPE_GO, pdFALSE, pdTRUE, portMAX_DELAY);

    uint64_t core0_cycles = 0;
    bool stop = false;
    for (int pass = -1; pass < NUM_PASSES && !stop; pass++) {
        for (int f = 0; f < NUM_FRAMES; f++) {
            int slot = f & 1;
            uint32_t t0 = esp_cpu_get_cycle_count();
            // Core 0 stages/copies the incoming frame into the internal ping-pong SRAM buffer
            memcpy(ctx->ping_pong_pcm[slot], &g_pcm[f * FRAME_SAMPLES], FRAME_SAMPLES * sizeof(int16_t));
            uint32_t t1 = esp_cpu_get_cycle_count();
            if (pass >= 0) core0_cycles += (t1 - t0);

            ctx->slot_ready = slot;
            xTaskNotifyGive(ctx->cons_handle);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // consumer has picked up frame f
            if (ctx->abort) { stop = true; break; }

            // Yield outside the timed region so IDLE0 and its watchdog keep running
            if ((f % 10) == 9) vTaskDelay(1);
        }
    }

    ctx->res->core0_avg_cycles = (uint32_t)(core0_cycles / (NUM_PASSES * NUM_FRAMES));
    xEventGroupSetBits(ctx->eg, PIPE_PROD_DONE);
    vTaskDelete(NULL);
}

static void run_dual_core_pipeline(int complexity, dual_pipeline_result_t *r)
{
    *r = (dual_pipeline_result_t){ .complexity = complexity, .ok = true, .deterministic = true };
    static pipe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.complexity = complexity;
    ctx.res        = r;
    ctx.eg         = xEventGroupCreate();

    // The calling task (bench_task) runs on core 1, so the producer must be its own task on core 0.
    xTaskCreatePinnedToCore(dual_pipe_consumer_core1, "pipe_c1", BENCH_TASK_STACK_BYTES,
                            &ctx, 5, &ctx.cons_handle, 1);
    xTaskCreatePinnedToCore(dual_pipe_producer_core0, "pipe_c0", 4096,
                            &ctx, 5, &ctx.prod_handle, 0);

    xEventGroupSetBits(ctx.eg, PIPE_GO);
    xEventGroupWaitBits(ctx.eg, PIPE_PROD_DONE | PIPE_CONS_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
    vEventGroupDelete(ctx.eg);

    r->wall_avg_us = r->core1_avg_cycles / esp_rom_get_cpu_ticks_per_us();
}


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

    print_stage_profile();

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

    // ---- Run Dual-Core Benchmarks ------------------------------------------
    // Core 1 baselines are the sweep above. Core 0 also runs system tasks, so it gets its own
    // single-stream baseline; otherwise core asymmetry would be reported as contention.
    printf("\nRunning single-stream baselines on core 0...\n");
    for (int i = 0; i < NUM_COMPLEXITIES; i++) {
        printf("  core 0 alone, complexity %d ...\n", k_complexities[i]);
        dual_parallel_result_t solo;
        run_parallel_workers(k_complexities[i], 1, &solo);
        g_core0_res[i] = solo.core[0];
        g_core0_res[i].ok = solo.ok;
    }

    // printf("\nRunning Dual-Core Parallel (Core 0 + Core 1) complexity sweep...\n");
    // for (int i = 0; i < NUM_COMPLEXITIES; i++) {
    //     printf("  dual-core parallel complexity %d ...\n", k_complexities[i]);
    //     run_dual_core_parallel(i, &g_dual_par_res[i]);
    // }

    printf("\n===== Dual-Core Parallel Benchmark (2 Simultaneous Streams: Core 0 + Core 1) =====\n");
    printf("cplx | C0 avg cyc | C1 avg cyc | C0/C1 max us | contention C0/C1 (%%) | eff us/frame | streams avg/worst | both hashes vs single-stream\n");
    for (int i = 0; i < NUM_COMPLEXITIES; i++) {
        const dual_parallel_result_t *dr = &g_dual_par_res[i];
        if (!dr->ok) {
            printf("%4d | (failed - check heap)\n", dr->complexity);
            continue;
        }
        uint32_t c0_max_us = dr->core[0].max_cycles / cpu_mhz;
        uint32_t c1_max_us = dr->core[1].max_cycles / cpu_mhz;
        bool hash_ok = (dr->core[0].hash == g_res[i].hash) && (dr->core[1].hash == g_res[i].hash);
        printf("%4d | %10" PRIu32 " | %10" PRIu32 " | %5" PRIu32 " / %-5" PRIu32 " | %+7.2f / %+-7.2f | %8" PRIu32 " us | %5.2fx / %-5.2fx | %s (0x%08" PRIX32 ")\n",
               dr->complexity,
               dr->core[0].avg_cycles, dr->core[1].avg_cycles,
               c0_max_us, c1_max_us,
               dr->contention_pct[0], dr->contention_pct[1],
               dr->wall_avg_us_per_frame,
               dr->throughput_streams, dr->throughput_streams_worst,
               hash_ok ? "MATCH" : "MISMATCH",
               dr->core[0].hash);
    }
    printf("(Contention = a core's dual-run average vs. that same core encoding alone. Streams = real-time streams\n"
           " the two cores sustain together, from the slower core's average / worst frame. Both cores encode identical\n"
           " input in lockstep, the worst case for shared flash/SRAM contention.)\n");
    printf("Dual-run heap used     : %u bytes incl. the 2 worker task stacks (min free 8-bit heap: %u bytes)\n",
           (unsigned)g_dual_par_res[0].dual_heap_used,
           (unsigned)g_dual_par_res[0].min_heap_free);
    print_dual_stage_profile();

    printf("\nRunning Dual-Core Producer-Consumer Handoff (Complexity 0)...\n");
    run_dual_core_pipeline(0, &g_dual_pipe_res);
    if (!g_dual_pipe_res.ok) {
        printf("Handoff test failed (encoder create or encode error)\n");
    } else {
        printf("Handoff C0 (PRO) staging avg: %" PRIu32 " cycles (%.1f us)\n",
               g_dual_pipe_res.core0_avg_cycles, (float)g_dual_pipe_res.core0_avg_cycles / (float)cpu_mhz);
        printf("Handoff C1 (APP) encode avg : %" PRIu32 " cycles (%.1f us), max: %" PRIu32 " cycles (%.1f us)\n",
               g_dual_pipe_res.core1_avg_cycles, (float)g_dual_pipe_res.core1_avg_cycles / (float)cpu_mhz,
               g_dual_pipe_res.core1_max_cycles, (float)g_dual_pipe_res.core1_max_cycles / (float)cpu_mhz);
        printf("Handoff hash check          : 0x%08" PRIX32 " -> %s, %s\n",
               g_dual_pipe_res.hash,
               g_dual_pipe_res.hash == BASELINE_HASH_C0 ? "MATCH" : "DIFFERENT",
               g_dual_pipe_res.deterministic ? "deterministic" : "NOT deterministic");
        printf("(Core 0 only stages PCM; core 1 runs the whole encode. This measures handoff overhead, not a split of Opus stages.)\n");
    }

    printf("\nDone. Press EN to run again.\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Pin to core 1 so system and Wi-Fi housekeeping on core 0 disturbs the timing less.
    xTaskCreatePinnedToCore(bench_task, "opus_bench", BENCH_TASK_STACK_BYTES,
                            NULL, 5, NULL, 1);
}
