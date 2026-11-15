#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Checks that a finished program keeps all of its memory below the address where the game's "segment addresses" begin.

    imagelimit.py build/linux32/kyujo            # prints the image's first and last address and the room left

Why this matters. The game passes memory addresses to the graphics hardware in two different forms: a plain address (a pointer into
the memory pool, a vertex array, ...) and a SEGMENTED address, whose top byte is a segment number 1..15 (0x01000000 .. 0x0FFFFFFF) and
whose remaining 24 bits are an offset (include/variables.h, src/memmap.c). In this port a pointer is the real address of the data, so a
pointer whose top byte is 1..15 is indistinguishable from a segmented address: the program would misread it WITHOUT any error. The only
safe rule is that every address the game can pass on lies below 0x01000000 (16 MiB). The program's own image (code, data, the memory pool
array) is the bulk of that, so the link places the image low (targets.py, IMAGE_BASE) and this check fails the build if its end ever
reaches the limit. It reads the file's headers only; ELF32 and PE32 are understood.
"""
import struct
import sys

LIMIT = 0x01000000
MIN_ROOM = 0x001C0000       # require 1.75 MiB of room: the game threads' stacks are inside the image (host_mips.c), so this is only a margin for growth


def image_extent(path):
    """(first address, one past the last address) of the memory image described by the file's headers."""
    d = open(path, "rb").read()
    if d[:4] == b"\x7fELF":
        if d[4] != 1:
            raise SystemExit("%s: not a 32-bit ELF file" % path)
        e_phoff, = struct.unpack_from("<I", d, 0x1C)
        e_phentsize, e_phnum = struct.unpack_from("<HH", d, 0x2A)
        lo, hi = None, 0
        for i in range(e_phnum):
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from("<8I", d, e_phoff + i * e_phentsize)
            if p_type == 1:                                  # PT_LOAD
                lo = p_vaddr if lo is None else min(lo, p_vaddr)
                hi = max(hi, p_vaddr + p_memsz)
        return lo, hi
    if d[:2] == b"MZ":
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        opt = pe + 24
        image_base, = struct.unpack_from("<I", d, opt + 28)
        size_of_image, = struct.unpack_from("<I", d, opt + 56)
        return image_base, image_base + size_of_image
    raise SystemExit("%s: neither an ELF nor a PE file" % path)


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    lo, hi = image_extent(argv[0])
    room = LIMIT - hi
    print("image 0x%08X .. 0x%08X (%.2f MiB); room below 0x%08X: %.2f MiB" % (lo, hi, (hi - lo) / 1048576.0, LIMIT, room / 1048576.0))
    if room < MIN_ROOM:
        print("FAIL: less than %.1f MiB of room left. Addresses at or above 0x%08X would be taken for segment numbers." % (MIN_ROOM / 1048576.0, LIMIT))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
