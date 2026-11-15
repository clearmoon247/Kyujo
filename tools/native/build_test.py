#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Builds and (for linux32) runs one native self-test from tests/native/.

    build_test.py --decomp ext/decomp --build-dir build/linux32 --profile linux32 --sysroot work/sysroot/musl-i386 --test sched [--no-run]

A test is two files: tests/native/<name>_test_game.c (GAME side: libultra headers) and tests/native/<name>_test_host.c (HOST side: C library,
contains main). The test is linked with the pieces build_native.py already produced in --build-dir: the native libultra archive,
src/ultra (the replacement assembly), and the host platform objects. Anything libultra still needs from the (unfinished) hardware
layer becomes a loud stub, exactly as in the full build, so a test that reaches unimplemented code stops and names it.

The exit status of this script is the test program's exit status (0 = all checks passed).
"""
import argparse
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import compile_decomp  # noqa: E402
import proc  # noqa: E402
import targets         # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("FAILED: %s\n  command: %s\n%s" % (what, " ".join(cmd), r.stderr[-4000:]))
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decomp", required=True, help="the decomp checkout (the stub generator reads its ROM layout files)")
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--profile", required=True, choices=sorted(targets.TARGETS))
    ap.add_argument("--sysroot", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--no-run", action="store_true", help="only build (use for the Windows profile: it cannot run here)")
    args = ap.parse_args()

    t = targets.TARGETS[args.profile]
    b = args.build_dir
    tree = os.path.join(b, "tree")
    out = os.path.join(b, "tests", args.test)
    obj_dir = os.path.join(out, "obj")
    os.makedirs(obj_dir, exist_ok=True)

    # GAME-side test file: same flags as src/ultra (libultra headers + the platform interface).
    lib_flags = t["cflags"] + ["-I" + os.path.join(tree, "lib", "ultralib", "src"), "-I" + os.path.join(tree, "lib", "ultralib", "src", "os"),
                               "-I" + os.path.join(REPO, "src", "platform"), "-Wall"]
    game_src = os.path.join(REPO, "tests", "native", args.test + "_test_game.c")
    res = compile_decomp.compile_one(tree, t["triple"], "O0", obj_dir, game_src, lib_flags, lib=True, force=True)
    if not res["ok"]:
        sys.exit("test (game side) failed to compile: " + str(res["first_error"]))
    # HOST-side test file.
    host_flags = targets.host_include_flags(args.profile, args.sysroot) + t["cflags"] + \
        ["-std=gnu11", "-Wall", "-Wextra", "-g", "-O0", "-I" + os.path.join(REPO, "src", "platform")]
    run(["clang"] + host_flags + ["-c", os.path.join(REPO, "tests", "native", args.test + "_test_host.c"), "-o",
                                  os.path.join(obj_dir, args.test + "_test_host.o")], "compile host side of the test")
    # Reuse the already built replacement and platform objects (not host_main.o: the test has its own main).
    reuse = glob.glob(os.path.join(b, "ultra", "*.o")) + [os.path.join(b, "host", n) for n in ("host_sched.o", "missing.o", "host_rom.o", "host_save.o", "host_be.o")]
    for o in reuse:
        if not os.path.exists(o):
            sys.exit("missing %s: run build_native.py for this profile first" % o)
    archive = os.path.join(b, "libultra", "libultra.a")

    # Loud stubs for whatever the pulled-in libultra members still need.
    stage = os.path.join(out, "stage")
    os.makedirs(stage, exist_ok=True)
    for f in glob.glob(os.path.join(stage, "*.o")):
        os.remove(f)
    for o in glob.glob(os.path.join(obj_dir, "*.o")) + reuse:
        subprocess.run(["cp", o, stage], check=True)
    gen = os.path.join(out, "gen")
    r = run(proc.python_cmd() + [os.path.join(HERE, "gen_platform_symbols.py"), "--decomp", args.decomp, "--profile", args.profile,
             "--sysroot", args.sysroot, "--objs", stage, "--archives", archive, "--out", gen], "generate stubs for the test")
    run(["clang", "--target=" + t["triple"], "-c", os.path.join(gen, "abs_symbols.S"), "-o", os.path.join(gen, "abs_symbols.o")], "assemble")
    run(["clang", "--target=" + t["triple"], "-nostdinc", "-isystem", targets.clang_resource_include(), "-fno-builtin", "-fno-pic", "-fno-pie",
         "-c", os.path.join(gen, "stubs.c"), "-o", os.path.join(gen, "stubs.o")], "compile stubs")
    program = os.path.join(out, args.test + t["program"][len("kyujo"):])
    objs = sorted(glob.glob(os.path.join(stage, "*.o"))) + [os.path.join(gen, "abs_symbols.o"), os.path.join(gen, "stubs.o"), archive]
    run(targets.link_command(args.profile, args.sysroot, objs, program), "link the test")
    print("built " + program)
    if args.no_run:
        return 0
    return subprocess.run([program]).returncode


if __name__ == "__main__":
    sys.exit(main())
