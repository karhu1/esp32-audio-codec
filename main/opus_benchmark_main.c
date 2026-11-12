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
#include "esp_timer.h"      // esp_timer_get_time()
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

// How many times the complexity-0 measurement is repeated in one boot to measure
// run-to-run spread. Each run is ~2.5 s, so 10 runs is ~25 s.
#define BASELINE_RUNS         10

// Task stack. Higher complexity may use more stack than complexity 0 did, so this
// is larger than the 16 KB v1 used. The result block prints how much was never used.
#define BENCH_TASK_STACK_BYTES  (24 * 1024)

#define NUM_HARMONICS         30
#define TWO_PI_F              6.28318530718f

// ---- Buffers (static, so they live in internal RAM, not on a stack) --------
static int16_t  g_pcm[NUM_FRAMES * FRAME_SAMPLES];
static uint8_t  g_pkt[1276];                 // Single-core packet buffer


// Per-frame statistics (reused for every complexity level).
static uint32_t g_frame_cycle_sum[NUM_FRAMES];   // summed over the timed passes
static uint16_t g_frame_bytes[NUM_FRAMES];       // packet size (identical every pass)
static uint8_t  g_frame_config[NUM_FRAMES];      // Opus TOC config number (mode/bandwidth)
/* Preserve complexity 0 data for the JSON block, which is printed after the sweep. */
static uint32_t g_c0_frame_cycle_sum[NUM_FRAMES];
static uint16_t g_c0_frame_bytes[NUM_FRAMES];
static uint8_t  g_c0_frame_config[NUM_FRAMES];

typedef struct {
    size_t dram_free;
    size_t dram_largest;
    size_t exec_free;
    size_t exec_largest;
    UBaseType_t stack_unused;
} memory_snapshot_t;

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
    memory_snapshot_t memory_before_create;
    memory_snapshot_t memory_after_create;
    memory_snapshot_t memory_after_encode;
    memory_snapshot_t memory_after_destroy;
} result_t;

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

static result_t               g_res[NUM_COMPLEXITIES];
static dual_pipeline_result_t g_dual_pipe_res;
static opus_bench_profile_t   g_stage_profile;



static memory_snapshot_t capture_memory_snapshot(void)
{
    const uint32_t dram_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t exec_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_EXEC;
    memory_snapshot_t snapshot = {
        .dram_free = heap_caps_get_free_size(dram_caps),
        .dram_largest = heap_caps_get_largest_free_block(dram_caps),
        .exec_free = heap_caps_get_free_size(exec_caps),
        .exec_largest = heap_caps_get_largest_free_block(exec_caps),
        .stack_unused = uxTaskGetStackHighWaterMark(NULL),
    };
    return snapshot;
}

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

/* Print a JSON string with escaping so toolchain/version text cannot corrupt the
 * machine-readable result block. */
static void print_json_string(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            putchar('\\');
            putchar(c);
        } else if (c == '\n') printf("\\n");
        else if (c == '\r') printf("\\r");
        else if (c == '\t') printf("\\t");
        else if (c >= 0x20) putchar(c);
    }
    putchar('"');
}

static void print_memory_record(int complexity, const char *stage,
                                const memory_snapshot_t *snapshot)
{
    printf("{\"type\":\"memory\",\"complexity\":%d,\"stage\":\"%s\","
           "\"dram_8bit_internal_free_bytes\":%u,\"dram_8bit_internal_largest_block_bytes\":%u,"
           "\"executable_internal_free_bytes\":%u,\"executable_internal_largest_block_bytes\":%u,"
           "\"task_stack_unused_bytes\":%u}\n",
           complexity, stage,
           (unsigned)snapshot->dram_free, (unsigned)snapshot->dram_largest,
           (unsigned)snapshot->exec_free, (unsigned)snapshot->exec_largest,
           (unsigned)snapshot->stack_unused);
}

