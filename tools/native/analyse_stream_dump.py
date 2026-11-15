#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Reads a KYUJO_STREAM_DUMP file (see host_audio.c) and reports what the game's streamed voice (the announcer) looked like in its ring buffer.

    analyse_stream_dump.py dump.bin [--wav out.wav] [--channel N]

Record layout: u8 channel, u8 state, u16 0, u32 read position, u32 write position (host byte order), 0x1140 bytes ring (2208 big-endian s16: left, right, left, right ...).
Prints, per channel: records, state changes, how the read/write distance moves, and for the samples the game wrote between two reads: jumps (isolated
spikes) and how many of the samples the RSP read were never written freshly (the reader overtook the writer). --wav writes what the reader consumed, in order,
as a mono 16-bit WAV at 8 kHz scale (the sample rate of the stream is whatever the game uses; only the shape matters). Standard library and numpy.
"""
import argparse, struct, sys
import numpy as np

RING = 0x1140
REC = 12 + RING

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--wav")
    ap.add_argument("--channel", type=int, default=-1)
    a = ap.parse_args()
    data = open(a.dump, "rb").read()
    n = len(data) // REC
    print("records:", n)
    recs = []
    for i in range(n):
        o = i * REC
        ch, st = data[o], data[o + 1]
        rd, wr = struct.unpack_from("<II", data, o + 4)
        ring = np.frombuffer(data, dtype=">i2", count=RING // 2, offset=o + 12).astype(np.int32)
        recs.append((ch, st, rd, wr, ring))
    for ch in (0, 1):
        if a.channel >= 0 and ch != a.channel: continue
        rs = [r for r in recs if r[0] == ch]
        if not rs: continue
        print("channel", ch, "records", len(rs), "states", sorted(set(r[1] for r in rs)))
        # reader consumes 0xB8 s16 at rd each task; the stream is stereo-duplicated (L==R) so take every second value
        out = []
        dist = []
        stale = 0
        for (_, st, rd, wr, ring) in rs:
            seg = ring[rd:rd + 0xB8] if rd + 0xB8 <= len(ring) else ring[rd:]
            out.append(seg[::2])
            dist.append((wr - rd) % 2208)
            if st in (3, 4):
                pass
        pcm = np.concatenate(out) if out else np.zeros(0, dtype=np.int32)
        lr_diff = sum(int(np.abs(r[4][0::2] - r[4][1::2]).sum() > 0) for r in rs)
        print("  samples read:", len(pcm), " rms:", int(np.sqrt((pcm.astype(float) ** 2).mean())) if len(pcm) else 0, " peak:", int(np.abs(pcm).max()) if len(pcm) else 0)
        print("  records with left != right somewhere in the ring:", lr_diff)
        d = np.array(dist)
        print("  write-minus-read distance (s16 units): min %d  max %d  mean %.0f" % (d.min(), d.max(), d.mean()))
        sp = np.abs(pcm[1:-1] - (pcm[:-2] + pcm[2:]) / 2) if len(pcm) > 2 else np.zeros(0)
        print("  isolated spikes > 4000:", int((sp > 4000).sum()), " > 8000:", int((sp > 8000).sum()), " of ", len(sp))
        if a.wav:
            import wave
            w = wave.open(a.wav + ".ch%d.wav" % ch, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
            w.writeframes(np.clip(pcm, -32768, 32767).astype("<i2").tobytes()); w.close()
            print("  wrote", a.wav + ".ch%d.wav" % ch)

if __name__ == "__main__":
    main()
