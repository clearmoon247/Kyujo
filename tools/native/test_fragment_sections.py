#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Checks be_memory.assign_fragment_section (the sections that let the program restore a code overlay's variables on every load).
No compiler needed. Run: python3 -I tools/native/test_fragment_sections.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import be_memory as b  # noqa: E402

failures = 0


def check(name, got, want):
    global failures
    if got != want:
        failures += 1
        print("FAIL %s\n   got  %s\n   want %s" % (name, got, want))
    else:
        print("ok   " + name)


def sec(line, windows=False, idx=5):
    return b.assign_fragment_section(line, idx, windows)


# zero-initialised -> the "zero" section; initialised -> the "data" section; the section goes before the alignment
check("zero scalar (ELF)", sec("@D_1 = dso_local global i8 0, align 1"), '@D_1 = dso_local global i8 0, section "fz05", align 1')
check("zeroinitializer array (ELF)", sec("@s = internal global [8 x i8] zeroinitializer, align 8"),
      '@s = internal global [8 x i8] zeroinitializer, section "fz05", align 8')
check("initialised array (ELF)", sec("@t = internal global [3 x i32] [i32 1, i32 2, i32 3], align 4"),
      '@t = internal global [3 x i32] [i32 1, i32 2, i32 3], section "fd05", align 4')
check("initialised, COFF names", sec("@t = internal global i32 5, align 4", True), '@t = internal global i32 5, section ".fdat$05m", align 4')
check("zero, COFF names", sec("@t = internal global i32 0, align 4", True), '@t = internal global i32 0, section ".fzer$05m", align 4')
check("pointer initialiser with commas inside", sec("@p = dso_local global ptr getelementptr inbounds ([3 x i32], ptr @t, i32 0, i32 1), align 4"),
      '@p = dso_local global ptr getelementptr inbounds ([3 x i32], ptr @t, i32 0, i32 1), section "fd05", align 4')
# left alone
for name, line in (("constant", '@c = internal constant [3 x i8] c"ab\\00", align 1'), ("declaration", "@e = external global i32, align 4"),
                   ("thread local", "@x = dso_local thread_local global i32 0, align 4"), ("llvm.used", "@llvm.used = appending global [1 x ptr] [ptr @f], section \"llvm.metadata\""),
                   ("has a section", '@y = internal global i32 1, section "foo", align 4'), ("weak", "@w = weak global i32 0, align 4")):
    check("unchanged: " + name, sec(line), line)

# the order of a code overlay's variables follows their console addresses (a DMA may run from one variable into the next).
# Addresses come from the names (D_<address>) or from the documented symbols; a variable with no known address stays right behind the
# known one before it (an earlier version put them first, which moved the Game Boy emulator's audio state: Game Boy Tower crash).
out = ['@lead = internal global i32 0, section ".fzer$05m", align 4',            # unknown, before every known variable: stays first
       '@D_86B0EBDC = internal global ptr null, section ".fzer$05m", align 4',
       '@other = internal global i32 0, section ".fzer$05m", align 4',          # unknown: stays behind D_86B0EBDC
       '@D_86B0F648 = internal global [384 x i8] zeroinitializer, section ".fzer$05m", align 1',
       '@D_86B0EBE8 = internal global [2656 x i8] zeroinitializer, section ".fzer$05m", align 1',
       '@f.D_86B0EBC0 = internal global ptr null, section ".fzer$05m", align 4',
       '@t = internal global i32 7, section ".fdat$05m", align 4',
       '@D_86B0DBC0 = internal global i32 3, section ".fdat$05m", align 4',
       'define void @g() {']
b.order_fragment_globals(out, [0, 1, 2, 3, 4, 5, 6, 7])
check("zero section: ascending addresses, unknown variables behind their predecessor", [l.split(" ")[0] for l in out[:6]],
      ["@lead", "@f.D_86B0EBC0", "@D_86B0EBDC", "@other", "@D_86B0EBE8", "@D_86B0F648"])
check("data section ordered on its own", [l.split(" ")[0] for l in out[6:8]], ["@t", "@D_86B0DBC0"])
check("a non-variable line is untouched", out[8], "define void @g() {")

# addresses of documented symbols: by the whole name and by the name after the last dot (function-local statics)
syms = {"sGbApuRegs": 0x8120EA80, "hdwrBugFlag": 0x8120EA50}
out = ['@sGbApuRegs = dso_local global [64 x i8] zeroinitializer, section "fd09", align 1',
       '@D_8120EA60 = dso_local global i32 0, section "fd09", align 4',
       '@osGbSetNextBuffer.hdwrBugFlag = internal global i32 0, section "fd09", align 4']
b.order_fragment_globals(out, [0, 1, 2], syms)
check("documented symbols place variables without a D_ name", [l.split(" ")[0] for l in out],
      ["@osGbSetNextBuffer.hdwrBugFlag", "@D_8120EA60", "@sGbApuRegs"])
check("console_address: unknown name", b.console_address("@zzz = global i32 0", syms), None)

print("all passed" if not failures else "%d FAILED" % failures)
sys.exit(1 if failures else 0)
