#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds a minimal compiler-rt "builtins" archive (libclang_rt.builtins-<arch>.a) for a 32-bit or 64-bit x86 target, or for wasm32.

Why this exists
    A 32-bit x86 program needs helper routines for 64-bit integer division, shifts and int<->float conversion
    (__divdi3, __udivdi3, __ashldi3, ...). GCC supplies them in libgcc; clang supplies them in compiler-rt. The development
    machine has clang but not a compiler-rt build for the targets we need (i386 Linux/musl, i686 and x86_64 Windows/MinGW), so
    this script compiles the same source files that LLVM's own CMake build would compile for those targets.

Where the file list comes from (so nothing here is guessed)
    The list of generic source files is read from the `set(GENERIC_SOURCES ...)` block of compiler-rt/lib/builtins/CMakeLists.txt in
    the LLVM source tree you point it at. The per-architecture additions below are copied from the `i386_SOURCES` /
    `x86_64_SOURCES` blocks of the same file (LLVM 18.1.x). Files that LLVM only adds for features we do not use are skipped and
    listed in SKIPPED, each with a reason.

Headers
    The builtins include <limits.h>, <stdint.h> and similar. The build host's own /usr/include is for a different C library, so
    the compile uses -nostdinc and only two header directories: clang's own freestanding headers and the target's C-library
    headers, given with --sysroot-include (musl's `make install-headers` output, or mingw-w64-headers' install directory).

Usage
    build_builtins.py <llvm-project>/compiler-rt/lib/builtins <clang-triple> <output.a> --sysroot-include DIR [--windows]
    e.g. build_builtins.py .../builtins wasm32-wasi out/libclang_rt.builtins-wasm32.a --sysroot-include wasi-sysroot/include/wasm32-wasi
         build_builtins.py .../builtins i386-linux-musl out/libclang_rt.builtins-i386.a --sysroot-include sysroot/include
         build_builtins.py .../builtins i686-w64-windows-gnu out/libclang_rt.builtins-i686.a --sysroot-include sysroot/include --windows

Needs: clang, llvm-ar on PATH. Standard library only.
"""
import os
import re
import subprocess
import sys
import tempfile

# Files present in LLVM's generic list (or added by it) that we deliberately do not build, and why.
SKIPPED = {
    "emutls.c": "emulated TLS; needs pthreads and is not used by our code",
    "enable_execute_stack.c": "trampoline support; not used",
    "eprintf.c": "assertion helper that pulls in stdio",
    "gcc_personality_v0.c": "C++/unwinding personality routine; not used",
    "clear_cache.c": "instruction-cache flush for JITs; not used",
    "cpu_model/x86.c": "__builtin_cpu_supports support; not used",
}

# From the i386_SOURCES block of compiler-rt/lib/builtins/CMakeLists.txt (LLVM 18).
I386_EXTRA = [
    "i386/ashldi3.S", "i386/ashrdi3.S", "i386/divdi3.S", "i386/floatdidf.S", "i386/floatdisf.S",
    "i386/floatundidf.S", "i386/floatundisf.S", "i386/lshrdi3.S", "i386/moddi3.S", "i386/muldi3.S",
    "i386/udivdi3.S", "i386/umoddi3.S", "i386/floatdixf.S", "i386/floatundixf.S",
]
# From the x86_64_SOURCES block (non-Android). On Windows the .S conversions are replaced by the C versions listed.
X86_64_EXTRA_COMMON = ["x86_64/floatdidf.c", "x86_64/floatdisf.c", "x86_64/floatdixf.c"]
X86_64_EXTRA_NON_WINDOWS = ["x86_64/floatundidf.S", "x86_64/floatundisf.S", "x86_64/floatundixf.S"]
# From the x86_80_BIT_SOURCES block (long double is 80 bits on these targets).
X86_80_BIT = [
    "divxc3.c", "extendxftf2.c", "fixxfdi.c", "fixxfti.c", "fixunsxfdi.c", "fixunsxfsi.c", "fixunsxfti.c",
    "floatdixf.c", "floattixf.c", "floatundixf.c", "floatuntixf.c", "mulxc3.c", "powixf2.c", "trunctfxf2.c",
]
# From the GENERIC_TF_SOURCES block: 128-bit float helpers, used only by x86_64 in LLVM's list.
GENERIC_TF_BLOCK = "GENERIC_TF_SOURCES"


def read_cmake_list(cmake_text, name):
    """Returns the file names inside `set(<name> ... )` (first definition only; no conditionals inside these blocks)."""
    m = re.search(r"set\(%s\s*\n(.*?)\n\s*\)" % re.escape(name), cmake_text, re.S)
    if not m:
        raise SystemExit("could not find set(%s ...) in CMakeLists.txt; the LLVM version may differ from 18.x" % name)
    return [tok for tok in re.split(r"\s+", m.group(1).strip()) if tok and not tok.startswith("#")]


def clang_resource_include():
    """Directory of clang's own freestanding headers (stdint.h, stddef.h, limits.h, ...)."""
    out = subprocess.run(["clang", "--print-resource-dir"], capture_output=True, text=True, check=True).stdout.strip()
    return os.path.join(out, "include")


