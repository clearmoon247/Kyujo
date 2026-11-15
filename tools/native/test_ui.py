#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds and runs tests/native/ui/ui_test_host.c: the settings file, the key and gamepad bindings and the path helpers of the menu.

    test_ui.py --sysroot-linux work/sysroot/musl-i386 --out build/ui-test

The test is compiled for the Linux 32-bit target (the one this repository can run) together with src/platform/host_settings.c, host_keys.c,
host_input.c, host_paths.c and host_cheat.c, which are the real files of the program; what they need from the rest of the program is stubbed in the test.
It shows that the bindings do what the README says (default keys, rebinding, ports, dead zone) and that the settings text survives a round
trip. It does not show anything about the window layer, which reads the real keyboard and gamepads, or about how the menu looks.
Standard library only.
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import targets  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PLATFORM = os.path.join(REPO, "src", "platform")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sysroot-linux", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    t = targets.TARGETS["linux32"]
    os.makedirs(a.out, exist_ok=True)
    flags = targets.host_include_flags("linux32", a.sysroot_linux) + t["cflags"] + ["-std=gnu11", "-Wall", "-Wextra", "-O1", "-g", "-I" + PLATFORM]
    objs = []
    for src in (os.path.join(REPO, "tests", "native", "ui", "ui_test_host.c"),) + tuple(os.path.join(PLATFORM, f) for f in ("host_settings.c", "host_keys.c", "host_input.c", "host_paths.c", "host_cheat.c")):
        o = os.path.join(a.out, os.path.basename(src)[:-2] + ".o")
        r = subprocess.run(["clang"] + flags + ["-c", src, "-o", o], capture_output=True, text=True)
        if r.returncode or r.stderr.strip():
            sys.exit("compile of %s:\n%s" % (src, r.stderr))
        objs.append(o)
    prog = os.path.join(a.out, "ui_test")
    r = subprocess.run(targets.link_command("linux32", a.sysroot_linux, objs, prog), capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr[-3000:])
    r = subprocess.run([prog], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode or "UI TEST PASSED" not in r.stdout:
        sys.stderr.write(r.stderr)
        sys.exit("ui test FAILED (exit %d)" % r.returncode)


if __name__ == "__main__":
    main()
