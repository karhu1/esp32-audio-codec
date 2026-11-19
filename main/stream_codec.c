/*
 * USB-serial Opus streaming. See stream_codec.h for the protocol and data flow.
 *
 * Tasks
 *   rx_task  (core 0): reads the UART, parses frames, queues PCM jobs for core 1
 *   tx_task  (core 0): sends READY / PACKET / DONE / ERROR frames produced by core 1
 *   enc_task (core 1): owns the Opus encoder; the only task that calls opus_*()
 *
 * Message buffers each have exactly one writer and one reader:
 *   g_in  : writer = rx_task, reader = enc_task
 *   g_out : writer = enc_task, reader = tx_task
 * Errors detected on the RX side are sent directly (send_frame is mutex-protected),
 * never through g_out, so g_out keeps a single writer.
 *
 * While a session is active the console UART carries binary data, so nothing in the
 * session path may printf or log.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/message_buffer.h"
#include "driver/uart.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "opus.h"

#include "stream_codec.h"

// ---- Fixed audio format (matches the benchmark) ----------------------------
#define SAMPLE_RATE_HZ      16000
#define FRAME_SAMPLES       320                    // 20 ms
#define FRAME_BYTES         (FRAME_SAMPLES * 2)
#define MAX_PKT             1276                   // largest Opus packet

// ---- Link settings ----------------------------------------------------------
#define UART_PORT           UART_NUM_0
#define BOOT_BAUD           115200
#define MIN_BAUD            115200
#define MAX_BAUD            1000000                // CP2102 limit
#define STREAM_WINDOW       8                      // PCM frames the PC may have in flight
#define PROTO_VERSION       1
#define UART_RX_RING_BYTES  8192                   // must exceed STREAM_WINDOW * 649 bytes (5,192)
#define UART_TX_RING_BYTES  2048
#define SESSION_TIMEOUT_US  (5 * 1000 * 1000)
#define PARSER_STALL_US     (100 * 1000)
// After DONE the board stays at the high baud rate for this long so it can answer a repeated
// END with DONE again. Must exceed the PC's resend window (1 s x 4 resends + 1 s = 5 s).
#define DONE_GRACE_US       (6 * 1000 * 1000)

// Core 1 never blocks while data is queued, so IDLE1 would starve and trip the task
// watchdog (default 5 s). Sleep one tick every N frames to let it run. Lower this if
// you see "Task watchdog got triggered".
#define YIELD_EVERY_FRAMES  200

// ---- Wire constants ---------------------------------------------------------
#define SYNC0               0xA5
#define SYNC1               0x5A
#define HDR_BYTES           7                      // sync(2) + type + seq(2) + len(2)
#define CRC_BYTES           2
#define MAX_RX_PAYLOAD      700

enum { T_HELLO = 0x01, T_PCM = 0x02, T_END = 0x03, T_BYE = 0x04,
       T_READY = 0x81, T_PACKET = 0x82, T_DONE = 0x83, T_ERROR = 0x8F };

enum { ERR_BAD_FRAME = 1, ERR_ENC_CREATE = 2, ERR_ENC_FAIL = 3, ERR_BAD_HELLO = 4,
       ERR_SEQ = 5, ERR_TIMEOUT = 6, ERR_OVERFLOW = 7, ERR_BAD_PCM = 8 };

// ---- Messages between cores -------------------------------------------------
enum { JOB_START = 1, JOB_PCM = 2, JOB_END = 3 };
typedef struct {
    uint8_t  kind;
    uint8_t  complexity;
    uint16_t seq;
    uint32_t bitrate;
    uint32_t baud;
    int16_t  pcm[FRAME_SAMPLES];        // present only for JOB_PCM
} job_t;
#define JOB_HDR_BYTES offsetof(job_t, pcm)

enum { OUT_READY = 1, OUT_PACKET = 2, OUT_DONE = 3, OUT_ERROR = 4 };
typedef struct {
    uint8_t  kind;
    uint8_t  code;                      // OUT_ERROR: error code
    uint16_t seq;
    uint32_t value;                     // READY: baud, PACKET: cycles, ERROR: detail
    uint16_t len;                       // bytes used in data[]
    uint8_t  data[MAX_PKT];             // PACKET: opus bytes, DONE: 6 x u32 stats
} out_t;
#define OUT_HDR_BYTES offsetof(out_t, data)

// ---- Shared state -----------------------------------------------------------
static MessageBufferHandle_t g_in;
static MessageBufferHandle_t g_out;
static SemaphoreHandle_t     g_tx_lock;
static uint8_t               g_txbuf[HDR_BYTES + CRC_BYTES + 4 + MAX_PKT + 16];

static volatile bool    g_hello_pending;   // HELLO accepted, READY not yet sent
static volatile bool    g_active;          // session running at the high baud rate
static volatile bool    g_ending;          // END received, waiting for DONE
static volatile int64_t g_last_rx_us;
static uint16_t         g_expect_seq;
static uint8_t          g_done_pl[24];     // DONE payload kept for resending (6 x u32)
static volatile bool    g_done_grace;      // DONE sent; still at high baud, waiting for BYE
static volatile int64_t g_done_deadline_us;

static uint16_t g_crc_tab[256];

// ---- Small helpers ----------------------------------------------------------
static void crc_init(void)
{
    for (int i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
        g_crc_tab[i] = c;
    }
}

static uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n)
{
    while (n--) crc = (uint16_t)((crc << 8) ^ g_crc_tab[(crc >> 8) ^ *d++]);
    return crc;
}

// ESP32 is little-endian, so memcpy matches the wire format.
static uint32_t get_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void     put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void     put_u16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static uint32_t fnv1a(uint32_t h, const uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
}

/* Builds one frame (payload = a || b) in a shared buffer and writes it with a single call. */
static void send_frame(uint8_t type, uint16_t seq, const void *a, size_t alen,
                       const void *b, size_t blen)
{
    xSemaphoreTake(g_tx_lock, portMAX_DELAY);
    uint8_t *p = g_txbuf;
    size_t len = alen + blen;
    p[0] = SYNC0; p[1] = SYNC1; p[2] = type;
    put_u16(p + 3, seq);
    put_u16(p + 5, (uint16_t)len);
    if (alen) memcpy(p + HDR_BYTES, a, alen);
    if (blen) memcpy(p + HDR_BYTES + alen, b, blen);
    uint16_t crc = crc16_update(0xFFFF, p + 2, 5 + len);
    put_u16(p + HDR_BYTES + len, crc);
    uart_write_bytes(UART_PORT, (const char *)p, HDR_BYTES + len + CRC_BYTES);
    xSemaphoreGive(g_tx_lock);
}

