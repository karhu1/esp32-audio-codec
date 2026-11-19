#!/usr/bin/env python3
"""Stream an MP3 to the ESP32 Opus encoder over USB serial and save the Opus packets it returns.

Setup (once):   python -m pip install pyserial miniaudio
Firmware:       idf.py -DAPP_MODE=stream build flash   (close any serial monitor afterwards)
Run:            python tools/stream_mp3.py path/to/song.mp3 [--port COM6]

The MP3 is decoded and resampled on the PC to 16 kHz mono 16-bit, cut into 20 ms frames, and
sent with a credit window so both ESP32 cores stay busy. Output (in out/):
  <name>.opus        playable Ogg Opus file (open in VLC, or: ffplay out/<name>.opus)
  <name>.opkt        header + length-prefixed raw Opus packets
  <name>.cycles.csv  per-frame packet size and core-1 encode cycles
.opkt layout: b"OPKT", u16 version(1), u16 sample_rate, u16 frame_samples, u16 reserved,
then repeated: u16 packet_length, packet bytes.  (all little-endian)
Protocol details are documented in main/stream_codec.h.
"""

import argparse
import os
import re
import struct
import sys
import time
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parents[1]
OUTPUT_DIR = PROJECT_DIR / "out"

SAMPLE_RATE = 16000
FRAME_SAMPLES = 320
FRAME_BYTES = FRAME_SAMPLES * 2
PROTO_VERSION = 1
STALL_TIMEOUT_S = 12
END_RESEND_AFTER_S = 1.0   # all packets in, END sent, no DONE for this long -> send END again
END_MAX_RESENDS = 4        # then give up (the board keeps its high baud rate for 6 s after DONE)
BOOT_BAUD = 115200
MAGIC = b"\xA5\x5A"

T_HELLO, T_PCM, T_END, T_BYE = 0x01, 0x02, 0x03, 0x04
T_READY, T_PACKET, T_DONE, T_ERROR = 0x81, 0x82, 0x83, 0x8F

ERROR_NAMES = {1: "bad frame (CRC)", 2: "encoder create failed", 3: "encode failed",
               4: "bad HELLO parameters", 5: "sequence error", 6: "session timeout",
               7: "buffer overflow", 8: "bad PCM frame length"}

_CRC_TABLE = []
for _i in range(256):
    _c = _i << 8
    for _ in range(8):
        _c = ((_c << 1) ^ 0x1021) & 0xFFFF if _c & 0x8000 else (_c << 1) & 0xFFFF
    _CRC_TABLE.append(_c)


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE (check value for b'123456789' is 0x29B1)."""
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC_TABLE[(crc >> 8) ^ b]
    return crc


def build_frame(ftype, seq, payload=b""):
    body = struct.pack("<BHH", ftype, seq & 0xFFFF, len(payload)) + payload
    return MAGIC + body + struct.pack("<H", crc16(body))


class FrameParser:
    """Extracts valid frames from a byte stream, skipping boot text and garbage."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        frames = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                self.buf = bytearray(self.buf[-1:]) if self.buf.endswith(b"\xA5") else bytearray()
                break
            if i:
                del self.buf[:i]
            if len(self.buf) < 7:
                break
            ftype, seq, length = struct.unpack_from("<BHH", self.buf, 2)
            if length > 1400:
                del self.buf[:2]
                continue
            total = 9 + length
            if len(self.buf) < total:
                break
            body = bytes(self.buf[2:7 + length])
            (crc_rx,) = struct.unpack_from("<H", self.buf, 7 + length)
            if crc16(body) == crc_rx:
                frames.append((ftype, seq, bytes(self.buf[7:7 + length])))
                del self.buf[:total]
            else:
                del self.buf[:2]
        return frames


def fnv1a(data, h=2166136261):
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


# ---- Ogg Opus output (RFC 3533 / RFC 7845) ----------------------------------
# Ogg Opus counts time in 48 kHz samples regardless of the encode rate, so one 20 ms frame
# is 960 and the encoder lookahead (16000/400 + 16000/250 = 104 samples at 16 kHz, from
# OPUS_GET_LOOKAHEAD in opus_encoder.c) is 312 as the OpusHead pre-skip.
PRE_SKIP_48K = 312
FRAME_48K = 960
PACKETS_PER_PAGE = 50

