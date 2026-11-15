#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Differential test of the software RDP: the same random drawing commands go to a trusted copy of rdp.c and to the working rdp.c, and every
case's picture checksum must be identical. See rdp_diff_fuzz.c for what is generated and what that proves.

usage: run_rdp_diff.py --decomp <decomp checkout> --sysroot <musl-i386 sysroot> --ref <trusted rdp.c> [--cases N] [--seeds a,b,c] [--work <dir>]
  --ref   a copy of src/render/rdp.c from before the change under test (for example `git show HEAD:src/render/rdp.c > /tmp/rdp_ref.c`).
Exit status 0 = identical on every seed; 1 = a difference (the first differing case is printed); 2 = the generator drew nothing (test void).
"""
import argparse, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "native"))
import targets  # noqa: E402


def build(name, rdp_c, a, work, opt):
    flags = targets.host_include_flags("linux32", a.sysroot) + targets.TARGETS["linux32"]["cflags"] + [opt, "-g", "-w", "-DLANGUAGE_C", "-D_LANGUAGE_C",
             "-D_MIPS_SZLONG=32", "-D_MIPS_SZINT=32", "-D_MIPS_SZPTR=32", "-DF3DEX_GBI_2"]
    incs = ["-I" + os.path.join(a.decomp, "lib", "ultralib", "include"), "-I" + os.path.join(ROOT, "src", "render"), "-I" + os.path.join(ROOT, "src", "platform")]
    objs = []
    for s, tag in ((os.path.join(HERE, "rdp_diff_fuzz.c"), "fuzz"), (rdp_c, "rdp")):
        o = os.path.join(work, "%s_%s.o" % (name, tag))
        subprocess.run(["clang", "-c"] + flags + incs + ["-I" + os.path.dirname(os.path.abspath(rdp_c)), "-o", o, s], check=True)
        objs.append(o)
    exe = os.path.join(work, name)
    cmd = [c for c in targets.link_command("linux32", a.sysroot, objs, exe) if not c.startswith("-Wl,--image-base")]
    subprocess.run(cmd, check=True)
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--decomp", required=True)
    ap.add_argument("--sysroot", required=True)
    ap.add_argument("--ref", required=True)
    ap.add_argument("--rdp", default=os.path.join(ROOT, "src", "render", "rdp.c"), help="the rdp.c under test (default: the working tree's)")
    ap.add_argument("--cases", type=int, default=3000)
    ap.add_argument("--seeds", default="1,2,3")
    ap.add_argument("--opt", default="-O2", help="optimisation level for both builds (the game's host code uses -O2)")
    ap.add_argument("--work", default=None)
    a = ap.parse_args()
    work = a.work or tempfile.mkdtemp(prefix="rdp-diff-")
    os.makedirs(work, exist_ok=True)
    ref_dir = os.path.join(work, "ref"); os.makedirs(ref_dir, exist_ok=True)
    # The reference is copied next to nothing else of the tree, but includes the tree's headers (rdp.h, platform.h): they are unchanged API.
    ref = build("fuzz_ref", a.ref, a, work, a.opt)
    new = build("fuzz_new", a.rdp, a, work, a.opt)
    bad = 0
    for seed in a.seeds.split(","):
        outs = []
        for exe in (ref, new):
            r = subprocess.run([exe, seed, str(a.cases)], capture_output=True, text=True)
            if r.returncode != 0:
                print("seed %s: %s exited with status %d\n%s" % (seed, os.path.basename(exe), r.returncode, r.stderr[-500:])); return 1
            outs.append(r)
        info = outs[1].stderr.strip().splitlines()[-1]
        lines = [o.stdout.splitlines() for o in outs]
        diff = next((i for i, (x, y) in enumerate(zip(*lines)) if x != y), None)
        if diff is None and len(lines[0]) == len(lines[1]):
            print("seed %-4s identical  (%s)" % (seed, info))
        else:
            bad += 1
            print("seed %-4s DIFFERENT at case %s: reference %r, new %r" % (seed, diff, lines[0][diff] if diff is not None else None, lines[1][diff] if diff is not None else None))
        if "0 drew pixels" in outs[0].stderr:
            print("the generator drew nothing: the test proves nothing"); return 2
    print("ALL IDENTICAL" if not bad else "%d seed(s) DIFFER" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
