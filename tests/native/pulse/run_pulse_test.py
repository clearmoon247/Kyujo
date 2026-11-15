#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Tests the PulseAudio client (src/platform/host_pulse.c) against the fake server in fake_pulse_server.py, using the probe program.

Checked: the client's login, naming and stream-creation commands are read by the fake server field by field in the order PulseAudio's
protocol-native.c reads them (a wrong tag or an extra/missing field is an error); the flow control (the client sends no more than the server
asked for); the 80 ms cushion of silence at the start; and the sound that arrives is the continuous 440 Hz test tone with nothing dropped or
repeated. A real server may still disagree: this is a test of the code against its own source of knowledge (see host_pulse.c).
Usage: run_pulse_test.py PATH-TO-kyujo_probe
"""
import math, os, struct, subprocess, sys, tempfile, time, wave
HERE = os.path.dirname(os.path.abspath(__file__))
OK = True

def check(name, cond, info=""):
    global OK
    print(("PASS  " if cond else "FAIL  ") + name + ("  " + info if info else ""))
    OK = OK and cond

def main():
    probe = sys.argv[1] if len(sys.argv) > 1 else None
    if not probe or not os.path.exists(probe):
        raise SystemExit(__doc__)
    with tempfile.TemporaryDirectory() as d:
        sock, wav = os.path.join(d, "native"), os.path.join(d, "out.wav")
        srv = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "fake_pulse_server.py"), sock, wav, "5"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(50):
            if os.path.exists(sock): break
            time.sleep(0.1)
        env = {"PATH": os.environ["PATH"], "HOME": d, "PULSE_SERVER": "unix:" + sock}
        out = subprocess.run([probe, "sound"], env=env, capture_output=True, text=True, timeout=30).stdout
        srv_out = srv.communicate(timeout=30)[0]
        check("the probe's sound check finished without failure", "sound check finished" in out and "FAILED" not in out, out.strip().splitlines()[-3:].__str__())
        check("the fake server accepted login, client name and stream", "AUTH ok" in srv_out and "client name" in srv_out and "stream: spec (3, 2, 48000)" in srv_out and "Traceback" not in srv_out, srv_out[-300:])
        check("no sound was dropped", "dropped 0 bytes" in out, [l for l in out.splitlines() if "accepted" in l].__str__())
        w = wave.open(wav)
        n = w.getnframes()
        left = struct.unpack("<%dh" % (n * 2), w.readframes(n))[0::2]
        first = next((i for i, v in enumerate(left) if v), None)
        check("sound arrived", first is not None and n > 90000, "%d frames" % n)
        if first is not None:
            check("80 ms of silence first (the cushion)", abs(first / 48.0 - 80) < 1, "%.1f ms" % (first / 48.0))
            seg = left[first:first + 90000]
            step = max(abs(a - b) for a, b in zip(seg, seg[1:]))
            crossings = sum(1 for a, b in zip(seg[:48000], seg[1:48001]) if a < 0 <= b)
            check("continuous 440 Hz tone (no gap, no repeat)", step <= int(9000 * 2 * math.pi * 440 / 48000) + 1 and abs(crossings - 440) <= 1, "largest step %d, %d periods in one second" % (step, crossings))
    print("ALL PASSED" if OK else "SOME FAILED")
    return 0 if OK else 1

if __name__ == "__main__":
    sys.exit(main())
