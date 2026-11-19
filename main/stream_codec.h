/*
 * USB-serial Opus streaming mode (kept separate from the benchmark build).
 *
 * Data flow
 *   PC (tools/stream_mp3.py) --PCM frames--> core 0 RX task --job buffer--> core 1 encoder task
 *   PC <--Opus packets------- core 0 TX task <--result buffer------------- core 1 encoder task
 *
 * Core 0 owns the UART. Core 1 only runs opus_encode().
 *
 * Wire format (all integers little-endian):
 *   0xA5 0x5A | type u8 | seq u16 | len u16 | payload[len] | crc16 u16
 *   crc16 = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type..payload.
 *
 * PC -> ESP32
 *   0x01 HELLO   baud u32, bitrate u32, complexity u8, version u8   (sent at 115200)
 *   0x02 PCM     320 x int16 (one 20 ms frame, 16 kHz mono); seq = frame number mod 65536
 *   0x03 END     total_frames u32
 * ESP32 -> PC
 *   0x81 READY   version u8, window u8, frame_samples u16, sample_rate u32, baud u32
 *                (sent at 115200; both sides then switch to `baud`)
 *   0x82 PACKET  encode_cycles u32, opus bytes...  (seq = PCM seq it answers)
 *   0x83 DONE    frames, total_bytes, fnv1a_hash, min_cycles, avg_cycles, max_cycles (6 x u32)
 *   0x8F ERROR   code u8, detail i32
 *
 * Flow control: the PC may have at most `window` PCM frames sent but not yet answered by a
 * PACKET. Each PACKET returns one credit. There are no other credit messages.
 *
 * Select this mode at build time:   idf.py -DAPP_MODE=stream build flash
 * Back to the benchmark:            idf.py -DAPP_MODE=benchmark build flash
 */
#pragma once

void stream_codec_start(void);