def main(argv):
    argv = list(argv)
    sysroot_include = None
    if "--sysroot-include" in argv:
        i = argv.index("--sysroot-include")
        sysroot_include = argv[i + 1]
        del argv[i:i + 2]
    args = [a for a in argv[1:] if not a.startswith("--")]
    windows = "--windows" in argv
    if len(args) != 3 or sysroot_include is None:
        print(__doc__)
        return 2
    src_dir, triple, out_path = args
    with open(os.path.join(src_dir, "CMakeLists.txt")) as f:
        cmake = f.read()

    sources = read_cmake_list(cmake, "GENERIC_SOURCES")
    is_64 = triple.startswith(("x86_64", "amd64"))
    if triple.startswith("wasm32"):
        sources += read_cmake_list(cmake, GENERIC_TF_BLOCK)   # LLVM's wasm32_SOURCES: the generic list plus the 128-bit float helpers
    elif is_64:
        sources += read_cmake_list(cmake, GENERIC_TF_BLOCK)
        sources += X86_64_EXTRA_COMMON + X86_80_BIT
        if not windows:
            sources += X86_64_EXTRA_NON_WINDOWS
        else:
            sources += ["x86_64/chkstk.S"]          # stack probe used by large frames on Windows
    else:
        sources += I386_EXTRA + X86_80_BIT + ["i386/fp_mode.c"]
        if windows:
            sources += ["i386/chkstk.S"]            # stack probe (___chkstk_ms) used by large frames on Windows

    sources = [s for s in sources if s not in SKIPPED]
    # De-duplicate while keeping order (x86_80_BIT repeats floatdixf.c on x86_64 in LLVM's own list).
    seen, ordered = set(), []
    for s in sources:
        if s not in seen:
            seen.add(s)
            ordered.append(s)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        objs = []
        for rel in ordered:
            path = os.path.join(src_dir, rel)
            if not os.path.exists(path):
                failures.append((rel, "file not found in this LLVM version"))
                continue
            obj = os.path.join(tmp, rel.replace("/", "_") + ".o")
            cmd = ["clang", "--target=" + triple, "-c", "-O2", "-fno-builtin", "-fvisibility=hidden",
                   "-fomit-frame-pointer", "-ffreestanding", "-nostdinc",
                   "-isystem", clang_resource_include(), "-isystem", sysroot_include,
                   "-I", src_dir, path, "-o", obj]
            if rel.endswith(".S"):
                cmd[cmd.index("-fno-builtin"):cmd.index("-fno-builtin")] = ["-x", "assembler-with-cpp"]
            else:
                cmd[cmd.index("-fno-builtin"):cmd.index("-fno-builtin")] = ["-std=c11"]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                errs = [l for l in r.stderr.splitlines() if "error:" in l]
                failures.append((rel, errs[0] if errs else "compile failed"))
                continue
            objs.append(obj)
        if os.path.exists(out_path):
            os.remove(out_path)
        subprocess.run(["llvm-ar", "rcs", out_path] + objs, check=True)
    print("built %s from %d of %d source files" % (out_path, len(objs), len(ordered)))
    for rel, why in failures:
        print("  NOT BUILT %-28s %s" % (rel, why))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
