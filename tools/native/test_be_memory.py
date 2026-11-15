#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Tests tools/native/be_memory.py (the big-endian memory rewrite). Run:

    test_be_memory.py --sysroot-linux work/sysroot/musl-i386 --sysroot-win work/sysroot/mingw-i686 --out build/be-test

What is checked, and by what evidence:
  1. RUN (linux32 only): tests/native/be/be_cases.c, compiled through the rewrite, executes ~70 checks whose expected values were
     written by hand from the definition of big-endian order (most significant byte at the lowest address).
  2. DATA IMAGES: the same source is compiled for 32-bit big-endian MIPS (the console's CPU, an independent compiler back end). The
     bytes of every initialised test global in that object must equal the bytes the rewritten i386 program holds after the
     start-up reversal. Pointer slots are excluded (their values are addresses) and checked by the run in (1).
  3. REFUSALS: inputs the tool must reject (bit-fields, atomics, aggregate loads, long double, unknown intrinsics, 64-bit pointers)
     are rejected, with a message. A tool that silently translated them would make wrong programs.
  4. BUILD (win32): the same source is compiled and linked for Windows and the finished .exe is inspected statically (section
     layout of the start-up table). It is NOT run: there is no Windows or Wine here.
"""
import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import be_memory       # noqa: E402
import compile_decomp  # noqa: E402
import targets         # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CASES = os.path.join(REPO, "tests", "native", "be", "be_cases.c")
HOST = os.path.join(REPO, "tests", "native", "be", "be_test_host.c")
HOST_BE = os.path.join(REPO, "src", "platform", "host_be.c")

failures = []


def check(ok, what):
    print("  %s  %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        failures.append(what)


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("FAILED: %s\n  command: %s\n%s%s" % (what, " ".join(cmd), r.stdout[-2000:], r.stderr[-4000:]))
    return r


def build(profile, sysroot, out):
    t = targets.TARGETS[profile]
    os.makedirs(out, exist_ok=True)
    res = compile_decomp.compile_one(REPO, t["triple"], "O0", out, CASES, t["cflags"], lib=False, force=True)
    if not res["ok"]:
        sys.exit("be_cases.c failed to compile: %s" % res["first_error"])
    host_flags = targets.host_include_flags(profile, sysroot) + t["cflags"] + ["-std=gnu11", "-Wall", "-Wextra", "-g", "-O0",
                                                                                "-I" + os.path.join(REPO, "src", "platform")]
    objs = [os.path.join(out, "be_cases.o")]
    for src in (HOST, HOST_BE):
        o = os.path.join(out, os.path.basename(src)[:-2] + ".o")
        run(["clang"] + host_flags + ["-c", src, "-o", o], "compile " + src)
        objs.append(o)
    program = os.path.join(out, "be_test" + (".exe" if profile == "win32" else ""))
    run(targets.link_command(profile, sysroot, objs, program), "link")
    return program


# ---- reading the big-endian MIPS object (ELF32, big-endian) with the standard library only ---------------------------------------------

def read_mips_images(path, symbols):
    """{symbol: (bytes, set of byte offsets that are relocated pointer slots)} for the named data symbols of a MIPS o32 ELF object."""
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 1 and d[5] == 2, "expected ELF32 big-endian"
    shoff, = struct.unpack(">I", d[0x20:0x24])
    shentsize, shnum, shstrndx = struct.unpack(">HHH", d[0x2E:0x34])
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack(">IIIIIIIIII", d[shoff + i * shentsize: shoff + (i + 1) * shentsize])
        secs.append(dict(name=name, type=typ, off=off, size=size, link=link, info=info, entsize=entsize))
    shstr = d[secs[shstrndx]["off"]:secs[shstrndx]["off"] + secs[shstrndx]["size"]]

    def sname(s):
        e = shstr.index(b"\0", s["name"])
        return shstr[s["name"]:e].decode()
    for s in secs:
        s["sname"] = sname(s)
    symtab = next(s for s in secs if s["type"] == 2)
    strtab = secs[symtab["link"]]
    strs = d[strtab["off"]:strtab["off"] + strtab["size"]]
    syms = {}
    for k in range(symtab["size"] // 16):
        nm, val, sz, info, other, shndx = struct.unpack(">IIIBBH", d[symtab["off"] + k * 16: symtab["off"] + (k + 1) * 16])
        e = strs.index(b"\0", nm)
        syms[strs[nm:e].decode()] = (val, sz, shndx)
    relocs = {}
    for s in secs:
        if s["type"] == 9:                               # SHT_REL: entries of (offset, info)
            target = s["info"]
            for k in range(s["size"] // 8):
                ro, _ = struct.unpack(">II", d[s["off"] + k * 8: s["off"] + (k + 1) * 8])
                relocs.setdefault(target, set()).add(ro)
    out = {}
    for name in symbols:
        if name not in syms:
            sys.exit("symbol %s not found in the MIPS object" % name)
        val, sz, shndx = syms[name]
        sec = secs[shndx]
        data = d[sec["off"] + val: sec["off"] + val + sz] if sec["type"] != 8 else bytes(sz)
        slots = set()
        for ro in relocs.get(shndx, ()):
            if val <= ro < val + sz:
                slots.update(range(ro - val, ro - val + 4))
        out[name] = (data, slots)
    return out


def test_run_and_images(sysroot_linux, out):
    print("== linux32: run the checks, compare data images with big-endian MIPS")
    program = build("linux32", sysroot_linux, os.path.join(out, "linux32"))
    r = subprocess.run([program, "--dump"], capture_output=True, text=True)
    lines = r.stdout.splitlines()
    fails = [l for l in lines if l.startswith("FAIL ")]
    result = [l for l in lines if l.startswith("RESULT ")]
    for l in fails:
        print("   ", l)
    check(r.returncode == 0 and result and not fails, "game-side checks: " + (result[0][7:] if result else r.stdout[-300:] + r.stderr[-300:]))
    info = [l for l in lines if l.startswith("INFO ")]
    check(bool(info) and not info[0].startswith("INFO 0 "), "start-up reversal found data to fix (" + (info[0][5:] if info else "none") + ")")

    mips = os.path.join(out, "be_cases_mips.o")
    run(["clang", "--target=mips-unknown-linux-gnu", "-c", "-O0", "-G0", "-mabi=32", "-mno-abicalls", "-fno-pic", "-funsigned-char",
         "-fno-builtin", "-std=gnu89", "-nostdinc", CASES, "-o", mip_path(mips)], "compile for MIPS")
    dumped = {}
    for l in lines:
        if l.startswith("DATA "):
            _, name, size, hexs = l.split(" ")
            dumped[name] = bytes.fromhex(hexs)
    images = read_mips_images(mips, sorted(dumped))
    for name in sorted(dumped):
        ref, slots = images[name]
        got = dumped[name]
        diffs = [i for i in range(min(len(ref), len(got))) if ref[i] != got[i] and i not in slots]
        check(len(ref) == len(got) and not diffs,
              "data image of %-9s (%5d bytes, %d pointer bytes excluded) equals the big-endian MIPS image%s" %
              (name, len(ref), len(slots), "" if not diffs else "; first difference at byte %d: MIPS %02x, ours %02x" % (diffs[0], ref[diffs[0]], got[diffs[0]])))


def mip_path(p):
    return p


def test_refusals():
    print("== inputs that must be refused")
    base = ('target datalayout = "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128"\n'
            'target triple = "i386-unknown-linux-musl"\n')

    def refused(body, what, source="x.c", allow=()):
        try:
            be_memory.transform(base + body, source, allow)
        except be_memory.BeError as e:
            check(True, "refused: " + what + "  [" + str(e).splitlines()[0][:70] + "]")
            return
        check(False, "refused: " + what)
    refused('define i32 @f(ptr %p) {\n  %bf.load = load i8, ptr %p\n  ret i32 0\n}\n', "bit-field access")
    refused('define i32 @f(ptr %p) {\n  %v = load atomic i32, ptr %p seq_cst, align 4\n  ret i32 %v\n}\n', "atomic load")
    refused('define void @f(ptr %p) {\n  %v = atomicrmw add ptr %p, i32 1 seq_cst\n  ret void\n}\n', "atomicrmw")
    refused('define x86_fp80 @f(ptr %p) {\n  %v = load x86_fp80, ptr %p, align 4\n  ret x86_fp80 %v\n}\n', "long double load")
    refused('define <4 x i32> @f(ptr %p) {\n  %v = load <4 x i32>, ptr %p, align 4\n  ret <4 x i32> %v\n}\n', "vector load")
    refused('define void @f(ptr %p) {\n  %v = load %struct.A, ptr %p\n  ret void\n}\n', "aggregate load")
    refused('define void @f(ptr %p, i32 %x) {\n  %v = call i32 @llvm.ctlz.i32(i32 %x, i1 false)\n  call void @llvm.gcroot(ptr %p, ptr null)\n  ret void\n}\n', "unreviewed intrinsic")
    refused('define void @f(i32 %x) {\n  call void asm sideeffect "nop", ""()\n  ret void\n}\n', "inline assembly")
    refused('@w = weak global i32 5, align 4\n', "weak initialised global")
    refused('@s = global i32 5, section ".mysec", align 4\n', "global with an explicit section")
    try:
        be_memory.transform(base.replace("p:32:32", "p:64:64") + "\n", "x.c")
        check(False, "refused: 64-bit pointers")
    except be_memory.BeError:
        check(True, "refused: 64-bit pointers")
    # allowed when the file is listed
    try:
        _, st = be_memory.transform(base + 'define i32 @f(ptr %p) {\n  %bf.load = load i8, ptr %p\n  ret i32 0\n}\n', "x.c", {"x.c"})
        check(st["bitfield"] == ["@f"], "a listed bit-field file is accepted and reported")
    except be_memory.BeError:
        check(False, "a listed bit-field file is accepted and reported")
    # a second pass over its own output must not happen silently
    out, _ = be_memory.transform(base + '@g = global [2 x i32] [i32 1, i32 2], align 4\n', "x.c")
    try:
        be_memory.transform(out, "x.c")
        check(False, "refused: processing the same module twice")
    except be_memory.BeError:
        check(True, "refused: processing the same module twice")


def test_constant_folding():
    print("== constants written at build time")
    base = ('target datalayout = "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128"\n'
            'target triple = "i386-unknown-linux-musl"\n')
    text = base + ('define void @f(ptr %p) {\n  store i32 305419896, ptr %p, align 4\n  store i16 4660, ptr %p, align 2\n'
                   '  store i64 72623859790382856, ptr %p, align 8\n  store float 1.500000e+00, ptr %p, align 4\n'
                   '  store double 1.500000e+00, ptr %p, align 8\n  store float 0x3FB99999A0000000, ptr %p, align 4\n'
                   '  store i32 -1, ptr %p, align 4\n  ret void\n}\n')
    out, _ = be_memory.transform(text, "x.c")

    def stored(value_bytes_be, bits):
        """The signed integer that, stored little-endian by the machine, leaves `value_bytes_be` in memory."""
        v = int.from_bytes(value_bytes_be, "little")
        return v - (1 << bits) if v >> (bits - 1) else v
    # expected values come from Python's own big-endian packing, not from be_memory
    cases = [("i32", 32, struct.pack(">I", 0x12345678), "i32 0x12345678"),
             ("i16", 16, struct.pack(">H", 0x1234), "i16 0x1234"),
             ("i64", 64, struct.pack(">Q", 0x0102030405060708), "i64 0x0102030405060708"),
             ("i32", 32, struct.pack(">f", 1.5), "float 1.5"),
             ("i64", 64, struct.pack(">d", 1.5), "double 1.5"),
             ("i32", 32, struct.pack(">f", 0.1), "float 0.1 (written by LLVM as a widened hex double)"),
             ("i32", 32, struct.pack(">i", -1), "all-ones")]
    for ity, bits, be_bytes, what in cases:
        want = "store %s %d, ptr %%p" % (ity, stored(be_bytes, bits))
        check(want in out, "%s is stored as the bytes %s  [%s]" % (what, be_bytes.hex(), want))


def test_stack_slots():
    print("== rule 7: local character arrays get whole 4-byte slots")
    base = ('target datalayout = "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128"\n'
            'target triple = "i386-unknown-linux-musl"\n')
    # The `entry:` label is part of the text on purpose: clang prints it, and a first version of this rule that did not skip it did nothing.
    # Expected sizes are written by hand: 2 -> 4, 3 -> 4, 4 -> 4, 7 -> 8, 8 -> 8, 9 -> 12. Other slots are left alone.
    text = base + ('define void @f() {\nentry:\n  %a = alloca [2 x i8], align 1\n  %b = alloca [3 x i8], align 1\n  %c = alloca [4 x i8], align 1\n'
                   '  %d = alloca [7 x i8], align 1\n  %e = alloca [8 x i8], align 1\n  %f = alloca [9 x i8], align 1\n'
                   '  %g = alloca [3 x i16], align 2\n  %h = alloca i8, align 1\n  %i = alloca ptr, align 4\n  ret void\n}\n')
    out, st = be_memory.transform(text, "x.c")
    for n, want in (("a", 4), ("b", 4), ("c", 4), ("d", 8), ("e", 8), ("f", 12)):
        check(("%%%s = alloca [%d x i8], align 1" % (n, want)) in out, "[%s x i8] slot becomes %d bytes" % ({"a": 2, "b": 3, "c": 4, "d": 7, "e": 8, "f": 9}[n], want))
    check("%g = alloca [3 x i16], align 2" in out and "%h = alloca i8, align 1" in out and "%i = alloca ptr, align 4" in out,
          "slots that are not character arrays are unchanged")
    check(st["padded_arrays"] == 4, "four slots were counted as padded (2, 3, 7, 9)")


def test_return_slot():
    print("== rule 6: a function with a return slot starts by storing zero into it")
    base = ('target datalayout = "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-i128:128-f64:32:64-f80:32-n8:16:32-S128"\n'
            'target triple = "i386-unknown-linux-musl"\n')
    # Same shape as clang's unoptimised output: the `entry:` label first, then the allocas. (The first version of rule 6 expected the
    # allocas on the first line and so never did anything; this test would have caught that.)
    text = base + ('define i32 @f(i32 %x) {\nentry:\n  %retval = alloca i32, align 4\n  %x.addr = alloca i32, align 4\n'
                   '  store i32 %x, ptr %x.addr, align 4\n  ret i32 0\n}\n'
                   'define void @g() {\nentry:\n  %a = alloca i32, align 4\n  ret void\n}\n')
    out, st = be_memory.transform(text, "x.c")
    lines = out.split("\n")
    i = lines.index("  %x.addr = alloca i32, align 4")
    check(lines[i + 1].strip() == "store i32 zeroinitializer, ptr %retval, align 4", "the zero store follows the last leading alloca of f")
    check(st["retzero"] == 1, "only the function with a return slot is touched (g has none)")


def test_win32(sysroot_win, out):
    print("== win32: build and inspect (NOT run)")
    program = build("win32", sysroot_win, os.path.join(out, "win32"))
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "pe_check.py"), program, "--befix"], capture_output=True, text=True)
    print(r.stdout.rstrip())
    check(r.returncode == 0, "pe_check on the Windows test program")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sysroot-linux", required=True)
    ap.add_argument("--sysroot-win")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    test_refusals()
    test_constant_folding()
    test_stack_slots()
    test_return_slot()
    test_run_and_images(args.sysroot_linux, args.out)
    if args.sysroot_win:
        test_win32(args.sysroot_win, args.out)
    print("\n%s (%d check(s) failed)" % ("ALL PASSED" if not failures else "FAILED", len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