/* Return to the boot baud rate and idle state. Safe to call from either core-0 task. */
static void session_reset(void)
{
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(200));
    uart_set_baudrate(UART_PORT, BOOT_BAUD);
    g_active = false;
    g_ending = false;
    g_hello_pending = false;
    g_done_grace = false;
    esp_log_level_set("*", ESP_LOG_INFO);
}

static void send_error_and_reset(uint8_t code, int32_t detail)
{
    uint8_t pl[5];
    pl[0] = code;
    memcpy(pl + 1, &detail, 4);
    send_frame(T_ERROR, 0, pl, sizeof(pl), NULL, 0);
    session_reset();
}

/* Leave the post-DONE grace period: back to 115200 and print the summary on the console. */
static void finish_session(void)
{
    uint32_t st[6];
    memcpy(st, g_done_pl, sizeof(st));
    session_reset();                                   // also clears g_done_grace
    printf("stream done: %u frames, %u bytes, avg %u / max %u cycles per frame\n",
           (unsigned)st[0], (unsigned)st[1], (unsigned)st[4], (unsigned)st[5]);
}

// ---- RX task (core 0): UART bytes -> frames -> jobs -------------------------
typedef enum { P_SYNC0, P_SYNC1, P_HDR, P_PAYLOAD, P_CRC } pstate_t;
static struct {
    pstate_t state;
    uint8_t  hdr[5];
    uint8_t  hdr_got;
    uint8_t  type;
    uint16_t seq, len, got;
    uint8_t  payload[MAX_RX_PAYLOAD];
    uint16_t crc, crc_rx;
    uint8_t  crc_got;
} g_ps;

static job_t g_rx_job;