_OGG_TABLE = []
for _i in range(256):
    _r = _i << 24
    for _ in range(8):
        _r = ((_r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if _r & 0x80000000 else (_r << 1) & 0xFFFFFFFF
    _OGG_TABLE.append(_r)


def ogg_crc(data):
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _OGG_TABLE[((crc >> 24) & 0xFF) ^ b]
    return crc


def ogg_page(serial, seqno, header_type, granule, packets):
    segs = bytearray()
    for p in packets:
        segs += b"\xFF" * (len(p) // 255) + bytes([len(p) % 255])
    assert len(segs) <= 255
    head = struct.pack("<4sBBqIIIB", b"OggS", 0, header_type, granule, serial, seqno, 0, len(segs))
    page = head + bytes(segs) + b"".join(packets)
    return page[:22] + struct.pack("<I", ogg_crc(page)) + page[26:]


def write_ogg_opus(path, packets, real_samples):
    """Write the raw Opus packets as an Ogg Opus file (mono, input rate 16 kHz)."""
    serial = 0x4F505553
    opus_head = struct.pack("<8sBBHIhB", b"OpusHead", 1, 1, PRE_SKIP_48K, SAMPLE_RATE, 0, 0)
    vendor = b"esp32-opus-stream"
    opus_tags = b"OpusTags" + struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", 0)
    # End-trim: drop the silence padded onto the last frame.
    final_granule = min(len(packets) * FRAME_48K, PRE_SKIP_48K + real_samples * 3)
    with open(path, "wb") as f:
        f.write(ogg_page(serial, 0, 0x02, 0, [opus_head]))
        f.write(ogg_page(serial, 1, 0x00, 0, [opus_tags]))
        seqno, i, n = 2, 0, len(packets)
        while i < n:
            group, segs = [], 0
            while i < n and len(group) < PACKETS_PER_PAGE:
                need = len(packets[i]) // 255 + 1
                if segs + need > 255:
                    break
                group.append(packets[i])
                segs += need
                i += 1
            last = i >= n
            granule = final_granule if last else i * FRAME_48K
            f.write(ogg_page(serial, seqno, 0x04 if last else 0x00, granule, group))
            seqno += 1


def default_port():
    settings = PROJECT_DIR / ".vscode" / "settings.json"
    try:
        m = re.search(r'"idf\.port"\s*:\s*"([^"]+)"', settings.read_text(encoding="utf-8"))
        if m:
            return m.group(1)
    except OSError:
        pass
    return os.environ.get("ESPPORT")


def load_mp3(path):
    """Decode an MP3 to 16 kHz mono signed 16-bit PCM, padded with silence to whole frames."""
    if path.suffix.lower() != ".mp3":
        sys.exit("Only .mp3 input is accepted: " + str(path))
    try:
        import miniaudio
    except ImportError:
        sys.exit("Missing miniaudio: python -m pip install miniaudio")
    decoded = miniaudio.decode_file(str(path), output_format=miniaudio.SampleFormat.SIGNED16,
                                    nchannels=1, sample_rate=SAMPLE_RATE)
    samples = decoded.samples
    if sys.byteorder != "little":
        samples.byteswap()
    pcm = samples.tobytes()
    if not pcm:
        sys.exit("Decoded audio is empty")
    pad = (-len(pcm)) % FRAME_BYTES
    return pcm + b"\x00" * pad, len(pcm) // 2   # padded PCM, real sample count


def open_port(port, baud):
    try:
        import serial
    except ImportError:
        sys.exit("Missing pyserial: python -m pip install pyserial")
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    ser.write_timeout = 5
    ser.dtr = False   # try not to reset the board when the port opens
    ser.rts = False
    ser.open()
    return ser


def handshake(ser, baud, bitrate, complexity):
    """Send HELLO at 115200 until READY arrives, then switch to the fast baud rate."""
    hello = build_frame(T_HELLO, 0, struct.pack("<IIBB", baud, bitrate, complexity, PROTO_VERSION))
    parser = FrameParser()
    ser.reset_input_buffer()
    deadline = time.time() + 15
    while time.time() < deadline:
        ser.write(hello)
        t_end = time.time() + 0.3
        while time.time() < t_end:
            for ftype, _seq, payload in parser.feed(ser.read(ser.in_waiting or 1)):
                if ftype == T_READY:
                    ver, window, frame_samples, rate, new_baud = struct.unpack("<BBHII", payload)
                    if ver != PROTO_VERSION or frame_samples != FRAME_SAMPLES or rate != SAMPLE_RATE:
                        sys.exit("Firmware/script mismatch in READY: " + repr(payload))
                    ser.baudrate = new_baud
                    time.sleep(0.05)
                    ser.reset_input_buffer()
                    return window
                if ftype == T_ERROR:
                    sys.exit("Board rejected HELLO: " + ERROR_NAMES.get(payload[0], str(payload[0])))
    sys.exit("No READY from the board. Is it flashed with APP_MODE=stream and is the "
             "serial monitor closed?")


def stream(ser, pcm, window):
    """Send all frames with a credit window; return (packets, cycles, done_stats, seconds, end_resends)."""
    nframes = len(pcm) // FRAME_BYTES
    packets, cycles = [], []
    parser = FrameParser()
    sent = 0
    end_sent = False
    end_resends = 0
    last_end_tx = 0.0
    done = None
    last_progress = time.time()
    shown = 0
    t_start = time.time()

    while done is None:
        while sent < nframes and sent - len(packets) < window:
            ser.write(build_frame(T_PCM, sent, pcm[sent * FRAME_BYTES:(sent + 1) * FRAME_BYTES]))
            sent += 1
        if sent == nframes and not end_sent:
            ser.write(build_frame(T_END, nframes, struct.pack("<I", nframes)))
            end_sent = True
            last_end_tx = time.time()

        for ftype, seq, payload in parser.feed(ser.read(ser.in_waiting or 1)):
            last_progress = time.time()
            if ftype == T_PACKET:
                if seq != (len(packets) & 0xFFFF):
                    sys.exit("Out-of-order packet: got seq %d, expected %d" % (seq, len(packets) & 0xFFFF))
                cycles.append(struct.unpack_from("<I", payload)[0])
                packets.append(payload[4:])
            elif ftype == T_DONE:
                done = struct.unpack("<6I", payload)
            elif ftype == T_ERROR:
                code = payload[0]
                detail = struct.unpack_from("<i", payload, 1)[0]
                extra = ""
                if code == 1:
                    extra = (", truncated frame, board expected PCM seq %d" % (detail & 0xFFFF)
                             if detail & 0x40000000 else ", board expected PCM seq %d" % detail)
                sys.exit("Board error: %s (code %d, detail %d%s). PC had sent %d PCM frames, "
                         "received %d packets, END sent: %s" %
                         (ERROR_NAMES.get(code, "unknown"), code, detail, extra,
                          sent, len(packets), end_sent))

        # Every packet is in but DONE is not: it (or END) may have been lost. Ask again, a few times.
        if (end_sent and done is None and len(packets) == nframes and
                time.time() - max(last_progress, last_end_tx) > END_RESEND_AFTER_S):
            if end_resends >= END_MAX_RESENDS:
                sys.exit("DONE not received after %d END resends (%d/%d packets received)" %
                         (end_resends, len(packets), nframes))
            ser.write(build_frame(T_END, nframes, struct.pack("<I", nframes)))
            end_resends += 1
            last_end_tx = time.time()

        # Longer than the board's own 5 s session timeout, so the board's ERROR frame arrives first.
        if time.time() - last_progress > STALL_TIMEOUT_S:
            sys.exit("Timed out waiting for the board (%d/%d packets received, %d PCM sent, END sent: %s)" %
                     (len(packets), nframes, sent, end_sent))
        if len(packets) // 50 != shown // 50:
            shown = len(packets)
            print("\r  %d / %d frames" % (shown, nframes), end="", flush=True)

    wall_s = time.time() - t_start
    print("\r  %d / %d frames" % (len(packets), nframes))
    # Tell the board it can leave its resend window and return to 115200 now. Sent twice in case
    # the first is lost; if both are, the board gives up by itself after 6 s.
    for i in range(2):
        ser.write(build_frame(T_BYE, 0))
        ser.flush()
        if i == 0:
            time.sleep(0.03)
    return packets, cycles, done, wall_s, end_resends


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mp3", type=Path, help="input .mp3 file")
    ap.add_argument("--port", default=default_port(), help="serial port (default: idf.port from .vscode/settings.json, then ESPPORT)")
    ap.add_argument("--baud", type=int, default=460800, help="link speed after the handshake (CP2102 max is 1000000)")
    ap.add_argument("--bitrate", type=int, default=20000, help="Opus target bitrate in bps")
    ap.add_argument("--complexity", type=int, default=0, help="Opus complexity 0-10")
    ap.add_argument("--out", type=Path, default=OUTPUT_DIR, help="output directory")
    args = ap.parse_args()
    if not args.port:
        sys.exit("No serial port: pass --port COMx (or set idf.port / ESPPORT)")

    pcm, real_samples = load_mp3(args.mp3)
    nframes = len(pcm) // FRAME_BYTES
    audio_s = nframes * FRAME_SAMPLES / SAMPLE_RATE
    print("%s: %d frames, %.1f s of audio at %d Hz mono" % (args.mp3.name, nframes, audio_s, SAMPLE_RATE))

    ser = open_port(args.port, BOOT_BAUD)
    try:
        window = handshake(ser, args.baud, args.bitrate, args.complexity)
        print("Linked at %d baud, window %d frames. Streaming..." % (ser.baudrate, window))
        packets, cycles, done, wall_s, end_resends = stream(ser, pcm, window)
    finally:
        ser.close()

    frames, total_bytes, board_hash, min_c, avg_c, max_c = done
    received_bytes = sum(len(p) for p in packets)
    pc_hash = fnv1a(b"".join(packets))
    ok = (frames == nframes == len(packets) and total_bytes == received_bytes and board_hash == pc_hash)

    args.out.mkdir(parents=True, exist_ok=True)
    stem = args.mp3.stem
    opkt = args.out / (stem + ".opkt")
    with open(opkt, "wb") as f:
        f.write(struct.pack("<4sHHHH", b"OPKT", 1, SAMPLE_RATE, FRAME_SAMPLES, 0))
        for p in packets:
            f.write(struct.pack("<H", len(p)) + p)
    opus_path = args.out / (stem + ".opus")
    write_ogg_opus(opus_path, packets, real_samples)
    csv_path = args.out / (stem + ".cycles.csv")
    with open(csv_path, "w") as f:
        f.write("frame,bytes,encode_cycles\n")
        for i, (p, c) in enumerate(zip(packets, cycles)):
            f.write("%d,%d,%d\n" % (i, len(p), c))

    print("\nIntegrity           : %s (frames %d/%d, bytes %d/%d, hash board 0x%08X vs PC 0x%08X)" %
          ("OK" if ok else "MISMATCH", frames, nframes, total_bytes, received_bytes, board_hash, pc_hash))
    print("Wall time           : %.2f s for %.1f s of audio = %.2fx real time" % (wall_s, audio_s, audio_s / wall_s))
    if end_resends:
        print("END resends         : %d (DONE was not received on the first try)" % end_resends)
    print("Opus output         : %d bytes, %.0f bps average" % (received_bytes, received_bytes * 8 / audio_s))
    print("Encode cycles/frame : min %d, avg %d, max %d (core 1; avg = %.0f us at 240 MHz)" %
          (min_c, avg_c, max_c, avg_c / 240.0))
    print("Saved               : %s  (playable)\n                      %s\n                      %s" %
          (opus_path, opkt, csv_path))
    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()