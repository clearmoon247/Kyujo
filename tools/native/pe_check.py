#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Static checks on a 32-bit Windows executable, for a machine that cannot run it.

    pe_check.py program.exe [--gui] [--constant 0x01002480] [--befix] [--fragdata]

Checks (each prints PASS/FAIL; exit status 1 if any FAIL):
  machine     the file is a PE image for 32-bit x86 (IMAGE_FILE_MACHINE_I386)
  subsystem   console program (or with --gui: a program without a console window, subsystem WINDOWS), minimum Windows version 6.0 or newer
              (header fields only)
  imports     every imported DLL is KERNEL32.dll, a Windows 10+ "api-ms-win-crt-*" Universal C Runtime forwarder, or one of the system DLLs
              of the window / sound / file dialog (USER32, GDI32, WINMM, COMDLG32; present on every Windows version)
              (per Microsoft's documentation these ship with Windows 10; this check cannot prove they exist on a given machine)
  constant    (optional) the 4-byte value appears in the image, and no base-relocation entry covers any occurrence that lies inside
              a section. The game uses absolute numbers such as 0x01002480 as addresses (see gen_platform_symbols.py). If the loader
              moved the image and a base relocation covered such a number, it would be shifted and the game's display lists
              would point to the wrong place.

  befix       (optional) the start-up table of the big-endian memory rewrite (src/platform/host_be.c): the merged `.befix` section
              must begin and end with a zero word (the two markers), and every word between them is either zero (padding) or the
              address of a function inside the code section (at least one). A wrong section order or a missing table would make
              Windows start the game with un-reversed data, which no other static check would notice.

What this does NOT show: that the program starts. That needs Windows.
Standard library only; the PE format is parsed directly (the layout used is the one documented in Microsoft's "PE Format" page).
"""
import re
import struct
import subprocess
import sys


def parse(path):
    d = open(path, "rb").read()
    if d[:2] != b"MZ":
        raise SystemExit("not a PE file (no MZ header)")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE file (no PE signature)")
    machine, nsec = struct.unpack_from("<HH", d, pe + 4)
    opt_size = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    sections = []
    for i in range(nsec):
        name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", d, opt + opt_size + i * 40)
        sections.append((name.rstrip(b"\0").decode(), va, vsize, rptr, rsize))
    return d, pe, machine, magic, opt, sections


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    path = argv[0]
    constant = int(argv[argv.index("--constant") + 1], 0) if "--constant" in argv else None
    d, pe, machine, magic, opt, sections = parse(path)
    failed = False

    def report(ok, what):
        nonlocal failed
        failed |= not ok
        print("  %s  %s" % ("PASS" if ok else "FAIL", what))

    report(machine == 0x14C and magic == 0x10B, "machine type is 32-bit x86 (0x%X, optional header magic 0x%X)" % (machine, magic))
    major_os, minor_os = struct.unpack_from("<HH", d, opt + 40)
    subsystem = struct.unpack_from("<H", d, opt + 68)[0]
    want_gui = "--gui" in argv
    report(subsystem == (2 if want_gui else 3) and major_os >= 6,
           "%s subsystem, minimum OS %d.%d" % ("windows (no console window)" if want_gui else "console", major_os, minor_os))
    # The entry point must be the C runtime's start function that matches the subsystem. mingw-w64's two start functions both end in the same code
    # (which calls main); the first instructions are `mov eax,[__mingw_app_type]; mov dword [eax], N` with N = 1 for the window entry
    # (WinMainCRTStartup) and 0 for the console entry (mainCRTStartup). Checked here so that a wrong -e option cannot go unnoticed.
    entry_rva = struct.unpack_from("<I", d, opt + 16)[0]
    entry_ok = False
    for (_, va, vsize, rptr, rsize) in sections:
        if va <= entry_rva < va + max(vsize, rsize):
            code = d[rptr + (entry_rva - va): rptr + (entry_rva - va) + 11]
            entry_ok = len(code) == 11 and code[0] == 0xA1 and code[5:7] == b"\xC7\x00" and code[7:11] == struct.pack("<I", 1 if want_gui else 0)
    report(entry_ok, "entry point is the C runtime's %s start function (sets __mingw_app_type to %d)" % ("window" if want_gui else "console", 1 if want_gui else 0))

    # The game stores pointers in 24-bit fields, so the program must run at its own link address (below 0x01000000). If the "can be moved"
    # (ASLR, IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE = 0x40) flag were set, Windows would load it at a random address and the game would break.
    image_base = struct.unpack_from("<I", d, opt + 28)[0]
    dll_chars = struct.unpack_from("<H", d, opt + 70)[0]
    report(not (dll_chars & 0x40), "ASLR flag (DYNAMIC_BASE) is OFF, so Windows tries the image base 0x%X first (DllCharacteristics 0x%04X)" % (image_base, dll_chars))

    out = subprocess.run(["llvm-readobj", "--coff-imports", path], capture_output=True, text=True, check=True).stdout
    dlls = sorted(set(re.findall(r"Name: (\S+\.dll)", out, re.IGNORECASE)))
    allowed = {"KERNEL32.DLL", "USER32.DLL", "GDI32.DLL", "WINMM.DLL", "COMDLG32.DLL"}
    bad = [x for x in dlls if not (x.upper() in allowed or x.lower().startswith("api-ms-win-crt-"))]
    report(not bad, "imports only KERNEL32, USER32, GDI32, WINMM, COMDLG32 and api-ms-win-crt-* (%d DLLs)%s" % (len(dlls), "" if not bad else "; unexpected: " + ", ".join(bad)))

    if constant is not None:
        pat = struct.pack("<I", constant)
        dd_off = opt + 96 + 5 * 8                                  # data directory 5 = base relocations
        rel_rva, rel_size = struct.unpack_from("<II", d, dd_off)
        covered = set()
        if rel_size:
            sec = next(s for s in sections if s[1] <= rel_rva < s[1] + max(s[2], s[4]))
            o, end = sec[3] + (rel_rva - sec[1]), sec[3] + (rel_rva - sec[1]) + rel_size
            while o < end:
                page, blk = struct.unpack_from("<II", d, o)
                for k in range((blk - 8) // 2):
                    e = struct.unpack_from("<H", d, o + 8 + 2 * k)[0]
                    if e >> 12 == 3:                               # IMAGE_REL_BASED_HIGHLOW: a 32-bit address that gets moved
                        covered.add(page + (e & 0xFFF))
                o += blk
        in_sections, pos = [], -1
        while True:
            pos = d.find(pat, pos + 1)
            if pos < 0:
                break
            for name, va, vsize, rptr, rsize in sections:
                if rptr <= pos < rptr + rsize:
                    in_sections.append((name, va + pos - rptr))
        moved = [(n, hex(r)) for n, r in in_sections if r in covered]
        report(bool(in_sections), "constant 0x%08X present in a section of the image (%d place(s): %s)"
               % (constant, len(in_sections), ", ".join("%s+RVA %s" % (n, hex(r)) for n, r in in_sections)))
        report(not moved, "constant is not covered by a base relocation (%d relocation entries in the image)" % len(covered))
    if "--befix" in argv:
        image_base = struct.unpack_from("<I", d, opt + 28)[0]
        sec = next((x for x in sections if x[0] == ".befix"), None)
        report(sec is not None, "section .befix exists (the sections .befix$a/$m/$z were merged by the linker)")
        if sec:
            name, va, vsize, rptr, rsize = sec
            words = list(struct.unpack_from("<%dI" % (vsize // 4), d, rptr))
            text = next(x for x in sections if x[0] == ".text")
            entries = [w for w in words[1:-1] if w]
            in_text = [w for w in entries if text[1] <= w - image_base < text[1] + text[2]]
            report(len(words) >= 3 and words[0] == 0 and words[-1] == 0, ".befix starts and ends with the zero markers (%d words)" % len(words))
            report(len(entries) >= 1 and len(in_text) == len(entries),
                   ".befix holds %d function address(es), all inside .text" % len(entries))
    if "--fragdata" in argv:
        # The code overlays' variables (be_memory.assign_fragment_section): all overlays share two merged sections, because a PE file may
        # have at most 96 sections. The order of the pieces inside (begin marker, variables, end marker per overlay) is the linker's
        # sort by the part after `$`; it was verified once with llvm-objdump -t (marker symbols gFragNNdBegin < gFragNNdEnd).
        names = [x[0] for x in sections]
        report(".fdat" in names and ".fzer" in names, "sections .fdat and .fzer exist (overlay variables, merged from .fdat$NN? / .fzer$NN?)")
        report(len(sections) <= 96, "the image has %d sections (the Windows loader accepts at most 96)" % len(sections))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
