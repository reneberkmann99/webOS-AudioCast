#!/usr/bin/env python3
"""TV audio adapter for the HyperHDR reactive-backlight pipeline.

Receives AudioCast RTP/L16 (48 kHz stereo S16, network byte order) from the webOS
TV, downmixes to mono, resamples 48 kHz -> 22.05 kHz, and forwards as little-endian
float32 UDP to the techno engine's network capture port on localhost.

No audio is ever stored. There is no authentication at this layer: the host firewall
restricts the RTP port to the TV IP only, and this process additionally ignores any
non-TV source address as a second line of defence.

RTP timestamps alone cannot measure TV-to-LED latency.
"""
from __future__ import annotations
import argparse
from array import array
import json
import math
import signal
import socket
import struct
import sys
import threading
import time

import numpy as np

TV_IP = "10.10.40.104"
RATE_IN = 48000
RATE_OUT = 22050
CHANNELS = 2
PAYLOAD_TYPE = 96
ENGINE_ADDR = ("127.0.0.1", 17000)


def delta(value, base, bits):
    return ((value - base + (1 << (bits - 1))) % (1 << bits)) - (1 << (bits - 1))


def parse_rtp(data, channels, payload_type=PAYLOAD_TYPE):
    if len(data) < 12 or data[0] >> 6 != 2:
        raise ValueError("invalid RTP v2 header")
    if data[1] & 127 != payload_type:
        raise ValueError("unexpected RTP payload type")
    offset = 12 + 4 * (data[0] & 15)
    if offset > len(data):
        raise ValueError("truncated CSRC list")
    if data[0] & 16:
        if offset + 4 > len(data):
            raise ValueError("truncated RTP extension")
        words = struct.unpack_from("!H", data, offset + 2)[0]
        offset += 4 + 4 * words
        if offset > len(data):
            raise ValueError("truncated RTP extension data")
    end = len(data)
    if data[0] & 32:
        pad = data[-1]
        if not pad or pad > end - offset:
            raise ValueError("invalid RTP padding")
        end -= pad
    payload = data[offset:end]
    if channels not in (1, 2) or not payload or len(payload) % (2 * channels):
        raise ValueError("unaligned L16 payload")
    seq, stamp, ssrc = struct.unpack_from("!HII", data, 2)
    return seq, stamp, ssrc, bool(data[1] & 128), payload


def l16_to_le(data):
    samples = array("h")
    samples.frombytes(data)
    samples.byteswap()
    return samples.tobytes()


class SequenceTracker:
    def __init__(self):
        self.highest = None
        self.seen = set()
        self.received = self.duplicates = self.reordered = self.gaps = self.recovered = 0

    def put(self, seq):
        extended = seq if self.highest is None else self.highest + delta(seq, self.highest & 65535, 16)
        if self.highest is not None and extended < self.highest - 4096:
            self.reordered += 1
            return False
        if extended in self.seen:
            self.duplicates += 1
            return False
        if self.highest is not None:
            if extended < self.highest:
                self.reordered += 1
                self.recovered += 1
            elif extended > self.highest + 1:
                self.gaps += extended - self.highest - 1
        self.highest = extended if self.highest is None else max(self.highest, extended)
        self.seen.add(extended)
        self.seen = {v for v in self.seen if v >= self.highest - 4096}
        self.received += 1
        return True


class JitterBuffer:
    def __init__(self, rate, channels, jitter_ms=40):
        self.rate, self.channels, self.jitter_ms = rate, channels, jitter_ms
        self.align = channels * 2
        self.blocks = {}
        self.cursor = None
        self.deadline = None
        self.ssrc = None
        self.tracker = SequenceTracker()
        self.resets = 0
        self.max_packets = 256
        self.max_frames = rate // 2

    def reset(self, packet, now):
        self.blocks.clear()
        self.cursor = packet[1]  # timestamp
        self.deadline = now + self.jitter_ms / 1000
        self.ssrc = packet[2]
        self.tracker = SequenceTracker()
        self.resets += 1

    def put(self, seq, stamp, ssrc, marker, payload, now):
        if self.ssrc != ssrc or self.cursor is None:
            self.reset((seq, stamp, ssrc, marker, payload), now)
        st = self.cursor + delta(stamp, self.cursor & 0xFFFFFFFF, 32)
        forward = self.tracker.highest is None or delta(seq, self.tracker.highest & 65535, 16) > 0
        if forward and ((marker and self.tracker.highest is not None) or abs(st - self.cursor) > self.rate):
            self.reset((seq, stamp, ssrc, marker, payload), now)
            st = stamp
        if not self.tracker.put(seq):
            return
        n = len(payload) // self.align
        if st + n <= self.cursor:
            return
        if st - self.cursor > self.max_frames or len(self.blocks) >= self.max_packets:
            return
        self.blocks[st] = payload

    def pop(self, now):
        if self.deadline is None or now < self.deadline:
            return None
        if now - self.deadline > 0.25:
            self.blocks.clear()
            self.deadline = None
            self.cursor = None
            self.ssrc = None
            return None
        frames = self.rate // 100
        end = self.cursor + frames
        out = bytearray(frames * self.align)
        covered = bytearray(frames)
        for stamp in sorted(list(self.blocks)):
            data = self.blocks[stamp]
            last = stamp + len(data) // self.align
            if last <= self.cursor:
                del self.blocks[stamp]
                continue
            if stamp >= end:
                break
            lo, hi = max(stamp, self.cursor), min(last, end)
            out[(lo - self.cursor) * self.align:(hi - self.cursor) * self.align] = \
                data[(lo - stamp) * self.align:(hi - stamp) * self.align]
            covered[lo - self.cursor:hi - self.cursor] = b"\x01" * (hi - lo)
            if last <= end:
                del self.blocks[stamp]
            else:
                self.blocks[end] = data[(end - stamp) * self.align:]
                del self.blocks[stamp]
        self.cursor = end
        self.deadline += frames / self.rate
        return l16_to_le(bytes(out))