static void handle_hello(const uint8_t *pl, uint16_t len)
{
    if (g_active || g_hello_pending) return;           // ignore HELLO retries
    uint32_t baud = 0, bitrate = 0;
    uint8_t complexity = 0, version = 0;
    if (len >= 10) {
        baud = get_u32(pl); bitrate = get_u32(pl + 4); complexity = pl[8]; version = pl[9];
    }
    if (len < 10 || version != PROTO_VERSION || baud < MIN_BAUD || baud > MAX_BAUD ||
        bitrate < 6000 || bitrate > 510000 || complexity > 10) {
        uint8_t e[5] = { ERR_BAD_HELLO, 0, 0, 0, 0 };
        send_frame(T_ERROR, 0, e, sizeof(e), NULL, 0);
        return;
    }
    g_hello_pending = true;
    g_expect_seq = 0;
    g_rx_job.kind = JOB_START;
    g_rx_job.complexity = complexity;
    g_rx_job.bitrate = bitrate;
    g_rx_job.baud = baud;
    if (xMessageBufferSend(g_in, &g_rx_job, JOB_HDR_BYTES, 0) == 0) {
        g_hello_pending = false;
        send_error_and_reset(ERR_OVERFLOW, 0);
    }
}

static void handle_frame(uint8_t type, uint16_t seq, const uint8_t *pl, uint16_t len)
{
    if (type == T_HELLO) { handle_hello(pl, len); return; }
    if (g_done_grace) {
        if (type == T_END) {                           // PC has not seen DONE: send it again
            send_frame(T_DONE, 0, g_done_pl, sizeof(g_done_pl), NULL, 0);
        } else if (type == T_BYE) {                    // PC has DONE: go idle now
            finish_session();
        }
        return;
    }
    if (!g_active || g_ending) return;                 // PCM/END only valid mid-session

    g_last_rx_us = esp_timer_get_time();
    if (type == T_PCM) {
        if (len != FRAME_BYTES) { send_error_and_reset(ERR_BAD_PCM, len); return; }
        if (seq != g_expect_seq) { send_error_and_reset(ERR_SEQ, (int32_t)seq); return; }
        g_expect_seq++;
        g_rx_job.kind = JOB_PCM;
        g_rx_job.seq = seq;
        memcpy(g_rx_job.pcm, pl, FRAME_BYTES);
        if (xMessageBufferSend(g_in, &g_rx_job, sizeof(g_rx_job), 0) == 0) {
            send_error_and_reset(ERR_OVERFLOW, (int32_t)seq);   // PC exceeded the window
        }
    } else if (type == T_END) {
        g_ending = true;
        g_rx_job.kind = JOB_END;
        xMessageBufferSend(g_in, &g_rx_job, JOB_HDR_BYTES, portMAX_DELAY);
    }
}

