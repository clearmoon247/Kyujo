#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds and runs tests/native/audio/test_cushion.c, which checks the sound-device cushion in src/platform/host_audio.c (Windows waveOut
path) against a simulated device, and checks the numbers it prints:

  cushion 0 (off):  a 50 ms hold-up of the game is heard as a gap (device idle for about that long).
  cushion 80:       the same hold-up is NOT heard (gap 0), and exactly one cushion of silence was queued (at start-up).
  cushion 80:       a 200 ms hold-up IS heard (longer than the cushion), the device is then given a new cushion (2 silence buffers in total)
                    and the next hold-up-free stretch has no further gap.
Usage: run_cushion_test.py   (needs gcc, run from anywhere)
"""
import os, re, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

def run(exe, cushion, scenario, volume=100, start=None):
    out = subprocess.run([exe, str(cushion), str(scenario), str(cushion if start is None else start)], capture_output=True, text=True, check=True, env=dict(os.environ, TEST_VOLUME=str(volume)))
    m = re.search(r"gap_ms=([\d.]+) silence_buffers=(\d+) buffers=(\d+) peak=(\d+) queue_ms=([\d.]+)", out.stdout)
    run.peak = int(m.group(4))
    run.queue = float(m.group(5))
    return float(m.group(1)), int(m.group(2)), int(m.group(3)), out.stderr

def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_cushion")
        subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra", "-D_WIN32", "-include", "time.h", "-I" + os.path.join(HERE, "fake"), "-I" + os.path.join(REPO, "src", "platform"),
                        os.path.join(HERE, "test_cushion.c"), os.path.join(REPO, "src", "platform", "host_audio.c"), "-o", exe, "-lm"], check=True)
        ok = True
        def check(name, cond, info):
            nonlocal ok
            print(("PASS  " if cond else "FAIL  ") + name + "  " + info)
            ok = ok and cond
        g, s, b, _ = run(exe, 0, 1)
        check("cushion off, 50 ms hold-up is heard", 30 <= g <= 60 and s == 0, "gap %.1f ms, silence buffers %d" % (g, s))
        g, s, b, _ = run(exe, 80, 1)
        check("cushion 80 ms, 50 ms hold-up is not heard", g == 0 and s == 1, "gap %.1f ms, silence buffers %d" % (g, s))
        g, s, b, e = run(exe, 80, 2)
        check("cushion 80 ms, 200 ms hold-up is heard once and the cushion is rebuilt", 100 <= g <= 140 and s == 2, "gap %.1f ms, silence buffers %d" % (g, s))
        check("the ran-dry notice is printed once", e.count("had run dry") == 1, "count %d" % e.count("had run dry"))
        # start-up cushion (the Windows "Pika" voice stutter): a 150 ms stall right after the start
        g, s, b, e = run(exe, 80, 3)
        check("start cushion = normal cushion (80 ms): a 150 ms stall just after the start is heard", g > 40, "gap %.1f ms" % g)
        g, s, b, e = run(exe, 80, 3, start=300)
        check("start cushion 300 ms: the same stall is not heard", g == 0 and s == 1, "gap %.1f ms, silence buffers %d" % (g, s))
        g, s, b, e = run(exe, 80, 4, start=300)
        q = run.queue
        check("start cushion 300 ms is run down to the normal cushion (queue at 30 s between 80 and 190 ms)", 80 <= q <= 190 and g == 0, "queue %.1f ms, gap %.1f ms" % (q, g))
        g, s, b, e = run(exe, 80, 4)
        check("no start cushion: the queue stays at the normal cushion, no speed-up", 60 <= run.queue <= 130 and g == 0, "queue %.1f ms" % run.queue)
        run(exe, 80, 1, 100); full = run.peak
        run(exe, 80, 1, 50); half = run.peak
        run(exe, 80, 1, 0); none = run.peak
        check("volume 100 leaves the samples alone, 50 halves them, 0 silences them", full > 1000 and abs(half * 2 - full) <= full // 20 and none == 0, "peaks %d / %d / %d" % (full, half, none))
        print("ALL PASSED" if ok else "FAILED")
        return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
