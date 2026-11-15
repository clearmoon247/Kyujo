#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Lists the places where the decomp's C reads a local variable that was never assigned (clang -Wuninitialized family).

    uninit_scan.py --build <build dir made by build_native.py>  [--all]

Why: on the console such a read returns whatever the stack slot held, and the matching build is deterministic about it. In the native
build the value is different and, when it is used as a pointer or a loop count, can crash or hang (found: BattleAI_SelectBestScoredMove,
patch 0016). Only -Wuninitialized and -Wsometimes-uninitialized are printed by default; -Wconditional-uninitialized
("may be uninitialized") is mostly out-parameters filled by the callee and is only printed with --all.
This tool only REPORTS; each finding must be read by a person (see docs/known-decomp-issues.md for the ones read so far).
Standard library only; uses clang and the compile settings of compile_decomp.py.
"""
import argparse
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compile_decomp as cd  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True, help="build directory (contains tree/ and game/obj/*.cc.json)")
    ap.add_argument("--target", default="i386-linux-musl")
    ap.add_argument("--all", action="store_true", help="also print the 'may be uninitialized' findings")
    a = ap.parse_args()
    tree = os.path.join(a.build, "tree")
    seen = set()
    for f in sorted(glob.glob(os.path.join(a.build, "game", "obj", "*.cc.json"))):
        rel = json.load(open(f))["result"]["file"]       # the .cc.json files are used only to learn which sources the build compiled
        lib = rel.startswith("lib/")
        # -O1: the analysis that finds "sometimes uninitialized" needs some optimisation; only syntax/analysis is run, nothing is built.
        cmd = ["clang", "--target=" + a.target, "-O1", "-fno-builtin", "-funsigned-char", "-std=gnu89", "-nostdinc", "-fno-common",
               "-fno-strict-aliasing", "-fwrapv", "-Wuninitialized", "-Wsometimes-uninitialized", "-Wconditional-uninitialized"]
        cmd += ["-Wno-error=" + w for w in cd.WARNING_DOWNGRADES]
        cmd += ["-D" + d for d in (cd.ULTRALIB_DEFINES if lib else cd.DEFINES)]
        cmd += ["-I" + os.path.join(tree, i) for i in (cd.ULTRALIB_INCLUDES if lib else cd.INCLUDES)]
        cmd += ["-I" + os.path.join(tree, os.path.dirname(rel)), os.path.join(tree, rel), "-fsyntax-only"]
        for line in subprocess.run(cmd, capture_output=True, text=True).stderr.splitlines():
            if "uninitialized" not in line or "warning:" not in line:
                continue
            if "conditional-uninitialized" in line and not a.all:
                continue
            line = line.replace(tree + "/", "")
            if line not in seen:
                seen.add(line)
                print(line)


if __name__ == "__main__":
    main()
