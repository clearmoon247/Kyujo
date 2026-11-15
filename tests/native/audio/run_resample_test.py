#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds and runs tests/native/audio/test_resample.c (the sample-rate conversion of src/platform/host_audio.c) and checks its numbers:
flat gain (within 0.1 dB at 1 kHz and 10 kHz), the image of a 12 kHz tone at 20 kHz at least 60 dB down (linear interpolation: about 17 dB),
the same output whether the signal comes in one buffer or many, no wrap-around at full scale, no click when the rate changes.
Usage: run_resample_test.py   (needs gcc, run from anywhere)"""
import os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "test_resample")
        subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra", "-D_WIN32", "-include", "time.h", "-I" + os.path.join(HERE, "fake"), "-I" + os.path.join(REPO, "src", "platform"),
                        os.path.join(HERE, "test_resample.c"), "-o", exe, "-lm"], check=True)
        v = {}
        for line in subprocess.run([exe], capture_output=True, text=True, check=True).stdout.splitlines():
            k, x = line.split(); v[k] = float(x)
        ok = True
        def check(name, cond, info):
            nonlocal ok
            print(("PASS  " if cond else "FAIL  ") + name + "  " + info)
            ok = ok and cond
        check("gain at 1 kHz is 0 dB", abs(v["gain_1k"]) < 0.1, "%.3f dB" % v["gain_1k"])
        check("gain at 10 kHz is 0 dB", abs(v["gain_10k"]) < 0.1, "%.3f dB" % v["gain_10k"])
        check("image of a 12 kHz tone is at least 60 dB down", v["image_12k_db"] < -60, "%.1f dB" % v["image_12k_db"])
        check("one buffer or many give the same samples", v["split_diff"] <= 1, "largest difference %.0f" % v["split_diff"])
        check("full-scale square wave is clamped, not wrapped", v["clamp_ok"] == 1, "")
        check("no click when the rate changes", v["rate_change_ok"] == 1, "")
        print("ALL PASSED" if ok else "FAILED")
        return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
