#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""ubsan_triage.py <run log> <patched source tree>  -  lists each distinct array-bounds report of a bounds-checking run once.

How the reports come about (see docs/known-decomp-issues.md, "Array-bounds checking"):
  1. Build with   PORT_EXTRA_CFLAGS="-fsanitize=array-bounds -fsanitize-recover=array-bounds"   (and a separate --out directory).
     The game code then calls a handler (src/platform/host_ubsan.c) whenever an array is indexed outside its DECLARED size. The handler
     prints one line per site and index and keeps running:
        ubsan: array index out of bounds at <path>/tree/<file>:<line>:<col> (index N; array type T; hit K)
  2. Play the game with that build and keep stderr in a log file.
  3. Run this script. For every source line it prints how often it fired, the indices seen, the declared array type, and the source text, so
     a human can decide for each one:
       - benign : the array is a member of a struct whose real size is larger than the decomp declares, or one table that the decomp split into
                  several neighbouring symbols (the console's linker kept them adjacent; so does the native build, check with `nm -n`);
       - suspect: a stand-alone variable indexed past its end. On the console that reads or writes whatever the linker put next; natively that
                  may be something else (the "layout accident" class, patches 0017, 0018, 0020, 0021, 0022).
This script only sorts and prints; it decides nothing. The old log format without "array type" is accepted too.
"""
import collections
import re
import sys

REPORT = re.compile(r"ubsan: array index out of bounds at .*?/tree/(.+?):(\d+):(\d+) "
                    r"\(index (-?\d+);(?: array type (.*?);)? hit (\d+)\)")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    log, tree = sys.argv[1], sys.argv[2]
    sites = collections.OrderedDict()                  # (file, line) -> [largest hit count, set of indices, set of array types]
    with open(log, errors="replace") as f:
        for line in f:
            m = REPORT.match(line)
            if not m:
                continue
            path, ln, _col, idx, ty, hits = m.group(1), int(m.group(2)), m.group(3), int(m.group(4)), m.group(5) or "?", int(m.group(6))
            e = sites.setdefault((path, ln), [0, set(), set()])
            e[0] = max(e[0], hits)
            e[1].add(idx)
            e[2].add(ty)
    for (path, ln), (hits, idxs, tys) in sorted(sites.items()):
        try:
            with open("%s/%s" % (tree, path), errors="replace") as src:
                text = src.read().split("\n")[ln - 1].strip()
        except (OSError, IndexError):
            text = "?"
        print("%s:%d  hits>=%d idx=%s type=%s | %s" % (path, ln, hits, sorted(idxs)[:4], sorted(tys)[:1], text[:90]))
    print("%d distinct sites" % len(sites))


if __name__ == "__main__":
    main()
