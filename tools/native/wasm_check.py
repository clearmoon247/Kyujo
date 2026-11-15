#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Checks the linked WebAssembly core (build/kyujo.wasm) for the layout facts the game relies on.

    wasm_check.py kyujo.wasm [--wasm-objdump path]

  * The program's memory (static data end) stays below 0x01000000 with room to spare (the same rule as imagelimit.py for the other targets).
  * gMainImage (the RAM image of the main program) lies inside that range.
  * The start-up marker pairs are in order: be_fixups begin < end, and for every overlay the data and zero sections' markers are in order and
    inside the program image (wasm-ld joins same-named sections in link order; this is the check that it really did).
Exit status 0 if all pass. Standard library only."""
import argparse
import re
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wasm")
    ap.add_argument("--wasm-objdump", default="wasm-objdump")
    a = ap.parse_args()
    out = subprocess.run([a.wasm_objdump, "-x", a.wasm], capture_output=True, text=True, check=True).stdout
    vals = {}
    for m in re.finditer(r"global\[(\d+)\] i32 mutable=\d <([^>]*)> - init i32=(-?\d+)", out):
        vals[m.group(2)] = int(m.group(3)) & 0xFFFFFFFF
    exported = set(re.findall(r'global\[\d+\] -> "([^"]+)"', out))
    problems = []
    def need(name):
        if name not in vals or name not in exported:
            problems.append("missing export: " + name)
            return None
        return vals[name]
    data_end, heap_base, image = need("__data_end"), need("__heap_base"), need("gMainImage")
    print("static data ends at 0x%X, heap starts at 0x%X, gMainImage at 0x%X" % (data_end or 0, heap_base or 0, image or 0))
    if data_end is not None and data_end > 0x01000000 - 0x100000:
        problems.append("the static data reaches 0x%X: less than 1 MiB left below 0x01000000" % data_end)
    if image is not None and data_end is not None and not (0 < image < data_end):
        problems.append("gMainImage is outside the static data")
    b, e = need("gBeFixupsBegin"), need("gBeFixupsEnd")
    if b is not None and e is not None:
        print("be_fixups: 0x%X .. 0x%X (%d entries)" % (b, e, (e - b) // 4 - 1))
        if not b < e or (e - b) // 4 - 1 < 1:
            problems.append("be_fixups markers out of order or empty")
    frags = sorted({int(n[5:7]) for n in vals if re.fullmatch(r"gFrag\d\dd(Begin|End)", n)})
    for i in frags:
        for t in "dz":
            lo, hi = vals.get("gFrag%02d%sBegin" % (i, t)), vals.get("gFrag%02d%sEnd" % (i, t))
            if lo is None or hi is None or not lo < hi or hi > data_end:
                problems.append("overlay %02d %s markers: 0x%s..0x%s" % (i, t, "%X" % lo if lo is not None else "?", "%X" % hi if hi is not None else "?"))
    print("%d overlays with variable sections checked" % len(frags))
    for p in problems:
        print("PROBLEM:", p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