static void parser_feed_byte(uint8_t b)
{
    switch (g_ps.state) {
    case P_SYNC0:
        if (b == SYNC0) g_ps.state = P_SYNC1;
        break;
    case P_SYNC1:
        if (b == SYNC1) { g_ps.state = P_HDR; g_ps.hdr_got = 0; g_ps.crc = 0xFFFF; }
        else if (b != SYNC0) g_ps.state = P_SYNC0;
        break;
    case P_HDR:
        g_ps.hdr[g_ps.hdr_got++] = b;
        g_ps.crc = crc16_update(g_ps.crc, &b, 1);
        if (g_ps.hdr_got == sizeof(g_ps.hdr)) {
            g_ps.type = g_ps.hdr[0];
            g_ps.seq  = (uint16_t)(g_ps.hdr[1] | (g_ps.hdr[2] << 8));
            g_ps.len  = (uint16_t)(g_ps.hdr[3] | (g_ps.hdr[4] << 8));
            g_ps.got = 0;
            g_ps.crc_got = 0;
            g_ps.crc_rx = 0;
            if (g_ps.len > MAX_RX_PAYLOAD) g_ps.state = P_SYNC0;   // desync: resync on next magic
            else g_ps.state = g_ps.len ? P_PAYLOAD : P_CRC;
        }
        break;
    case P_PAYLOAD:
        g_ps.payload[g_ps.got++] = b;
        g_ps.crc = crc16_update(g_ps.crc, &b, 1);
        if (g_ps.got == g_ps.len) g_ps.state = P_CRC;
        break;
    case P_CRC:
        g_ps.crc_rx |= (uint16_t)(b << (8 * g_ps.crc_got));
        if (++g_ps.crc_got == CRC_BYTES) {
            g_ps.state = P_SYNC0;
            if (g_ps.crc_rx == g_ps.crc) handle_frame(g_ps.type, g_ps.seq, g_ps.payload, g_ps.len);
            else if (g_active && !g_ending) send_error_and_reset(ERR_BAD_FRAME, (int32_t)g_expect_seq);
        }
        break;
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t rxbuf[256];
    int64_t last_byte_us = esp_timer_get_time();

    for (;;) {
        int n = uart_read_bytes(UART_PORT, rxbuf, sizeof(rxbuf), pdMS_TO_TICKS(50));
        int64_t now = esp_timer_get_time();
        if (n > 0) {
            last_byte_us = now;
            for (int i = 0; i < n; i++) parser_feed_byte(rxbuf[i]);
        } else {
            if (g_ps.state != P_SYNC0 && now - last_byte_us > PARSER_STALL_US) {
                g_ps.state = P_SYNC0;
                // A frame cut short mid-session means bytes were lost. Report it instead of
                // waiting for the session timeout. Detail: bit 30 set = truncated frame,
                // low 16 bits = next expected PCM sequence number.
                if (g_active && !g_ending) send_error_and_reset(ERR_BAD_FRAME, (int32_t)(0x40000000u | g_expect_seq));
            }
            if (g_active && !g_ending && now - g_last_rx_us > SESSION_TIMEOUT_US) {
                send_error_and_reset(ERR_TIMEOUT, 0);
            }
        }
        if (g_done_grace && now > g_done_deadline_us) finish_session();   // PC never said BYE
    }
}

// ---- TX task (core 0): results from core 1 -> UART --------------------------
static out_t g_tx_msg;

static void tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        size_t n = xMessageBufferReceive(g_out, &g_tx_msg, sizeof(g_tx_msg), portMAX_DELAY);
        if (n < OUT_HDR_BYTES) continue;

        switch (g_tx_msg.kind) {
        case OUT_READY: {
            uint8_t pl[12];
            pl[0] = PROTO_VERSION;
            pl[1] = STREAM_WINDOW;
            put_u16(pl + 2, FRAME_SAMPLES);
            put_u32(pl + 4, SAMPLE_RATE_HZ);
            put_u32(pl + 8, g_tx_msg.value);
            send_frame(T_READY, 0, pl, sizeof(pl), NULL, 0);
            esp_log_level_set("*", ESP_LOG_NONE);          // keep the data link clean
            uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(200));
            uart_set_baudrate(UART_PORT, g_tx_msg.value);
            g_last_rx_us = esp_timer_get_time();
            g_hello_pending = false;
            g_active = true;
            break;
        }
        case OUT_PACKET:
            if (!g_active) break;                          // stale result from an aborted session
            send_frame(T_PACKET, g_tx_msg.seq, &g_tx_msg.value, 4, g_tx_msg.data, g_tx_msg.len);
            break;
        case OUT_DONE:
            if (!g_active) break;
            // Keep the payload and stay at the high baud rate. rx_task answers a repeated END
            // with DONE again, and returns to 115200 on BYE or when the grace period ends.
            memcpy(g_done_pl, g_tx_msg.data, sizeof(g_done_pl));
            g_done_deadline_us = esp_timer_get_time() + DONE_GRACE_US;
            g_done_grace = true;
            send_frame(T_DONE, 0, g_done_pl, sizeof(g_done_pl), NULL, 0);
            break;
        case OUT_ERROR:
            send_error_and_reset(g_tx_msg.code, (int32_t)g_tx_msg.value);
            break;
        }
    }
}

// ---- Encoder task (core 1) --------------------------------------------------
static job_t g_enc_job;
static out_t g_enc_out;

static void out_send(size_t data_len)
{
    g_enc_out.len = (uint16_t)data_len;
    // The PC's window keeps at most STREAM_WINDOW results queued, so this does not block.
    xMessageBufferSend(g_out, &g_enc_out, OUT_HDR_BYTES + data_len, portMAX_DELAY);
}

static void out_error(uint8_t code, int32_t detail)
{
    g_enc_out.kind = OUT_ERROR;
    g_enc_out.code = code;
    g_enc_out.value = (uint32_t)detail;
    out_send(0);
}

