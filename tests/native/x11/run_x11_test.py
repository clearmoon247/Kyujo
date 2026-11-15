#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Tests the X11 window code (src/platform/host_x11.c) against a virtual X server (Xvfb), using the probe program tools/linux/kyujo_probe.c.

What is checked:
  1. the probe connects, opens a 640 x 480 window and the keyboard focus arrives;
  2. the test picture really is on the screen: colours of the bars, the grey ramp and the red frame at the corners (so the pixel layout
     B,G,R,X and the 1:1 placement are right) - read from the server's own frame buffer file, not from the program;
  3. fake key presses (XTEST) arrive as the right keys: letter, digit, Enter, arrows, numpad digit, modifiers, punctuation, F-keys;
  3b. full screen on and off (F11) and at start (-f), against fake_wm.py;
  4. login with an X authority cookie: from $XAUTHORITY, from ~/.Xauthority, and from the "-auth" argument of the running server; and a clear
     error when the server wants a cookie and none can be found.
Usage: run_x11_test.py PATH-TO-kyujo_probe        (needs Xvfb and xauth; a 32-bit static probe runs on any 64-bit Linux)
Full screen is tested with fake_wm.py (a tiny window manager written from the EWMH specification); a real manager (openbox on Batocera) has not been tried.
"""
import os, secrets, shutil, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
OK = True

def check(name, cond, info=""):
    global OK
    print(("PASS  " if cond else "FAIL  ") + name + ("  " + info if info else ""))
    OK = OK and cond

def read_xwd(path):
    """The Xvfb frame buffer file is an XWD file; returns (width, height, bytes per line, offset of the first pixel, data)."""
    d = open(path, "rb").read()
    hdr = struct.unpack(">25I", d[:100])
    header_size, w, h, bpp, bpl, ncolors = hdr[0], hdr[4], hdr[5], hdr[11], hdr[12], hdr[19]
    return w, h, bpl, header_size + ncolors * 12, d, bpp

def pixel(img, x, y):
    w, h, bpl, off, d, bpp = img
    p = d[off + y * bpl + x * 4: off + y * bpl + x * 4 + 4]
    return (p[2], p[1], p[0])                        # the server stores B,G,R,X

def start_xvfb(num, fbdir, extra=()):
    p = subprocess.Popen(["Xvfb", ":%d" % num, "-screen", "0", "1280x720x24", "-fbdir", fbdir, "-nolisten", "tcp"] + list(extra),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        if os.path.exists("/tmp/.X11-unix/X%d" % num):
            time.sleep(0.3)
            return p
        time.sleep(0.1)
    raise SystemExit("Xvfb did not start")

def run_probe(probe, num, seconds, env_extra=None, extra=()):
    env = {"PATH": os.environ["PATH"], "DISPLAY": ":%d" % num, "HOME": "/nonexistent"}
    env.update(env_extra or {})
    p = subprocess.Popen([probe, "x11", "-t", str(seconds)] + list(extra), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p

def main():
    probe = sys.argv[1] if len(sys.argv) > 1 else None
    if not probe or not os.path.exists(probe):
        raise SystemExit(__doc__)
    if not shutil.which("Xvfb") or not shutil.which("xauth"):
        print("SKIP  Xvfb or xauth is not installed")
        return 0
    tmp = tempfile.mkdtemp(prefix="x11test")
    num = 90 + os.getpid() % 9
    xv = start_xvfb(num, tmp)
    try:
        # 1 + 2: window, focus, picture
        p = run_probe(probe, num, 12)       # long enough for the 15 key presses below (0.5 s each)
        time.sleep(2.0)
        img = read_xwd(os.path.join(tmp, "Xvfb_screen0"))
        bars = [(255,255,255),(255,255,0),(0,255,255),(0,255,0),(255,0,255),(255,0,0),(0,0,255),(20,20,20)]
        ok_bars = all(pixel(img, i * 80 + 40, 100) == bars[i] for i in range(8))
        check("picture: eight colour bars at the right places", ok_bars, str([pixel(img, i * 80 + 40, 100) for i in range(8)]))
        check("picture: grey ramp", abs(pixel(img, 320, 420)[0] - 127) < 12 and pixel(img, 320, 420)[0] == pixel(img, 320, 420)[2], str(pixel(img, 320, 420)))
        check("picture: red frame reaches the corners", pixel(img, 0, 0) == (255,0,0) and pixel(img, 639, 479) == (255,0,0), str((pixel(img, 0, 0), pixel(img, 639, 479))))
        check("picture: nothing outside the 640 x 480 window", pixel(img, 700, 300) == (0,0,0) and pixel(img, 300, 500) == (0,0,0))
        # 3: keys (letters: A=38, digit 1 = 10, Enter 36, Up 111, KP_7 79, LShift 50, LCtrl 37, LAlt 64, Space 65, comma 59, period 60, slash 61, Delete 119, Insert 118, F11 95)
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "xtest_keys.py"), str(num), "38", "10", "36", "111", "79", "50", "37", "64", "65", "59", "60", "61", "119", "118", "95"], check=True)
        out = p.communicate(timeout=30)[0]
        check("window opened at 640x480 and got the focus", "window size now 640x480" in out and "keyboard focus: yes" in out)
        for name in ("A", "1", "ENTER", "UP", "NUM7", "LSHIFT", "LCTRL", "LALT", "SPACE", "COMMA", "PERIOD", "SLASH", "DELETE", "INSERT", "F11"):
            check("key " + name + " down and up", ("key %-10s down" % name) in out and ("key %-10s up" % name) in out)
        # 3b: full screen, with a fake window manager (Xvfb has none): F11 on, F11 off, and starting in full screen (-f)
        wm = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "fake_wm.py"), str(num)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        p = run_probe(probe, num, 8)
        time.sleep(1.5)
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "xtest_keys.py"), str(num), "95"], check=True)
        time.sleep(1.2)
        full = read_xwd(os.path.join(tmp, "Xvfb_screen0"))
        check("full screen: the picture fills the 1280 x 720 screen (red frame at the far corner)", pixel(full, 1279, 719) == (255,0,0) and pixel(full, 1279, 0) == (255,0,0), str((pixel(full, 1279, 719), pixel(full, 1279, 0))))
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "xtest_keys.py"), str(num), "95"], check=True)
        time.sleep(1.2)
        small = read_xwd(os.path.join(tmp, "Xvfb_screen0"))
        check("full screen off: back to 640 x 480", pixel(small, 700, 300) == (0,0,0) and pixel(small, 639, 479) == (255,0,0), str((pixel(small, 700, 300), pixel(small, 639, 479))))
        out = p.communicate(timeout=30)[0]
        check("full screen reported on, then off, with the screen size, then 640x480 again", out.count("full screen: on") == 1 and "window size now 1280x720" in out and out.rstrip().split("full screen: off")[-1].count("window size now 640x480") >= 0 and out.index("full screen: on") < out.index("window size now 1280x720"), "")
        p = run_probe(probe, num, 3, extra=["-f"])
        out = p.communicate(timeout=30)[0]
        check("started with -f: the window is full screen from the start", "full screen: on" in out and "window size now 1280x720" in out, "")
        wm.terminate(); wm.wait()
        # 4: cookies
        xv.terminate(); xv.wait()
        cookie = secrets.token_hex(16)
        auth = os.path.join(tmp, "authfile")
        open(auth, "w").close()
        subprocess.run(["xauth", "-f", auth, "add", ":%d" % num, "MIT-MAGIC-COOKIE-1", cookie], check=True, stderr=subprocess.DEVNULL)
        xv = subprocess.Popen(["bash", "-c", "exec -a Xorg Xvfb :%d -screen 0 1024x600x24 -nolisten tcp -auth %s" % (num, auth)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.5)
        home = os.path.join(tmp, "home"); os.makedirs(home); shutil.copy(auth, os.path.join(home, ".Xauthority"))
        for label, env, expect in (("$XAUTHORITY", {"XAUTHORITY": auth}, "$XAUTHORITY"),
                                   ("~/.Xauthority", {"HOME": home}, "~/.Xauthority"),
                                   ("the -auth argument of the running X server", {}, "the -auth file of the X server")):
            out = run_probe(probe, num, 1, env).communicate(timeout=30)[0]
            check("login with the cookie from " + label, "connected. authorization: " + expect in out, [l for l in out.splitlines() if "authorization" in l or "FAILED" in l][:1].__str__())
        xv.terminate(); xv.wait()
        xv = subprocess.Popen(["Xvfb", ":%d" % num, "-screen", "0", "800x600x24", "-nolisten", "tcp", "-auth", auth], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  # not named X/Xorg
        time.sleep(1.5)
        out = run_probe(probe, num, 1).communicate(timeout=30)[0]
        check("clear error when a cookie is needed and none can be found", "FAILED: the X server refused the connection: Authorization required" in out, [l for l in out.splitlines() if "FAILED" in l][:1].__str__())
    finally:
        xv.terminate()
        shutil.rmtree(tmp, ignore_errors=True)
    print("ALL PASSED" if OK else "SOME FAILED")
    return 0 if OK else 1

if __name__ == "__main__":
    sys.exit(main())