/* Delimited JSON Lines block: monitor noise outside the markers is ignored. */
static void print_machine_results(const uint32_t cpu_mhz)
{
    printf("\n===OPUS_BENCH_JSON_V1_BEGIN===\n");
    printf("{\"type\":\"run\",\"schema\":1,\"cpu_mhz\":%u,\"sample_rate_hz\":%d,\"channels\":1,\"frame_ms\":%d,\"frame_samples\":%d,\"target_bitrate\":%d,\"passes\":%d,\"frames_per_pass\":%d,\"benchmark_task_stack_bytes\":%u,\"optimization\":",
           (unsigned)cpu_mhz, SAMPLE_RATE_HZ, FRAME_MS, FRAME_SAMPLES,
           BENCH_BITRATE, NUM_PASSES, NUM_FRAMES, (unsigned)BENCH_TASK_STACK_BYTES);
    print_json_string(opt_level_name());
    printf(",\"opus_version\":");
    print_json_string(opus_get_version_string());
    printf("}\n");

    for (int i = 0; i < NUM_COMPLEXITIES; i++) {
        const result_t *r = &g_res[i];
        printf("{\"type\":\"summary\",\"complexity\":%d,\"ok\":%s,\"timed_frames\":%" PRIu32,
               r->complexity, r->ok ? "true" : "false", r->timed_frames);
        if (r->ok && r->timed_frames > 0) {
            printf(",\"min_cycles\":%" PRIu32 ",\"avg_cycles\":%" PRIu32
                   ",\"max_cycles\":%" PRIu32 ",\"worst_frame\":%" PRIu32
                   ",\"total_bytes\":%" PRIu32 ",\"silk_frames\":%" PRIu32
                   ",\"hybrid_frames\":%" PRIu32 ",\"celt_frames\":%" PRIu32
                   ",\"hash\":\"0x%08" PRIX32 "\",\"deterministic\":%s",
                   r->min_cycles, r->avg_cycles, r->max_cycles, r->worst_frame,
                   r->total_bytes, r->silk, r->hybrid, r->celt, r->hash,
                   r->deterministic ? "true" : "false");
        }
        printf("}\n");
        if (r->ok && r->timed_frames > 0) {
            print_memory_record(r->complexity, "before_create", &r->memory_before_create);
            print_memory_record(r->complexity, "after_create", &r->memory_after_create);
            print_memory_record(r->complexity, "after_encode", &r->memory_after_encode);
            print_memory_record(r->complexity, "after_destroy", &r->memory_after_destroy);
        }
    }

    if (g_res[0].ok && g_res[0].complexity == 0) {
        for (int f = 0; f < NUM_FRAMES; f++) {
            uint32_t avg_cycles = g_c0_frame_cycle_sum[f] / NUM_PASSES;
            printf("{\"type\":\"frame\",\"complexity\":0,\"frame\":%d,\"avg_cycles\":%" PRIu32
                   ",\"avg_us\":%.2f,\"bytes\":%u,\"toc_config\":%u}\n",
                   f, avg_cycles, (float)avg_cycles / (float)cpu_mhz,
                   (unsigned)g_c0_frame_bytes[f], (unsigned)g_c0_frame_config[f]);
        }
    }
    const dual_pipeline_result_t *pipe = &g_dual_pipe_res;
    printf("{\"type\":\"dual_pipeline\",\"complexity\":%d,\"ok\":%s",
           pipe->complexity, pipe->ok ? "true" : "false");
    if (pipe->ok) {
        printf(",\"core0_staging_avg_cycles\":%" PRIu32
               ",\"core1_encode_avg_cycles\":%" PRIu32
               ",\"core1_encode_max_cycles\":%" PRIu32
               ",\"wall_avg_us_per_frame\":%" PRIu32
               ",\"hash\":\"0x%08" PRIX32 "\",\"deterministic\":%s",
               pipe->core0_avg_cycles, pipe->core1_avg_cycles,
               pipe->core1_max_cycles, pipe->wall_avg_us,
               pipe->hash, pipe->deterministic ? "true" : "false");
    }
    printf("}\n");
    printf("===OPUS_BENCH_JSON_V1_END===\n");
}