static void enc_task(void *arg)
{
    (void)arg;
    OpusEncoder *enc = NULL;
    bool ready = false;
    uint32_t hash = 0, frames = 0, total_bytes = 0, min_c = 0, max_c = 0;
    uint64_t sum_c = 0;
    uint32_t since_yield = 0;

    for (;;) {
        size_t n = xMessageBufferReceive(g_in, &g_enc_job, sizeof(g_enc_job), portMAX_DELAY);
        if (n < JOB_HDR_BYTES) continue;

        if (g_enc_job.kind == JOB_START) {
            int err = OPUS_OK;
            if (enc == NULL) enc = opus_encoder_create(SAMPLE_RATE_HZ, 1, OPUS_APPLICATION_VOIP, &err);
            if (enc == NULL || err != OPUS_OK) { enc = NULL; ready = false; out_error(ERR_ENC_CREATE, err); continue; }
            opus_encoder_ctl(enc, OPUS_SET_BITRATE((opus_int32)g_enc_job.bitrate));
            opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(g_enc_job.complexity));
            opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
            opus_encoder_ctl(enc, OPUS_SET_VBR(1));
            opus_encoder_ctl(enc, OPUS_RESET_STATE);
            hash = 2166136261u; frames = 0; total_bytes = 0; min_c = UINT32_MAX; max_c = 0; sum_c = 0;
            since_yield = 0;
            ready = true;
            g_enc_out.kind = OUT_READY;
            g_enc_out.value = g_enc_job.baud;
            out_send(0);

        } else if (g_enc_job.kind == JOB_PCM) {
            if (!ready) continue;
            uint32_t t0 = esp_cpu_get_cycle_count();
            int len = opus_encode(enc, g_enc_job.pcm, FRAME_SAMPLES, g_enc_out.data, MAX_PKT);
            uint32_t t1 = esp_cpu_get_cycle_count();
            if (len < 0) { ready = false; out_error(ERR_ENC_FAIL, len); continue; }

            uint32_t cycles = t1 - t0;
            hash = fnv1a(hash, g_enc_out.data, len);
            frames++;
            total_bytes += (uint32_t)len;
            sum_c += cycles;
            if (cycles < min_c) min_c = cycles;
            if (cycles > max_c) max_c = cycles;

            g_enc_out.kind = OUT_PACKET;
            g_enc_out.seq = g_enc_job.seq;
            g_enc_out.value = cycles;
            out_send((size_t)len);

            if (++since_yield >= YIELD_EVERY_FRAMES) { since_yield = 0; vTaskDelay(1); }

        } else if (g_enc_job.kind == JOB_END) {
            if (!ready) continue;
            uint32_t st[6] = { frames, total_bytes, hash, frames ? min_c : 0,
                               frames ? (uint32_t)(sum_c / frames) : 0, max_c };
            memcpy(g_enc_out.data, st, sizeof(st));
            g_enc_out.kind = OUT_DONE;
            out_send(sizeof(st));
            ready = false;
        }
    }
}

// ---- Setup ------------------------------------------------------------------
void stream_codec_start(void)
{
    printf("\nOpus USB stream mode: waiting for HELLO at %d baud (window %d frames).\n",
           BOOT_BAUD, STREAM_WINDOW);

    crc_init();
    g_tx_lock = xSemaphoreCreateMutex();
    g_in  = xMessageBufferCreate((STREAM_WINDOW + 2) * (sizeof(job_t) + 4));
    g_out = xMessageBufferCreate((STREAM_WINDOW + 2) * (sizeof(out_t) + 4));
    if (g_tx_lock == NULL || g_in == NULL || g_out == NULL) {
        printf("stream: out of memory creating buffers\n");
        return;
    }

    const uart_config_t cfg = {
        .baud_rate = BOOT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(UART_PORT, UART_RX_RING_BYTES, UART_TX_RING_BYTES, 0, NULL, 0) != ESP_OK ||
        uart_param_config(UART_PORT, &cfg) != ESP_OK ||
        uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        printf("stream: UART setup failed\n");
        return;
    }

    // Encoder on core 1 (same as the benchmark); UART handling on core 0.
    xTaskCreatePinnedToCore(enc_task, "enc_c1", 16 * 1024, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(tx_task,  "tx_c0",  4096,      NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(rx_task,  "rx_c0",  4096,      NULL, 6, NULL, 0);
}