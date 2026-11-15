#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Tests src/game_asm/libgsm.c against the console's ORIGINAL machine code. Run:

    test_gsm.py --sysroot-linux work/sysroot/musl-i386 --rom game.z64 --out build/gsm-test

What it does: the 0x600 bytes of the game's hand-written libgsm assembly are read from the player's ROM, placed at the console address they
had (0x80048590) in a flat memory image, and run by a small MIPS interpreter (tests/native/mips_ref). The C replacement is compiled the way
the game uses it (through the big-endian rewrite) and run on identical memory. Both must leave identical memory behind for every random
case, including extreme values (the original wraps around where the GSM standard saturates, and so must the C).
The C is tested compiled at -O0 (what the build uses today) and at -O2 (to catch code that only works by accident of unoptimised output).

What this does NOT show: that the interpreter itself is right (it is small and written from the instruction manuals; it is also used by
tests of other replacements, and it executes this game's own code to completion without hitting an unsupported instruction, which a wrong
decoder would be unlikely to do), nor anything about inputs outside the ranges listed in the host program.
The ROM is only read; nothing derived from it is stored in the repository.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compile_decomp  # noqa: E402
import targets         # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(REPO, "src", "game_asm", "libgsm.c")
TESTS = os.path.join(REPO, "tests", "native")
HOST_BE = os.path.join(REPO, "src", "platform", "host_be.c")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sysroot-linux", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--src", default=SRC, help="file under test (default: src/game_asm/libgsm.c); used to try deliberately broken copies")
    a = ap.parse_args()
    t = targets.TARGETS["linux32"]
    ok_all = True
    for opt in ("O0", "O2"):
        out = os.path.join(a.out, opt)
        os.makedirs(out, exist_ok=True)
        res = compile_decomp.compile_one(REPO, t["triple"], opt, out, a.src, t["cflags"], lib=False, force=True)
        if not res["ok"]:
            sys.exit("libgsm.c failed to compile at %s: %s" % (opt, res["first_error"]))
        flags = targets.host_include_flags("linux32", a.sysroot_linux) + t["cflags"] + \
            ["-std=gnu11", "-Wall", "-Wextra", "-O1", "-g", "-I" + os.path.join(REPO, "src", "platform"), "-I" + os.path.join(TESTS, "mips_ref")]
        objs = [os.path.join(out, os.path.basename(a.src)[:-2] + ".o")]
        for src in (os.path.join(TESTS, "gsm", "gsm_test_host.c"), os.path.join(TESTS, "mips_ref", "mips_ref.c"), HOST_BE):
            o = os.path.join(out, os.path.basename(src)[:-2] + ".o")
            r = subprocess.run(["clang"] + flags + ["-c", src, "-o", o], capture_output=True, text=True)
            if r.returncode:
                sys.exit(r.stderr)
            objs.append(o)
        prog = os.path.join(out, "gsm_test")
        r = subprocess.run(targets.link_command("linux32", a.sysroot_linux, objs, prog), capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr[-3000:])
        r = subprocess.run([prog, a.rom], capture_output=True, text=True)
        lines = [l.split() for l in r.stdout.splitlines()]
        for name, cases, bad in lines:
            good = int(bad) == 0
            ok_all &= good
            print("  %s  %-36s %s: %s cases, %s mismatches" % ("PASS" if good else "FAIL", name, opt, cases, bad))
        if r.returncode != 0 or len(lines) != 3:
            ok_all = False
            print("  FAIL  test program exit status %d, %d result lines\n%s" % (r.returncode, len(lines), r.stderr.strip()[-1500:]))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
