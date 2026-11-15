#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Checks that the game's sound goes on after the in-game menu was open for a while (Linux, needs the user's the game ROM).

Reported on Batocera: "when you exit the menu, there is no more audio". The menu pauses the game, so no sound is produced while it is open.
The suspected cause is a sound server that stops a stream which has been idle for a few seconds and does not restart it by itself. This test uses
the fake server of fake_pulse_server.py with --suspend-after, which MODELS that behaviour (it is a model, not a real server), and runs the real
game with a scripted menu visit (F1 at video field 1000, Esc at field 1500, about 8 s later).
Usage: run_menu_pause_test.py GAME ROM [--no-keepalive]
  default          the program as it is: the menu keeps the stream fed with silence (host_audio_idle_tick)
  --no-keepalive   switches that off, so that the safety net (a new stream after 3 s without sound, device_write) is what is tested
Sound after the menu is checked in the WAV the fake server writes. To see the failure the model describes, run the same with a program built
before the fix.
"""
import math, os, struct, subprocess, sys, tempfile, time, wave
HERE = os.path.dirname(os.path.abspath(__file__))

def main():
    if len(sys.argv) < 3: raise SystemExit(__doc__)
    game, rom = sys.argv[1], sys.argv[2]
    nokeep = "--no-keepalive" in sys.argv
    with tempfile.TemporaryDirectory() as d:
        sock, wav = os.path.join(d, "native"), os.path.join(d, "out.wav")
        srv = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "fake_pulse_server.py"), sock, wav, "48", "--suspend-after", "3"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(50):
            if os.path.exists(sock): break
            time.sleep(0.1)
        env = dict(os.environ, PULSE_SERVER="unix:" + sock, KYUJO_HEADLESS="1", KYUJO_HOME=d,
                   KYUJO_RAW="1000p:F1,1006p:,1500p:ESC,1506p:")
        if nokeep: env["KYUJO_AUDIO_NO_KEEPALIVE"] = "1"
        log = open(os.path.join(d, "game.log"), "w")
        g = subprocess.Popen([game, "--rom", rom, "--save", os.path.join(d, "t.sav")], env=env, stdout=log, stderr=subprocess.STDOUT)
        srv_out = srv.communicate(timeout=90)[0]
        g.kill(); g.wait(); log.close()
        text = open(os.path.join(d, "game.log")).read()
        w = wave.open(wav); n = w.getnframes()
        left = struct.unpack("<%dh" % (n * 2), w.readframes(n))[0::2]
        secs = n / 48000.0
        rms = [int(math.sqrt(sum(v * v for v in left[i:i + 48000]) / max(1, len(left[i:i + 48000])))) for i in range(0, len(left), 48000)]
        print(srv_out.strip())
        print("game log, audio lines:"); [print("  " + l[:170]) for l in text.splitlines() if "audio:" in l and "sample rate" not in l]
        print("menu opened/closed in the log:", "menu opened" in text and "menu closed" in text)
        print("sound received: %.1f s; RMS per second: %s" % (secs, rms))
        ok = "menu closed" in text and secs > 38 and sum(1 for r in rms[-8:] if r > 300) >= 6
        print("PASS  sound goes on after the menu" if ok else "FAIL  the sound stopped after the menu")
        return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
