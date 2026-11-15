#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""The native build targets, defined once. Used by compile_decomp.py and build_native.py.

A target answers: which clang triple, which extra flags make the data layout match the N64 build, how the C library and start-up
files are found in a sysroot made by tools/toolchain/build_sysroots.sh, and how a program is linked.

Flag reasons (each was measured; see docs/native-port-feasibility.md section 2.3):
  -fno-pic -fno-pie    absolute addresses are used for the segment constants (`D_1002480` is defined as the number 0x1002480, as the
                       original linker did); position-independent code would route them through a table for no benefit.
  -malign-double       32-bit Linux aligns 64-bit members to 4 bytes inside structs; MIPS o32 (and Windows) align them to 8.
  -mno-ms-bitfields    mingw targets default to Microsoft's bit-field packing; MIPS (and GCC/Linux) do not use it.
  -msse2 -mfpmath=sse  float maths in SSE registers: single precision stays single precision (the x87 unit would compute
                       in extended precision and change results). Not yet validated against the N64 (see the feasibility doc).
"""
import os

# Where the program is loaded. Low, so that the whole image stays below 0x01000000 (see imagelimit.py for the reason). Must be a multiple
# of 64 KiB (the PE rule) and above the first 64 KiB, which both Linux (mmap_min_addr) and Windows keep unmapped.
IMAGE_BASE = 0x20000

COMMON_CFLAGS = ["-fno-pic", "-fno-pie", "-msse2", "-mfpmath=sse"]

# Development only: PORT_EXTRA_CFLAGS adds flags to every GAME-side compile. Used with a separate output directory to build a "checking" program,
# for example PORT_EXTRA_CFLAGS="-fsanitize=array-bounds -fsanitize-trap=array-bounds" makes every indexing past a declared array size stop the
# program with an illegal-instruction trap (found "layout accident" bugs of that kind: see docs/known-decomp-issues.md). It is not part of the
# normal build and the objects are NOT rebuilt when it changes, so always give such a build its own --out directory.
COMMON_CFLAGS += os.environ.get("PORT_EXTRA_CFLAGS", "").split()

TARGETS = {
    "linux32": {
        "triple": "i386-linux-musl",
        "sysroot": "musl-i386",
        "symbol_prefix": "",          # C name -> object-file name
        "object_format": "elf",
        "cflags": COMMON_CFLAGS + ["-malign-double"],
        "program": "kyujo",
    },
    "win32": {
        "triple": "i686-w64-windows-gnu",
        "sysroot": "mingw-i686",
        "symbol_prefix": "_",
        "object_format": "coff",
        "cflags": COMMON_CFLAGS + ["-mno-ms-bitfields"],
        "program": "kyujo.exe",
    },
    # The sandboxed core for the 64-bit ARM ports (Switch, Android): the game and the portable host code compiled to WebAssembly (wasm32 = 32-bit
    # pointers, like the N64 build), translated to C by wabt's wasm2c at build time and compiled for the target by the target's own toolchain.
    # See docs/switch-android-plan.md. The C library is wasi-libc (sysroot "wasi"), the shell supplies the WASI system calls and a few imports.
    "wasm32": {
        "triple": "wasm32-unknown-unknown",
        "sysroot": "wasi",
        "symbol_prefix": "",
        "object_format": "wasm",
        # -mbulk-memory: memcpy/memset as single instructions. -mnontrapping-fptoint: float->int conversions saturate instead of trapping
        # (the console never traps; NaN gives 0 here but INT_MAX on MIPS - not yet checked whether the game ever converts a NaN).
        "cflags": ["-fno-pic", "-fno-pie", "-mbulk-memory", "-mnontrapping-fptoint"] + os.environ.get("PORT_EXTRA_CFLAGS", "").split(),
        "program": "kyujo.wasm",
    },
}


def clang_resource_include(clang="clang"):
    """clang's own headers (stddef.h, stdint.h...): they belong to the compiler, not to a C library."""
    import subprocess
    return os.path.join(subprocess.run([clang, "--print-resource-dir"], capture_output=True, text=True, check=True).stdout.strip(), "include")


def host_include_flags(target, sysroot_dir):
    """Flags that make a HOST-side source file (one that uses the C library) see only clang's headers and the sysroot's."""
    if target == "wasm32":        # wasi-libc: its headers sit in include/wasm32-wasi and need the WASI target to select them
        return ["--target=wasm32-wasi", "-nostdinc", "-isystem", clang_resource_include(),
                "-isystem", os.path.join(sysroot_dir, "include", "wasm32-wasi")]
    return ["--target=" + TARGETS[target]["triple"], "-nostdinc", "-isystem", clang_resource_include(),
            "-isystem", os.path.join(sysroot_dir, "include")]


