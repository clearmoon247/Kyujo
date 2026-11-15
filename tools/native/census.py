#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Lists what the natively compiled decomp still needs from "outside", sorted into kinds, so progress can be measured.

    census.py <object-dir> [--json out.json] [--strip-underscore]

--strip-underscore: 32-bit Windows (COFF) decorates every C name with a leading underscore (`_osCreateThread`); this removes it so
the same classification applies. Compiler bookkeeping symbols that start with '.' or '@' (for example `.refptr.<name>` and
`@feat.00`) are ignored on every target.

It reads every .o in <object-dir> (made by compile_decomp.py) with `llvm-nm`, takes the symbols that are referenced but defined by
no object, and sorts them by what kind of thing they are. It also reports global symbols that two objects both define (a link
error in a statically linked build); the first run on the decomp found none.

Kinds, and what each means for the port (see docs/native-port-feasibility.md):
  segment_offset   D_<up to 8 hex digits> whose number is a *segment offset* (e.g. D_1002480 = byte 0x2480 of RSP segment 1).
                   The game addresses its loaded assets as (segment << 24 | offset); the renderer resolves these through a
                   segment table, as the real RCP does. They are constants, not variables to define.
  rom_range        <name>_ROM_START / _ROM_END and similar linker-provided constants: ROM offsets of assets. Derivable from the
                   decomp's yamls/us/rom.yaml.
  address_data     D_8xxxxxxx / jtbl_8xxxxxxx / func_8xxxxxxx: data or code the decomp has not (yet) written in C.
  libultra_os      osXxx and __osXxx: the Nintendo operating-system API. Replaced by this project's own implementation.
  libultra_gu      guXxx: matrix/utility functions. Pure maths; can be compiled from the decomp's lib/ultralib sources.
  libultra_al      alXxx: the audio library. Same treatment as gu where it is plain C.
  compiler_rt      helper routines the compiler itself emits (64-bit division, ...). Supplied by compiler-rt/libgcc.
  other            everything else; listed in full in the report so each can be decided by hand.
"""
import collections
import glob
import json
import os
import sys

# `python3 -I` does not add the script's folder to the import path; do it explicitly, for this repository's own modules only.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import symbols  # noqa: E402


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    strip = "--strip-underscore" in argv
    objs = sorted(glob.glob(os.path.join(argv[1], "**", "*.o"), recursive=True))
    if not objs:
        print("no object files under", argv[1])
        return 2
    defined_by, unresolved = symbols.scan(objs, "_" if strip else "")
    duplicates = sorted(sym for sym, f in defined_by.items() if len(f) > 1)

    kinds = collections.defaultdict(list)
    for s in unresolved:
        kinds[symbols.classify(s)].append(s)

    print("objects: %d   global symbols defined: %d   unresolved: %d   defined twice: %d"
          % (len(objs), len(defined_by), len(unresolved), len(duplicates)))
    for k in ("segment_offset", "rom_range", "address_data", "libultra_os", "libultra_gu", "libultra_al", "compiler_rt", "other"):
        print("  %-15s %5d" % (k, len(kinds[k])))
    if "--json" in argv:
        path = argv[argv.index("--json") + 1]
        with open(path, "w") as f:
            json.dump({"objects": len(objs), "unresolved": {k: v for k, v in kinds.items()}, "duplicates": duplicates}, f, indent=1)
        print("wrote", path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
