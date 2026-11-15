#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Replaces the function addresses (0x1a23a0) in a call-trace log (src/platform/host_trace.c) by names from the build's kyujo.syms.

    trace_resolve.py movefx-Gengar-Night-Shade.log kyujo.syms > resolved.log

The .syms file ("<hex address> <name>" per line) is written next to the program by tools/native/build_native.py. It must come from the SAME build as the
program that wrote the log (addresses differ between builds). Lines that already carry names are left alone. Standard library only."""
import re
import sys


def load(path):
    table = {}
    with open(path) as f:
        for line in f:
            parts = line.split(None, 1)
            if len(parts) == 2:
                try:
                    table[int(parts[0], 16)] = parts[1].strip()
                except ValueError:
                    pass
    return table


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    table = load(sys.argv[2])
    unknown = 0

    def sub(m):
        nonlocal unknown
        name = table.get(int(m.group(0), 16))
        if name is None:
            unknown += 1
            return m.group(0)
        return name

    with open(sys.argv[1]) as f:
        for line in f:
            sys.stdout.write(re.sub(r"\b0x[0-9a-fA-F]+\b", sub, line) if not line.startswith("#") else line)
    if unknown:
        sys.stderr.write("%d address(es) not in the symbol file (a different build?)\n" % unknown)


if __name__ == "__main__":
    main()
