#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Tests src/game_asm/yay0.c (the C replacement for the game's two hand-written Yay0 decoders) against real data. Run:

    test_yay0.py --sysroot-linux work/sysroot/musl-i386 --rom game.z64 --out build/yay0-test

Evidence this gives, and what it does not:
  * FORMAT: for every "Yay0" block found in the ROM, a decoder written here in Python (different language and structure from the C one,
    but by the same author, from the same description of the format) must find the three streams consistent with each other: the mask
    stream ends where the link table starts, the link table ends exactly where the chunk stream starts, and no stream is read past its
    end. Random data would fail this almost always, so it supports the description of the format; it is not a proof.
  * CODE: the C decoder (compiled through the big-endian rewrite, run as a 32-bit Linux program) must produce, for every block, the same
    output size and hash as the Python decoder, and must not write past the output size.
  * NOT shown: that the original console assembly gives the same bytes. The only reference for that would be the original code
    running on the ROM; the check available here is that the game's data decoded this way is plausible (see the audio check in
    the report) and that the game proceeds past its decoder calls.
The ROM is only read; nothing derived from it is stored in the repository.
"""
import argparse
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compile_decomp  # noqa: E402
import targets         # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(REPO, "src", "game_asm", "yay0.c")
HOST = os.path.join(REPO, "tests", "native", "yay0", "yay0_test_host.c")
HOST_BE = os.path.join(REPO, "src", "platform", "host_be.c")


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def reference_decode(d, o):
    """Decodes the Yay0 block at offset o of bytes d. Returns (output, stream-consistency problems)."""
    size, link_off, chunk_off = struct.unpack_from(">III", d, o + 4)
    mask_pos, link_pos, chunk_pos = o + 16, o + link_off, o + chunk_off
    out = bytearray()
    word, bits = 0, 0
    while len(out) < size:
        if bits == 0:
            word = struct.unpack_from(">I", d, mask_pos)[0]
            mask_pos += 4
            bits = 32
        if word & 0x80000000:
            out.append(d[chunk_pos])
            chunk_pos += 1
        else:
            entry = struct.unpack_from(">H", d, link_pos)[0]
            link_pos += 2
            distance, count = (entry & 0xFFF) + 1, entry >> 12
            if count == 0:
                count = d[chunk_pos] + 0x12
                chunk_pos += 1
            else:
                count += 2
            if distance > len(out):
                raise ValueError("back-reference before the start")
            for _ in range(count):                             # byte by byte: overlapping copies repeat data
                if len(out) < size:
                    out.append(out[-distance])
        word = (word << 1) & 0xFFFFFFFF
        bits -= 1
    problems = []
    if mask_pos > o + link_off:
        problems.append("mask stream runs into the link table")
    if link_pos != o + chunk_off:
        problems.append("link table does not end at the chunk stream")
    return bytes(out), problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sysroot-linux", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    t = targets.TARGETS["linux32"]
    os.makedirs(a.out, exist_ok=True)

    rom = open(a.rom, "rb").read()
    offsets = [m.start() for m in re.finditer(b"Yay0", rom)]
    expected, bad_format = {}, []
    for o in offsets:
        try:
            out, problems = reference_decode(rom, o)
        except (ValueError, struct.error, IndexError) as e:
            bad_format.append((o, str(e)))
            continue
        if problems:
            bad_format.append((o, "; ".join(problems)))
        expected[o] = (len(out), fnv1a(out))
    print("  %s  FORMAT: %d of %d Yay0 blocks have consistent streams%s" %
          ("PASS" if not bad_format and offsets else "FAIL", len(offsets) - len(bad_format), len(offsets),
           "" if not bad_format else " (first problems: %s)" % bad_format[:3]))

    res = compile_decomp.compile_one(REPO, t["triple"], "O2", a.out, SRC, t["cflags"], lib=False, force=True)
    if not res["ok"]:
        sys.exit("yay0.c failed to compile: %s" % res["first_error"])
    flags = targets.host_include_flags("linux32", a.sysroot_linux) + t["cflags"] + ["-std=gnu11", "-Wall", "-Wextra", "-O1", "-g",
                                                                                     "-I" + os.path.join(REPO, "src", "platform")]
    objs = [os.path.join(a.out, "yay0.o")]
    for src in (HOST, HOST_BE):
        o = os.path.join(a.out, os.path.basename(src)[:-2] + ".o")
        r = subprocess.run(["clang"] + flags + ["-c", src, "-o", o], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
        objs.append(o)
    prog = os.path.join(a.out, "yay0_test")
    r = subprocess.run(targets.link_command("linux32", a.sysroot_linux, objs, prog), capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr[-3000:])
    r = subprocess.run([prog, a.rom], capture_output=True, text=True)
    got = {}
    guards_ok = True
    for line in r.stdout.splitlines():
        p = line.split()
        if p[0] == "Y":
            got[int(p[1])] = (int(p[2]), int(p[3]))
            guards_ok &= p[4] == "1"
    print("  %s  C decoder ran on every block (exit status %d)%s" % ("PASS" if r.returncode == 0 and len(got) == len(offsets) else "FAIL",
                                                                   r.returncode, r.stderr.strip()[-200:]))
    mismatches = [o for o in expected if got.get(o) != expected[o]]
    print("  %s  C output equals the Python decoder's for %d of %d blocks (size and hash)" %
          ("PASS" if not mismatches else "FAIL", len(expected) - len(mismatches), len(expected)))
    print("  %s  nothing was written past the decoded size" % ("PASS" if guards_ok else "FAIL"))
    ok = not bad_format and not mismatches and guards_ok and r.returncode == 0 and len(got) == len(offsets)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