def stereo_le_to_mono_f32(pcm_le):
    arr = np.frombuffer(pcm_le, dtype="<i2").astype(np.float32)
    arr = arr.reshape(-1, 2)
    return arr.mean(axis=1) / 32768.0


def resample48_22050(mono48):
    n_in = mono48.shape[0]
    if n_in < 2:
        return None
    n_out = int(round(n_in * RATE_OUT / RATE_IN))
    if n_out < 1:
        return None
    x_in = np.arange(n_in, dtype=np.float64) / RATE_IN
    x_out = np.arange(n_out, dtype=np.float64) / RATE_OUT
    return np.interp(x_out, x_in, mono48).astype(np.float32)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5004)
    ap.add_argument("--tv", default=TV_IP, help="only accept RTP from this source IP")
    ap.add_argument("--engine", default="127.0.0.1:17000")
    args = ap.parse_args()

    host, _, port_s = args.engine.partition(":")
    engine_addr = (host, int(port_s))
    out_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
    sock.bind((args.bind, args.port))
    sock.setblocking(False)

    jitter = JitterBuffer(RATE_IN, CHANNELS, jitter_ms=40)
    accum = np.empty(0, dtype=np.float32)
    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())

    last_report = time.monotonic()
    received_bytes = 0
    forwarded_samples = 0
    last_sender = None
    last_rms = 0.0
    last_packet = 0.0
    print(f"tv-audio-adapter listening {args.bind}:{args.port} -> engine {args.engine} (tv={args.tv})",
          file=sys.stderr, flush=True)

    while not stop.is_set():
        now = time.monotonic()
        for _ in range(64):
            try:
                data, peer = sock.recvfrom(65536)
            except BlockingIOError:
                break
            if peer[0] != args.tv:
                continue
            if last_sender != peer[0]:
                last_sender = peer[0]
                print(f"receiving TV RTP from {peer[0]}", file=sys.stderr, flush=True)
            try:
                seq, stamp, ssrc, marker, payload = parse_rtp(data, CHANNELS)
            except ValueError:
                continue
            received_bytes += len(payload)
            last_packet = now
            jitter.put(seq, stamp, ssrc, marker, payload, now)

        # Stop producing silence-fill once the TV has been silent for a grace
        # period; the engine's own stale-audio logic then owns the quiet state.
        if now - last_packet <= 0.5:
            for _ in range(25):
                pcm = jitter.pop(now)
                if pcm is None:
                    break
                mono = stereo_le_to_mono_f32(pcm)
                accum = np.concatenate((accum, mono))
                if accum.shape[0] >= RATE_IN // 10:  # ~100ms at 48k
                    out22 = resample48_22050(accum)
                    if out22 is not None:
                        out_sock.sendto(out22.tobytes(), engine_addr)
                        forwarded_samples += int(out22.shape[0])
                        last_rms = float(np.sqrt(np.mean(out22.astype(np.float64) ** 2)))
                    accum = np.empty(0, dtype=np.float32)
        else:
            accum = np.empty(0, dtype=np.float32)
        if now - last_report >= 1.0:
            stats = {
                "rms": round(last_rms, 6),
                "received_pcm_bytes": received_bytes,
                "forwarded_samples": forwarded_samples,
                "packets": jitter.tracker.received,
                "gaps_unrepaired": jitter.tracker.gaps - jitter.tracker.recovered,
                "duplicates": jitter.tracker.duplicates,
                "sender": last_sender,
            }
            print(json.dumps(stats), flush=True)
            last_report = now

        stop.wait(0.002)

    sock.close()
    out_sock.close()
    print("tv-audio-adapter stopped", file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