// ---- One complexity level --------------------------------------------------
static void run_config(int complexity, result_t *r)
{
    *r = (result_t){ .complexity = complexity, .ok = true, .min_cycles = UINT32_MAX,
                     .deterministic = true };
    uint64_t sum_cycles = 0;
    uint32_t ref_hash = 0;

    for (int f = 0; f < NUM_FRAMES; f++) g_frame_cycle_sum[f] = 0;

    r->memory_before_create = capture_memory_snapshot();
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(SAMPLE_RATE_HZ, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || enc == NULL) {
        printf("opus_encoder_create failed (complexity %d): %s\n", complexity, opus_strerror(err));
        r->ok = false;
        return;
    }
    r->memory_after_create = capture_memory_snapshot();
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

    r->memory_after_encode = capture_memory_snapshot();
    opus_encoder_destroy(enc);
    r->memory_after_destroy = capture_memory_snapshot();
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

// ---- Benchmark task --------------------------------------------------------
// ---- Dual-Core Producer-Consumer Handoff ----------------------------------
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
    if (ctx->abort) {
        xEventGroupSetBits(ctx->eg, PIPE_CONS_DONE);
        vTaskDelete(NULL);
        return;
    }

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
    uint64_t sum_wall_us = 0;
    uint32_t max_cycles = 0;
    uint32_t ref_hash = 0;

    for (int pass = -1; pass < NUM_PASSES && r->ok; pass++) {
        opus_encoder_ctl(enc, OPUS_RESET_STATE);
        uint32_t hash = 2166136261u;
        int64_t pass_start_us = (pass >= 0) ? esp_timer_get_time() : 0;

        for (int f = 0; f < NUM_FRAMES; f++) {
            // Wait for core 0 to stage frame f in ping_pong_pcm[slot]
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            int slot = ctx->slot_ready;
            // Immediately release core 0 to stage frame f+1 in the other slot
            xTaskNotifyGive(ctx->prod_handle);

            uint32_t t0 = esp_cpu_get_cycle_count();
            int n = opus_encode(enc, ctx->ping_pong_pcm[slot], FRAME_SAMPLES,
                                g_pkt, (opus_int32)sizeof(g_pkt));
            uint32_t t1 = esp_cpu_get_cycle_count();

            if (n < 0) {
                printf("dual pipeline core 1 complexity %d: opus_encode failed on pass %d frame %d: %s (%d)\n",
                       ctx->complexity, pass, f, opus_strerror(n), n);
                r->ok = false;
                ctx->abort = true;
                xTaskNotifyGive(ctx->prod_handle);   // unblock the producer so it can exit
                break;
            }

            hash = fnv1a(hash, g_pkt, n);
            if (pass >= 0) {
                uint32_t c = t1 - t0;
                sum_cycles += c;
                if (c > max_cycles) max_cycles = c;
            }
        }
        if (pass >= 0 && r->ok) sum_wall_us += (uint64_t)(esp_timer_get_time() - pass_start_us);
        if (pass == -1) ref_hash = hash;
        else if (hash != ref_hash) r->deterministic = false;
    }

    r->core1_avg_cycles = (uint32_t)(sum_cycles / (NUM_PASSES * NUM_FRAMES));
    r->core1_max_cycles = max_cycles;
    r->wall_avg_us = (uint32_t)(sum_wall_us / (NUM_PASSES * NUM_FRAMES));
    r->hash = ref_hash;

    opus_encoder_destroy(enc);
    xEventGroupSetBits(ctx->eg, PIPE_CONS_DONE);
    vTaskDelete(NULL);
}

static void dual_pipe_producer_core0(void *arg)
{
    pipe_ctx_t *ctx = (pipe_ctx_t *)arg;
    xEventGroupWaitBits(ctx->eg, PIPE_GO, pdFALSE, pdTRUE, portMAX_DELAY);
    if (ctx->abort) {
        xEventGroupSetBits(ctx->eg, PIPE_PROD_DONE);
        vTaskDelete(NULL);
        return;
    }

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
    if (ctx.eg == NULL) {
        r->ok = false;
        return;
    }

    // The calling task (bench_task) runs on core 1, so the producer must be its own task on core 0.
    BaseType_t consumer_created = xTaskCreatePinnedToCore(dual_pipe_consumer_core1,
                                                           "pipe_c1", BENCH_TASK_STACK_BYTES,
                                                           &ctx, 5, &ctx.cons_handle, 1);
    BaseType_t producer_created = xTaskCreatePinnedToCore(dual_pipe_producer_core0,
                                                           "pipe_c0", 4096,
                                                           &ctx, 5, &ctx.prod_handle, 0);
    if (consumer_created != pdPASS || producer_created != pdPASS) {
        r->ok = false;
        ctx.abort = true;
        EventBits_t missing_done = 0;
        if (consumer_created != pdPASS) missing_done |= PIPE_CONS_DONE;
        if (producer_created != pdPASS) missing_done |= PIPE_PROD_DONE;
        xEventGroupSetBits(ctx.eg, PIPE_GO | missing_done);
    } else {
        xEventGroupSetBits(ctx.eg, PIPE_GO);
    }
    xEventGroupWaitBits(ctx.eg, PIPE_PROD_DONE | PIPE_CONS_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
    vEventGroupDelete(ctx.eg);

}


// ---- Repeated complexity-0 baseline ----------------------------------------
// Repeats the complexity-0 measurement BASELINE_RUNS times in one boot so the
// run-to-run spread is measured rather than guessed. Each run builds a fresh
// encoder and does 1 warm-up pass + NUM_PASSES timed passes, exactly like the sweep.
static void sort_u32(uint32_t *v, int n)
{
    for (int i = 1; i < n; i++) {
        uint32_t key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; j--; }
        v[j + 1] = key;
    }
}

