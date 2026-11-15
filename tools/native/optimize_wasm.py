#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Dan B. Cooper
"""Optimises the game's WebAssembly module with binaryen's wasm-opt (Apache-2.0; build it from github.com/WebAssembly/binaryen).

    optimize_wasm.py --in bw/kyujo.wasm --out bw/kyujo.opt.wasm --wasm-opt binaryen-build/bin/wasm-opt --wabt wabt-checkout

Why this and not a compiler optimisation of the game code: the game code is generated at -O0 on purpose (the big-endian memory rewrite works on unoptimised IR,
and a global-changing optimiser shrank a variable, see docs/switch-android-plan.md section 10). wasm-opt works on the finished module, whose behaviour is defined
by the WebAssembly specification, so it cannot make the C code's undefined behaviour worse. Measured 2026-11-08 (x86-64, GPU renderer on llvmpipe): the module
shrinks from 32 MB to 10.6 MB, the generated C from 348 MB to 155 MB (compile time of the shell halves), and every picture of a 20 s run and of a 30 s scripted
menu run is byte-identical to the unoptimised build's. CPU time did not change on x86-64 (the game's own code is not what the machine is busy with).
The features are the ones the module declares (target_features section). The result is validated with wabt's wasm-validate.
Standard library only."""
import argparse
import os
import subprocess
import sys

FEATURES = ["--enable-bulk-memory", "--enable-extended-const", "--enable-multivalue", "--enable-mutable-globals",
            "--enable-nontrapping-float-to-int", "--enable-sign-ext"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--wasm-opt", required=True)
    ap.add_argument("--wabt", required=True, help="wabt checkout whose build/ holds wasm-validate")
    ap.add_argument("--level", default="O2")
    ap.add_argument("--function-map", help="write the map from the optimised module's function indexes (the fNNN of the generated C) to the game's function names here")
    a = ap.parse_args()
    r = subprocess.run([a.wasm_opt, a.inp] + FEATURES + ["-" + a.level] + (["--print-function-map"] if a.function_map else []) + ["-o", a.out], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("wasm-opt failed:\n" + (r.stderr or r.stdout)[-3000:])
    if a.function_map:
        open(a.function_map, "w").write(r.stdout)
    v = subprocess.run([os.path.join(a.wabt, "build", "wasm-validate"), "--enable-all", a.out], capture_output=True, text=True)
    if v.returncode != 0:
        sys.exit("the optimised module does not validate:\n" + (v.stderr or v.stdout)[-3000:])
    print("optimised %s: %d -> %d bytes" % (a.out, os.path.getsize(a.inp), os.path.getsize(a.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
