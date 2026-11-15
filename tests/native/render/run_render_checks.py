#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds and runs tests/native/render/rdp_gbi_check.c (renderer) and scale_check.c (window picture scaler) for the 32-bit Linux target (the renderer treats native pointers as game addresses,
so it cannot be tested as a 64-bit program). Needs the clang toolchain and the musl sysroot that tools/toolchain/build_sysroots.sh makes.

usage: run_render_checks.py --decomp <decomp checkout> --sysroot <musl-i386 sysroot> [--work <scratch dir>]
Exit status is that of the test (0 = all checks passed).
"""
import argparse, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "native"))
import targets  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--decomp", required=True, help="the unmodified decomp checkout (for lib/ultralib/include/PR/gbi.h)")
    ap.add_argument("--sysroot", required=True, help="musl-i386 sysroot directory")
    ap.add_argument("--work", default=None)
    a = ap.parse_args()
    work = a.work or tempfile.mkdtemp(prefix="render-checks-")
    os.makedirs(work, exist_ok=True)
    flags = targets.host_include_flags("linux32", a.sysroot) + targets.TARGETS["linux32"]["cflags"] + ["-O1", "-g", "-w", "-DLANGUAGE_C", "-D_LANGUAGE_C", "-D_MIPS_SZLONG=32", "-D_MIPS_SZINT=32", "-D_MIPS_SZPTR=32", "-DF3DEX_GBI_2"]
    # Only the libultra headers: the decomp's own include/ directory has a math.h that would hide the C library's.
    incs = ["-I" + os.path.join(a.decomp, "lib", "ultralib", "include"),
            "-I" + os.path.join(ROOT, "src", "render"), "-I" + os.path.join(ROOT, "src", "platform")]
    srcs = [os.path.join(HERE, "rdp_gbi_check.c"), os.path.join(ROOT, "src", "render", "f3dex2.c"), os.path.join(ROOT, "src", "render", "rdp.c")]
    objs = []
    for s in srcs:
        o = os.path.join(work, os.path.basename(s) + ".o")
        subprocess.run(["clang", "-c"] + flags + incs + ["-o", o, s], check=True)
        objs.append(o)
    exe = os.path.join(work, "rdp_gbi_check")
    # Same recipe as the game's own link, but without --image-base (this program does not need the low-address layout).
    cmd = targets.link_command("linux32", a.sysroot, objs, exe)
    cmd = [c for c in cmd if not c.startswith("-Wl,--image-base")]
    subprocess.run(cmd, check=True)
    rc = subprocess.run([exe]).returncode
    # The window scaler is plain C without game addresses; it is checked with the same flags and link recipe.
    sobjs = []
    for s in (os.path.join(HERE, "scale_check.c"), os.path.join(ROOT, "src", "render", "scale.c")):
        o = os.path.join(work, os.path.basename(s) + ".o")
        subprocess.run(["clang", "-c"] + flags + incs + ["-o", o, s], check=True)
        sobjs.append(o)
    sexe = os.path.join(work, "scale_check")
    cmd = [c for c in targets.link_command("linux32", a.sysroot, sobjs, sexe) if not c.startswith("-Wl,--image-base")]
    subprocess.run(cmd, check=True)
    rc2 = subprocess.run([sexe]).returncode
    return rc or rc2


if __name__ == "__main__":
    sys.exit(main())