def windows_builtins(lib, clang="clang"):
    """compiler-rt's 32-bit Windows `builtins` library (64-bit integer maths, float conversions the CPU lacks...). Two toolchains put it in two places:
    the sysroot made by tools/toolchain/build_sysroots.sh keeps it next to the C library as libclang_rt.builtins-i686.a; a ready-made
    llvm-mingw release keeps it under the compiler's own resource directory as lib/windows/libclang_rt.builtins-i386.a."""
    own = os.path.join(lib, "libclang_rt.builtins-i686.a")
    if os.path.isfile(own):
        return own
    import subprocess
    res = subprocess.run([clang, "--print-resource-dir"], capture_output=True, text=True, check=True).stdout.strip()
    theirs = os.path.join(res, "lib", "windows", "libclang_rt.builtins-i386.a")
    if os.path.isfile(theirs):
        return theirs
    raise SystemExit("no compiler-rt builtins library for 32-bit Windows: looked for %s and %s" % (own, theirs))


def link_command(target, sysroot_dir, objects, output, gui=False):
    """argv for linking `objects` into `output`. Mirrors tools/toolchain/smoke_test.sh, which proved this recipe works.
    gui=True (win32 only): a program without a console window (subsystem WINDOWS); the test programs stay console programs."""
    t = TARGETS[target]
    lib = os.path.join(sysroot_dir, "lib")
    cmd = ["clang", "--target=" + t["triple"], "-fuse-ld=lld", "-nostdlib"]
    if target == "linux32":
        rt = os.path.join(lib, "libclang_rt.builtins-i386.a")
        cmd += ["-static", "-no-pie", "-Wl,--image-base=0x%X" % IMAGE_BASE, os.path.join(lib, "crt1.o"), os.path.join(lib, "crti.o")] + objects + \
               ["-L" + lib, "-lc", rt, os.path.join(lib, "crtn.o"), "-Wl,--error-limit=0", "-o", output]
    elif target == "win32":
        rt = windows_builtins(lib)
        # --no-dynamicbase: WITHOUT it lld sets the "can be moved" (ASLR) flag in the exe, and Windows would load the program at a random
        # address. The game keeps pointers in 24-bit fields (see imagelimit.py), so the whole image must stay below 0x01000000 at ITS OWN base.
        # --stack: the main thread gets 8 MiB like a Linux main thread (Windows' default is 1 MiB); the game's own threads get 1 MiB each (os_sched.c).
        # tools/native/pe_check.py checks that the flag is off; host_main.c checks at run time that the image really is at IMAGE_BASE.
        # --strip-debug: in a PE file the DWARF debug sections (host code is compiled with -g) get ADDRESSES inside the image (about 0.4 MiB), which
        # eats the 1.75 MiB room that imagelimit.py requires below 0x01000000. Nothing at run time reads them (no debugger runs on the tester's PC),
        # and the Linux build keeps its debug info (ELF debug sections are not loaded).
        # /Brepro: the PE header's link time stamp is a hash of the program instead of the clock, so building the same sources twice gives the same
        # file (checked by comparing two builds; without it the 4 time bytes were the only difference).
        if gui:
            # No console window: subsystem WINDOWS (minimum Windows 6.0 as before) and the C runtime's WinMainCRTStartup entry, which also calls main() (mingw-w64's
            # crt2.o has both entries and both end in main; this one tells the C runtime the program is a GUI program). The program then writes its own log
            # file (host_main.c). tools/native/pe_check.py --gui checks the subsystem.
            cmd += ["-Wl,--subsystem,windows:6.0", "-Wl,-e,WinMainCRTStartup"]
        cmd += ["-static-libgcc", "-Wl,--image-base=0x%X" % IMAGE_BASE, "-Wl,--no-dynamicbase", "-Wl,/Brepro", "-Wl,--stack,8388608", "-Wl,--strip-debug", os.path.join(lib, "crt2.o"), os.path.join(lib, "crtbegin.o")] + objects + \
               ["-L" + lib, "-lmingw32", rt, "-lmingwex", "-lmsvcrt", "-lkernel32", "-ladvapi32",
               "-luser32", "-lgdi32", "-lwinmm", "-lcomdlg32",     # window, picture drawing, sound output, ROM file dialog (src/platform/host_window.c, host_audio.c)
               "-Wl,--error-limit=0", "-o", output]
    else:
        raise ValueError(target)
    return cmd


def provided_libraries(target, sysroot_dir):
    """Archives whose symbols count as 'already provided' when looking for what is missing."""
    lib = os.path.join(sysroot_dir, "lib")
    if target == "wasm32":
        return [os.path.join(lib, "wasm32-wasi", "libc.a"), os.path.join(lib, "wasm32-wasi", "libclang_rt.builtins-wasm32.a")]
    if target == "linux32":
        return [os.path.join(lib, "libc.a"), os.path.join(lib, "libclang_rt.builtins-i386.a")]
    # crt2.o / crtbegin.o are linked into every program (link_command), and some mingw-w64 versions define C functions in them (the
    # llvm-mingw release defines atexit in crt2.o), so what they define counts as provided too.
    return [os.path.join(lib, n) for n in ("crt2.o", "crtbegin.o", "libmingw32.a", "libmingwex.a", "libmsvcrt.a", "libkernel32.a", "libadvapi32.a",
                                           "libuser32.a", "libgdi32.a", "libwinmm.a", "libcomdlg32.a")] + [windows_builtins(lib)]