static void run_baseline_repeats(const uint32_t cpu_mhz)
{
    static uint32_t run_avg[BASELINE_RUNS];
    static uint32_t sorted[BASELINE_RUNS];
    uint32_t overall_min = UINT32_MAX, overall_max = 0;
    int n = 0;
    bool all_match = true, all_det = true;

    printf("\n===== Complexity-0 baseline: %d repeated runs =====\n", BASELINE_RUNS);
    printf("Conditions : %u MHz, %s, Opus %s\n", (unsigned)cpu_mhz, opt_level_name(),
           opus_get_version_string());
    printf("Settings   : %d Hz mono, %d ms frames, %d bps VBR, VOIP/voice, complexity 0\n",
           SAMPLE_RATE_HZ, FRAME_MS, BENCH_BITRATE);
    printf("Per run    : 1 warm-up pass + %d timed passes of %d frames\n", NUM_PASSES, NUM_FRAMES);
    printf("Note       : Opus stage-profile hooks are compiled in (components/opus/CMakeLists.txt)\n");
    printf("run, avg_cycles, min_cycles, max_cycles, hash, deterministic\n");

    for (int i = 0; i < BASELINE_RUNS; i++) {
        result_t r;
        run_config(0, &r);
        if (!r.ok || r.timed_frames == 0) {
            printf("%d, (failed)\n", i);
            break;
        }
        run_avg[n++] = r.avg_cycles;
        if (r.min_cycles < overall_min) overall_min = r.min_cycles;
        if (r.max_cycles > overall_max) overall_max = r.max_cycles;
        if (r.hash != BASELINE_HASH_C0) all_match = false;
        if (!r.deterministic) all_det = false;
        printf("%d, %" PRIu32 ", %" PRIu32 ", %" PRIu32 ", 0x%08" PRIX32 ", %s\n",
               i, r.avg_cycles, r.min_cycles, r.max_cycles, r.hash,
               r.deterministic ? "yes" : "NO");
        vTaskDelay(pdMS_TO_TICKS(50));   // let other tasks and the watchdog run between runs
    }

    if (n < 2) {
        printf("Baseline summary: not enough successful runs (%d).\n", n);
        return;
    }

    for (int i = 0; i < n; i++) sorted[i] = run_avg[i];
    sort_u32(sorted, n);
    uint32_t median = (n & 1) ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
    uint32_t avg_min = sorted[0], avg_max = sorted[n - 1];

    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += run_avg[i];
    mean /= n;
    double var = 0.0;
    for (int i = 0; i < n; i++) var += ((double)run_avg[i] - mean) * ((double)run_avg[i] - mean);
    double stddev = sqrt(var / (n - 1));

    printf("\n--- Baseline summary (complexity 0, %d runs) ---\n", n);
    printf("Median of run averages : %" PRIu32 " cycles (%.1f us)\n", median, (double)median / cpu_mhz);
    printf("Run-average range      : %" PRIu32 " .. %" PRIu32 " cycles (spread %.2f %% of median)\n",
           avg_min, avg_max, 100.0 * (double)(avg_max - avg_min) / (double)median);
    printf("Mean +/- stddev        : %.0f +/- %.0f cycles (%.2f %%)\n", mean, stddev, 100.0 * stddev / mean);
    printf("Fastest / slowest frame: %" PRIu32 " / %" PRIu32 " cycles (%.1f us worst, budget %d us)\n",
           overall_min, overall_max, (double)overall_max / cpu_mhz, FRAME_MS * 1000);
    printf("Output hash            : %s (expected 0x%08X), %s\n",
           all_match ? "all runs MATCH" : "MISMATCH in at least one run",
           (unsigned)BASELINE_HASH_C0, all_det ? "deterministic" : "NOT deterministic");
    printf("Rule of thumb: a change only counts as a real improvement if its median beats the\n"
           "median above by more than the run-average range.\n");
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
        if (k_complexities[i] == 0) {
            for (int f = 0; f < NUM_FRAMES; f++) {
                g_c0_frame_cycle_sum[f] = g_frame_cycle_sum[f];
                g_c0_frame_bytes[f] = g_frame_bytes[f];
                g_c0_frame_config[f] = g_frame_config[f];
            }
        }
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

    run_baseline_repeats(cpu_mhz);

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
        printf("Handoff pipeline wall avg   : %" PRIu32 " us/frame\n", g_dual_pipe_res.wall_avg_us);
        printf("Handoff hash check          : 0x%08" PRIX32 " -> %s, %s\n",
               g_dual_pipe_res.hash,
               g_dual_pipe_res.hash == BASELINE_HASH_C0 ? "MATCH" : "DIFFERENT",
               g_dual_pipe_res.deterministic ? "deterministic" : "NOT deterministic");
        printf("(Core 0 only stages PCM; core 1 runs the whole encode. This measures handoff overhead, not a split of Opus stages.)\n");
    }
    print_machine_results(cpu_mhz);

    printf("\nDone. Press EN to run again.\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Pin to core 1 so system and Wi-Fi housekeeping on core 0 disturbs the timing less.
    xTaskCreatePinnedToCore(bench_task, "opus_bench", BENCH_TASK_STACK_BYTES,
                            NULL, 5, NULL, 1);
}