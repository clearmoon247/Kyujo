#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Tests for ir_sizes.raise_alignment (run: python3 -I tools/native/test_ir_align.py). Standard library only."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir_sizes  # noqa: E402

LAYOUT = 'target datalayout = "E-m:e-p:32:32-i8:8:32-i16:16:32-i64:64-n32:64-S128"\n'

IR = LAYOUT + r'''
@small = dso_local global i32 5, align 4
@arr4 = dso_local constant [633 x i32] zeroinitializer, align 4
@arr_ok = dso_local global [4 x i32] zeroinitializer, align 16
@bytes = dso_local global [12 x i8] zeroinitializer
@str = private unnamed_addr constant [4 x i8] c"abc\00", align 1
@ext = external global [100 x i32]
@llvm.used = appending global [1 x ptr] [ptr @small], section "llvm.metadata"
@strct = dso_local global { i32, i32, i32 } zeroinitializer, align 4
@sec = dso_local constant [8 x i8] zeroinitializer, section ".rodata", align 1
'''


def main():
    out, n = ir_sizes.raise_alignment(IR)
    lines = {l.split(" = ")[0]: l for l in out.splitlines() if l.startswith("@")}
    checks = [
        ("@small", ", align 4", "4-byte scalar stays"),
        ("@arr4", ", align 8", "2532-byte table is raised to 8"),
        ("@arr_ok", ", align 16", "already 16: untouched"),
        ("@bytes", ", align 8", "12 bytes without an align clause gets one"),
        ("@str", ", align 8", "a 4-byte string stays"),            # replaced below: it must NOT change
        ("@strct", ", align 8", "12-byte struct is raised"),
        ("@sec", ", align 8", "8-byte object with a section keeps the section"),
    ]
    bad = 0
    for name, want, why in checks:
        got = lines[name].endswith(want)
        if name == "@str":
            got = lines[name].endswith(", align 1")
        if not got:
            bad += 1
            print("FAIL %s: %s -> %s" % (why, name, lines[name]))
    if "@ext = external global [100 x i32]" not in out:
        bad += 1
        print("FAIL: an external declaration changed")
    if "@llvm.used = appending global [1 x ptr] [ptr @small], section \"llvm.metadata\"" not in out:
        bad += 1
        print("FAIL: @llvm.* changed")
    if 'section ".rodata"' not in lines["@sec"]:
        bad += 1
        print("FAIL: the section clause was lost")
    if n != 4:
        bad += 1
        print("FAIL: expected 4 rewritten globals, got %d" % n)
    out2, n2 = ir_sizes.raise_alignment(out)
    if n2 != 0 or out2 != out:
        bad += 1
        print("FAIL: not idempotent")
    print("test_ir_align: %s" % ("FAILED (%d)" % bad if bad else "ok"))
    sys.exit(1 if bad else 0)


main()
